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

// Terrain density = D.1's REAL baked mesh (src/demo/assets/terrain/
// terrain_grid_vtx.h): 128 mesh-cells (N64 texture-window tiles), each a 3x3
// vertex patch (9 verts) triangulated into 8 tris by the shared per-tile index
// pattern (terrain_grid_idx.h), expanded with a +tile*9 offset. TILE-MAJOR, so
// a vertex's mesh-cell index = vert_index / 9. The mesh is flat-shaded with a
// DISTINCT debug color per mesh-cell (overriding the N64 pre-lit gray) so
// transform/cull/winding bugs stay catchable — a uniform-gray blob would blind
// the orientation check that caught the C1 regressions.
//
// Pool sizing (post-G1): geom transforms each LOAD_VERTS window ONCE into the
// pool and the all-inside fast path bins shared indices (zero new tverts), so
// peak pool occupancy is ~terrain_verts + tree_verts (+ near-clip fans, none on
// the off-near-plane scripted path), NOT tris*3. ~1152 + 504 = ~1656 tverts,
// well under RDR_MAX_TVERTS — the T0 peak-tvert probe drives the cap drop.
enum {
  DEMO_TERRAIN_TILES = 128,
  DEMO_TERRAIN_VERTS_PER_TILE = 9,
  DEMO_TERRAIN_TRIS_PER_TILE = 8,
  DEMO_TERRAIN_VERTS =
      DEMO_TERRAIN_TILES * DEMO_TERRAIN_VERTS_PER_TILE,                 // 1152
  DEMO_TERRAIN_TRIS = DEMO_TERRAIN_TILES * DEMO_TERRAIN_TRIS_PER_TILE,  // 1024
  DEMO_TERRAIN_IDX = DEMO_TERRAIN_TRIS * 3,                             // 3072
  // Tree billboard placeholders at the REAL scenery density (126 instances,
  // terrain_scenery.h): static upright debug-colored quads (2 tris / 4 verts).
  // True per-frame camera-facing billboarding needs float -> deferred; the
  // static quad keeps the per-frame path float-free. Bumped to real density so
  // the peak-tvert probe reflects the actual scene load.
  DEMO_TREE_COUNT = 126,
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

// ---- terrain geometry boundary (T0 swap seam, realized) ---------------------
// Fill caller-provided arrays with D.1's REAL baked terrain mesh: positions +
// UVs are copied verbatim from g_terrain_grid_vtx (flash-const), and the N64
// pre-lit gray is overridden with a DISTINCT debug color per mesh-cell (tile =
// vert/9, tile-major) so transform/cull/winding bugs are catchable. Indices are
// the shared per-tile pattern expanded over all tiles (pattern[k] + tile*9).
// The rest of demo_terrain_build is geometry-agnostic, so this boundary is the
// only mesh-specific code.
//   verts: out, capacity >= DEMO_TERRAIN_VERTS
//   idx:   out, capacity >= DEMO_TERRAIN_IDX
// Returns the vertex count written (DEMO_TERRAIN_VERTS).
uint32_t demo_terrain_geometry(struct Vtx* verts, uint16_t* idx);

// Fill caller-provided arrays with the placeholder tree billboard quads. UVs
// map the 32x64 tree0 sprite upright (corners bl/br/tl/tr -> the sprite's
// bottom/bottom/top/top edges, S10.5 = texel*32); the per-vertex color is a
// gray Gouraud gradient (top 206, bottom 157) exercising the Gouraud interp.
// Returns the vertex count.
uint32_t demo_tree_geometry(struct Vtx* verts, uint16_t* idx);

// ---- material fillers (single source of truth for the bound RenderStates) ---
// Fill *out with the EXACT RenderState demo_terrain_build binds before the
// terrain draw: the gutter'd 512x512 RGBA5551 atlas (REPEAT/POINT) with the
// P4-2 ENV combiner (TEXEL0 x ENV, COMBINE_CUSTOM, c=CC_ENVIRONMENT) and a
// near-neutral light-sage ENV tint. Opaque, CULL_BACK, pre-lit. Tests assert
// the bound material via these; the build uses the SAME fillers (no duplicated
// constants).
void demo_terrain_material(struct RenderState* out);

// Fill *out with the EXACT RenderState demo_terrain_build binds before the
// tree CUTOUT (opaque) pass: the 32x64 RGBA5551 tree0 sprite (CLAMP/POINT) with
// COMBINE_MODULATE (TEXEL x SHADE), double-sided (CULL_NONE, N6 billboard),
// ZMODE_OPAQUE + alpha_cmp != 0 (G_RM_AA_ZB_TEX_EDGE: alpha-test the 1-bit
// 5551 alpha so the transparent billboard background is discarded). Pre-lit.
// (The faithful XLU translucent pass is DEFERRED to T4 — it is visually
// identical to the cutout at 1-bit alpha / no-AA, and the double-draw overflows
// the shared bin-ref pool; its material filler lands with the AA pass.)
void demo_tree_material(struct RenderState* out);

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
