// demo/demo_scene.h — testable scene builder for the renderer demo. Orthodox
// C++.
//
// Factored OUT of main()'s for(;;) loop so the ACTUAL demo scene can be driven
// through the rdr pipeline by a host guard test — the C1 black-screen
// regression (camera behind the geometry -> every tri near-rejected) escaped
// precisely because the demo's own build path was never exercised on host. The
// builder fills a caller-provided Command buffer (END-terminated); matrices the
// stream references are held in module-local storage that outlives the returned
// stream until the next build call.
//
// TWO scenes share this module:
//   * demo_scene_build         — the C1 gldemo flat pyramid + cube (retained as
//                                a regression anchor; do not remove).
//   * demo_terrain_*           — Wave-D.7: a flat-shaded real-density terrain
//                                grid + tree billboards with a deterministic
//                                scripted camera (the T0 "real-density geometry
//                                + deterministic camera" milestone, app side).
//
// DETERMINISM (critical): the scripted camera path and the panorama/cloud
// scroll advance by FIXED per-frame INTEGER deltas with NO float in the
// per-frame path, so fb_crc reproduces and host==device. Floats appear ONLY in
// one-time matrix setup (the view/MVP bake), which is a pure function of the
// integer pose. See demo_camera_advance / demo_terrain_build.
#ifndef DEMO_SCENE_H
#define DEMO_SCENE_H

#include "rdr/types.h"

// ===========================================================================
// C1 gldemo scene (retained regression anchor)
// ===========================================================================

// Recommended command-buffer capacity for the gldemo scene (pyramid + cube).
enum { DEMO_CMD_CAP = 64 };

// Build the flat gldemo scene into `buf` (capacity `cap` Commands) at the given
// pyramid/cube rotation angles (degrees). The stream is END-terminated. Returns
// the command count written (0 if cap is too small). The referenced matrices
// live in module storage valid until the next demo_scene_build call.
uint32_t demo_scene_build(struct Command* buf, uint32_t cap, float pyr_angle,
                          float cube_angle);

// ===========================================================================
// Wave-D.7 terrain scene (real-density geometry + deterministic camera)
// ===========================================================================

// Global pre-scale (S0 LOCKED): N64 world coords are multiplied by this so all
// transformed coords stay inside the Q16.16 integer range (|v| < 2^15). Applied
// once during the (float) matrix/geometry setup, never in the per-frame path.
#define DEMO_SCENE_SCALE 0.005

// Terrain placeholder density. A vertex grid of (COLS+1) x (ROWS+1) verts,
// every quad split into 2 tris -> COLS*ROWS*2 tris. Sized so the transformed-
// vertex pool (RDR_MAX_TVERTS=3000) is not exhausted: each tri emits 3 tverts
// (no vertex sharing in the thin slice), so terrain_tris*3 + tree_tris*3 must
// stay under RDR_MAX_TVERTS. 30x15 = 900 tris -> 2700 tverts, leaving headroom
// for the tree billboards. Representative of the N64 33x17 / 9-vert-8-tri
// layout. (The REAL baked mesh from D.1 replaces this at T0 via
// demo_terrain_geometry — keep the swap seam clean.)
enum {
  DEMO_TERRAIN_COLS = 30,
  DEMO_TERRAIN_ROWS = 15,
  DEMO_TERRAIN_QUADS = DEMO_TERRAIN_COLS * DEMO_TERRAIN_ROWS,
  DEMO_TERRAIN_TRIS = DEMO_TERRAIN_QUADS * 2,
  DEMO_TERRAIN_VERTS = (DEMO_TERRAIN_COLS + 1) * (DEMO_TERRAIN_ROWS + 1),
  DEMO_TERRAIN_IDX = DEMO_TERRAIN_TRIS * 3,
  // Tree billboard quads (2 tris / 4 verts each), distinct debug sprite color.
  DEMO_TREE_COUNT = 12,
  DEMO_TREE_VERTS = DEMO_TREE_COUNT * 4,
  DEMO_TREE_TRIS = DEMO_TREE_COUNT * 2,
  DEMO_TREE_IDX = DEMO_TREE_TRIS * 3
};

// Command-buffer capacity for the terrain scene. The scene is single-batch (one
// LOAD_VERTS + one DRAW_TRIS for the terrain, one pair for the trees) plus
// state, so a small cap suffices; the heavy work is per-vertex inside geom, not
// per-command. The "~255-draw front-end" framing refers to the GBI draw budget,
// surfaced as the cmdgen telemetry counter, not the Command count.
enum { DEMO_TERRAIN_CMD_CAP = 32 };

// ---- scripted camera (deterministic; FLOAT-FREE per-frame path) -------------
// Q5/TW-05 (Lead decision): the scripted path's V*P matrices are baked OFFLINE
// to a committed Q16.16 table (src/demo/camera_path_gen.h, g_scripted_mvp[N])
// by src/demo/gen_camera_path.py. The runtime scripted per-frame path is JUST
// an integer index into that table (g_scripted_mvp[frame %
// SCRIPTED_FRAME_COUNT]) — NO float, so host and device read the SAME committed
// bytes -> bit-identical fb_crc. The keyframe poses + interpolation schedule
// live in the generator; these enums define that schedule and (by construction)
// SCRIPTED_FRAME_COUNT == DEMO_CAM_KEYFRAMES * DEMO_CAM_FRAMES_PER_SEG. The C1
// camera lesson is pinned: the eye stays in front of the geometry, OFF the near
// plane for the whole loop (no near-clip needed yet). FREE-FLY is the
// sanctioned on-device-float exception (interactive, never fb_crc-compared).
enum {
  DEMO_CAM_KEYFRAMES = 4,        // closed loop (last connects back to first)
  DEMO_CAM_FRAMES_PER_SEG = 120  // fixed integer frames per keyframe segment
};

// Camera mode: deterministic scripted path (table) vs interactive free-fly.
enum DemoCamMode { DEMO_CAM_SCRIPTED = 0, DEMO_CAM_FREEFLY = 1 };

// Camera state. The SCRIPTED per-frame mutation is integer-only (`frame`
// advances by 1; rendering reads the committed table). eye/look/yaw are the
// FREE-FLY handoff state, mutated only by fixed integer deltas. No float
// members; nothing on the scripted path touches float.
struct DemoCamera {
  uint8_t mode;     // enum DemoCamMode
  uint32_t frame;   // scripted-path frame counter (advances by 1)
  fx16_16 eye[3];   // free-fly eye (Q16.16, pre-scaled world)
  fx16_16 look[3];  // free-fly look-at target (Q16.16)
  int32_t yaw_q16;  // free-fly heading reserve (Q16.16)
};

// Initialize the camera to the scripted N64 start pose (eye = the LOCKED N64
// initial eye (3210,-3330,7330) * DEMO_SCENE_SCALE, committed as Q16.16
// constants — no runtime float).
void demo_camera_init(struct DemoCamera* cam);

// Advance the camera one frame from `held`/`pressed` button bitmasks (enum
// Button). Per-frame path is integer-only:
//   * BTN_A edge (in `pressed`) toggles scripted <-> free-fly.
//   * scripted: frame++ (rendering then indexes the committed V*P table).
//   * free-fly: D-pad/face buttons move eye by fixed integer deltas.
void demo_camera_advance(struct DemoCamera* cam, uint32_t held,
                         uint32_t pressed);

// ---- panorama / cloud scroll (deterministic integer phase) ------------------
// A background scroll phase advanced by a fixed integer delta per frame. Used
// as a UV/offset later; here it is a determinism-anchored counter surfaced in
// telemetry. Wraps at DEMO_SCROLL_PERIOD (power-of-two so the wrap is exact).
enum { DEMO_SCROLL_PERIOD = 1024, DEMO_SCROLL_DELTA = 3 };
struct DemoScroll {
  uint32_t phase;  // [0, DEMO_SCROLL_PERIOD)
};
void demo_scroll_init(struct DemoScroll* s);
void demo_scroll_advance(struct DemoScroll* s);  // (phase+DELTA) % PERIOD

// ---- per-stage telemetry ----------------------------------------------------
// The scene-build / cmdgen stage is the single-core front-end work that emits
// the GBI draw stream (the ~255-draw budget): counted SEPARATELY from the geom
// transform/bin and the back-end raster (which Frame.geom already counts). POD;
// filled by demo_terrain_build.
struct DemoTelemetry {
  uint32_t cmdgen_commands;  // Commands emitted into the stream this frame
  uint32_t cmdgen_draws;     // DRAW_TRIS calls emitted (GBI draw budget proxy)
  uint32_t cmdgen_verts;     // model-space verts referenced by LOAD_VERTS
  uint32_t cmdgen_tris;      // source triangles submitted (pre-cull)
  uint32_t scroll_phase;     // panorama/cloud scroll phase this frame
};

// ---- terrain geometry boundary (T0 swap seam) -------------------------------
// Fill caller-provided arrays with the placeholder procedural terrain: a
// debug-colored height-field grid (distinct flat color per mesh cell so
// transform/cull/winding bugs are catchable — uniform N64 gray would render one
// blob). The provoking-vertex color encodes the cell index. T0 (Lead) replaces
// THIS function with D.1's real baked mesh; the rest of demo_terrain_build is
// geometry-agnostic, so the swap is local to this boundary.
//   verts: out, capacity >= DEMO_TERRAIN_VERTS
//   idx:   out, capacity >= DEMO_TERRAIN_IDX
// Returns the vertex count written (DEMO_TERRAIN_VERTS).
uint32_t demo_terrain_geometry(struct Vtx* verts, uint16_t* idx);

// Fill caller-provided arrays with the placeholder tree billboard quads (each a
// distinct debug sprite color by sprite-id). Returns the vertex count.
uint32_t demo_tree_geometry(struct Vtx* verts, uint16_t* idx);

// ---- terrain scene build ----------------------------------------------------
// Build the terrain scene into `buf` (capacity `cap` Commands) for the given
// camera pose. The stream is END-terminated. Returns the command count (0 if
// cap is too small). Fills *telem with this frame's cmdgen counters. The
// referenced matrices/geometry live in module storage valid until the next
// demo_terrain_build call. NO float in the camera input — `cam` carries the
// integer pose; the float MVP bake from it is one-time setup, here.
uint32_t demo_terrain_build(struct Command* buf, uint32_t cap,
                            const struct DemoCamera* cam,
                            const struct DemoScroll* scroll,
                            struct DemoTelemetry* telem);

#endif  // DEMO_SCENE_H
