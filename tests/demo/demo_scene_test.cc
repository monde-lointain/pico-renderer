// demo_scene_test.cc — C1 demo-scene host guard. gtest.
//
// REGRESSION GUARD: the C1 demo shipped a black screen to the device — the
// camera sat behind the geometry (view translate -18 with a +z-forward clip
// convention), so every source triangle hit the near-reject (clip.w<=0) and
// tris=0 dropped=16. It escaped because the 14 rdr goldens use their OWN
// matrices; the demo's actual build_scene was never exercised on host. This
// test drives the REAL demo scene (demo_scene_build) through the full rdr
// pipeline and asserts the scene actually renders. Deterministic (angle 0).
#include "demo/demo_scene.h"

#include <stdint.h>
#include <string.h>

#include "demo/camera_path_gen.h"  // committed scripted V*P table (float-free)
#include "demo/debug_terrain_gen.h"  // committed FLASH-CONST placeholder geometry
#include "gfx/framebuffer.h"
#include "gtest/gtest.h"
#include "platform/platform.h"  // enum Button (camera trace)
#include "rdr/config.h"
#include "rdr/frame.h"
#include "rdr/rdr.h"

// Frame is large (pool + bins) -> BSS, not the stack.
static struct Frame g_frame;
static struct Framebuffer g_fb;

// Count framebuffer pixels that differ from the scene's CLEAR color.
static int count_non_clear(const uint16_t* fb, int n, uint16_t clear) {
  int drawn = 0;
  for (int i = 0; i < n; ++i) {
    if (fb[i] != clear) {
      ++drawn;
    }
  }
  return drawn;
}

// Unpack RGB565 -> 8-bit channels (matches gfx/framebuffer.h rgb565()).
static void unpack565(uint16_t c, int* r, int* g, int* b) {
  *r = ((c >> 11) & 0x1F) << 3;
  *g = ((c >> 5) & 0x3F) << 2;
  *b = (c & 0x1F) << 3;
}

// Tally pixels matching the cube's distinct face colors over the whole frame.
// The flat face colors are well separated, so a simple per-pixel bucket pins
// which faces are visible -> which side the camera is on. Front (+Z) = RED;
// the away-side faces the wrong-side camera would reveal are top (+Y) = GREEN
// and bottom (-Y) = ORANGE.
struct FaceTally {
  long red;     // cube +Z front (and pyramid apex)
  long green;   // cube +Y top
  long orange;  // cube -Y bottom
};
static void tally_faces(const uint16_t* fb, int n, uint16_t clear,
                        struct FaceTally* t) {
  t->red = 0;
  t->green = 0;
  t->orange = 0;
  for (int i = 0; i < n; ++i) {
    uint16_t const px = fb[i];
    if (px == clear) {
      continue;
    }
    int r;
    int g;
    int b;
    unpack565(px, &r, &g, &b);
    if (r > 180 && g < 60 && b < 60) {
      ++t->red;
    } else if (r < 60 && g > 180 && b < 60) {
      ++t->green;
    } else if (r > 180 && g > 60 && g < 180 && b < 60) {
      ++t->orange;
    }
  }
}

TEST(DemoSceneGuard, SceneRendersTrisAndPixels) {
  struct Command cmds[DEMO_CMD_CAP];
  uint32_t const n = demo_scene_build(cmds, DEMO_CMD_CAP, 0.0F, 0.0F);
  ASSERT_GT(n, 0U);
  ASSERT_LE(n, (uint32_t)DEMO_CMD_CAP);
  // Last command must be END-terminated. Index via a clamped local so it is
  // provably in [0, DEMO_CMD_CAP) for the static analyzer.
  uint32_t const last = (n - 1) % (uint32_t)DEMO_CMD_CAP;
  ASSERT_EQ(cmds[last].op, (uint8_t)CMD_END);

  g_frame.fb = g_fb.px;
  g_frame.width = RDR_SCREEN_W;
  g_frame.height = RDR_SCREEN_H;
  ASSERT_EQ(rdr_begin_frame(&g_frame), RDR_OK);
  ASSERT_EQ(rdr_submit(&g_frame, cmds), RDR_OK);
  ASSERT_EQ(rdr_end_frame(&g_frame), RDR_OK);

  // The regression that bit us: tris_total must be > 0. The scene is 16 source
  // tris (4 pyramid + 12 cube); back-face cull legitimately removes those
  // facing away, so assert a sensible lower bound, not all 16.
  EXPECT_GE(g_frame.geom.tris_total, 4U)
      << "tris_total=" << g_frame.geom.tris_total
      << " dropped=" << g_frame.geom.tris_dropped
      << " (black-screen regression: camera behind geometry?)";

  // The scene must actually rasterize, not just bin: at least one framebuffer
  // pixel differs from the CLEAR color.
  uint16_t const clear = rgb565(8, 8, 24);
  int const drawn =
      count_non_clear(g_frame.fb, RDR_SCREEN_W * RDR_SCREEN_H, clear);
  EXPECT_GT(drawn, 0) << "no non-clear pixels — scene binned but did not draw";

  // CAMERA-SIDE / CULL-SENSE GUARD (the wrong-side regression):
  // at angle 0 gldemo's camera (eye +Z, real lookAt) sees the cube FRONT (+Z,
  // RED) head-on and NOT its top (+Y, green) or bottom (-Y, orange) faces. A
  // wrong-side camera (the bug: a bare +Z translate that drops the lookAt
  // rotation -> 180deg orbit) reveals those away-faces — measured green~2160 /
  // orange~2160 px on the wrong build vs 0 on the fixed build. So: RED must
  // dominate, and the away-side green+orange must be negligible. This FAILS on
  // the wrong-side build (green/orange present) and PASSES after the lookAt.
  struct FaceTally t;
  tally_faces(g_frame.fb, RDR_SCREEN_W * RDR_SCREEN_H, clear, &t);
  EXPECT_GT(t.red, 4000) << "cube front (+Z red) not dominant — red=" << t.red;
  // The away-faces (top green / bottom orange) must be nearly absent; allow a
  // tiny margin for edge/AA pixels but reject the wrong-side build's ~2160
  // each.
  EXPECT_LT(t.green + t.orange, 400)
      << "wrong-side camera / inverted cull: away-faces visible — green="
      << t.green << " orange=" << t.orange << " (red=" << t.red << ")";
}

// ===========================================================================
// Wave-D.7 terrain scene + scripted-camera guards
// ===========================================================================

// Coarse hue bucket of a non-clear RGB565 pixel. The terrain debug palette
// assigns a DISTINCT flat color per mesh cell; a correct render shows MANY
// distinct buckets (not one blob), so counting populated buckets pins that the
// per-cell coloring + transform + winding all reached the framebuffer.
static int hue_bucket(uint16_t px) {
  int r;
  int g;
  int b;
  unpack565(px, &r, &g, &b);
  // 3-bit-per-channel coarse quantization -> 0..511 bucket id.
  int const rq = r >> 5;
  int const gq = g >> 5;
  int const bq = b >> 5;
  return (rq << 6) | (gq << 3) | bq;
}

// Drive a freshly built terrain frame through the full rdr pipeline.
static void render_terrain(const struct DemoCamera* cam,
                           const struct DemoScroll* scroll,
                           struct DemoTelemetry* telem) {
  struct Command cmds[DEMO_TERRAIN_CMD_CAP];
  uint32_t const n =
      demo_terrain_build(cmds, DEMO_TERRAIN_CMD_CAP, cam, scroll, telem);
  // Plain control-flow bound (not gtest ASSERT, which the static analyzer does
  // not model) so the direct cmds[n-1] index below is provably in range.
  if (n < 1U || n > (uint32_t)DEMO_TERRAIN_CMD_CAP) {
    FAIL() << "demo_terrain_build returned out-of-range n=" << n;
    return;
  }
  ASSERT_EQ(cmds[n - 1].op, (uint8_t)CMD_END);

  g_frame.fb = g_fb.px;
  g_frame.width = RDR_SCREEN_W;
  g_frame.height = RDR_SCREEN_H;
  ASSERT_EQ(rdr_begin_frame(&g_frame), RDR_OK);
  ASSERT_EQ(rdr_submit(&g_frame, cmds), RDR_OK);
  ASSERT_EQ(rdr_end_frame(&g_frame), RDR_OK);
}

// TERRAIN-SCENE GUARD: real-density geometry renders, with the debug-color
// histogram proving distinct per-cell colors reach the framebuffer (catches the
// transform/cull/winding bugs a uniform-gray blob would hide).
TEST(TerrainSceneGuard, RealDensityRendersDistinctDebugCells) {
  struct DemoCamera cam;
  struct DemoScroll scroll;
  struct DemoTelemetry telem;
  demo_camera_init(&cam);
  demo_scroll_init(&scroll);

  render_terrain(&cam, &scroll, &telem);

  // Real density: the placeholder grid is DEMO_TERRAIN_TRIS source tris. Most
  // survive (the camera looks down onto an up-facing field), so assert a
  // substantial accepted count — a regression that flips winding/cull or puts
  // the field behind the camera collapses this toward 0.
  EXPECT_GE(g_frame.geom.tris_total, (uint32_t)(DEMO_TERRAIN_TRIS / 2))
      << "tris_total=" << g_frame.geom.tris_total
      << " dropped=" << g_frame.geom.tris_dropped << " (terrain not visible?)";

  // The cmdgen telemetry must report the real source-tri budget (front-end
  // scene-build work), distinct from the geom-accepted count above.
  EXPECT_EQ(telem.cmdgen_tris, (uint32_t)(DEMO_TERRAIN_TRIS + DEMO_TREE_TRIS));
  EXPECT_GT(telem.cmdgen_draws, 0U);
  EXPECT_EQ(telem.cmdgen_verts,
            (uint32_t)(DEMO_TERRAIN_VERTS + DEMO_TREE_VERTS));

  // The scene must rasterize.
  uint16_t const clear = g_frame.clear_color;
  int const drawn =
      count_non_clear(g_frame.fb, RDR_SCREEN_W * RDR_SCREEN_H, clear);
  EXPECT_GT(drawn, 1000) << "terrain binned but barely drew — drawn=" << drawn;

  // DEBUG-COLOR HISTOGRAM: count distinct coarse hue buckets among non-clear
  // pixels. A correct per-cell-colored render lights up many buckets; a blinded
  // uniform-gray render (or a single-color bug) would light up ~1.
  unsigned char seen[512];
  memset(seen, 0, sizeof seen);
  int distinct = 0;
  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    uint16_t const px = g_frame.fb[i];
    if (px == clear) {
      continue;
    }
    int const b = hue_bucket(px);
    if (!seen[b]) {
      seen[b] = 1;
      ++distinct;
    }
  }
  EXPECT_GE(distinct, 8)
      << "too few distinct debug colors — one-blob render? distinct="
      << distinct;
}

// FIXED-DELTA SCROLL DETERMINISM: phase advances by exactly DEMO_SCROLL_DELTA
// per frame and wraps exactly at the power-of-two period (integer-only).
TEST(TerrainSceneGuard, ScrollAdvancesByFixedIntegerDelta) {
  struct DemoScroll s;
  demo_scroll_init(&s);
  EXPECT_EQ(s.phase, 0U);
  for (int k = 1; k <= 4000; ++k) {
    demo_scroll_advance(&s);
    EXPECT_EQ(s.phase, (uint32_t)((k * DEMO_SCROLL_DELTA) % DEMO_SCROLL_PERIOD))
        << "scroll diverged at frame " << k;
  }
}

// NO-FLOAT / REPRODUCIBLE per-frame path: replaying the SAME integer button
// trace from the SAME init state must yield a BIT-IDENTICAL camera + scroll
// state. A float in the per-frame path (rounding/order nondeterminism) would
// break exact reproduction; this is the determinism anchor fb_crc relies on.
TEST(TerrainSceneGuard, PerFramePathIsBitReproducible) {
  // A deterministic pseudo-button trace (held + edges), purely integer.
  struct DemoCamera a;
  struct DemoCamera b;
  struct DemoScroll sa;
  struct DemoScroll sb;
  demo_camera_init(&a);
  demo_camera_init(&b);
  demo_scroll_init(&sa);
  demo_scroll_init(&sb);

  for (int pass = 0; pass < 2; ++pass) {
    struct DemoCamera* cam = (pass == 0) ? &a : &b;
    struct DemoScroll* sc = (pass == 0) ? &sa : &sb;
    demo_camera_init(cam);
    demo_scroll_init(sc);
    for (int f = 0; f < 600; ++f) {
      // Toggle to free-fly at frame 200, back at 400; jiggle the D-pad.
      uint32_t pressed = 0;
      if (f == 200 || f == 400) {
        pressed |= (uint32_t)BTN_A;
      }
      uint32_t held = 0;
      if ((f / 7) % 2 == 0) {
        held |= (uint32_t)BTN_UP;
      }
      if ((f / 11) % 3 == 0) {
        held |= (uint32_t)BTN_LEFT;
      }
      demo_camera_advance(cam, held, pressed);
      demo_scroll_advance(sc);
    }
  }
  // Bit-identical reproduction, compared member-wise (the POD carries padding
  // bytes, so a raw memcmp is undefined — compare the meaningful state).
  EXPECT_EQ(a.mode, b.mode);
  EXPECT_EQ(a.frame, b.frame);
  EXPECT_EQ(a.yaw_q16, b.yaw_q16);
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(a.eye[i], b.eye[i])
        << "camera eye[" << i << "] not bit-reproducible (float crept in?)";
    EXPECT_EQ(a.look[i], b.look[i])
        << "camera look[" << i << "] not bit-reproducible";
  }
  EXPECT_EQ(sa.phase, sb.phase) << "scroll per-frame path is not reproducible";
}

// SCRIPTED-PATH NEAR-PLANE MARGIN: every keyframe-loop pose must keep the whole
// terrain in front of the camera (positive clip.w) so the thin-slice near-clip
// is not needed at T0. Proxy: across a full loop, tris_total never collapses to
// near-zero (a pose that crossed the near plane would near-reject the field).
TEST(TerrainSceneGuard, ScriptedPathStaysOffNearPlane) {
  struct DemoCamera cam;
  struct DemoScroll scroll;
  struct DemoTelemetry telem;
  demo_camera_init(&cam);
  demo_scroll_init(&scroll);

  // The committed table IS the rendered path now; cover its whole loop.
  uint32_t const total_frames = (uint32_t)SCRIPTED_FRAME_COUNT;
  // Sample the loop at a stride so the test stays fast but covers each segment.
  for (uint32_t f = 0; f < total_frames; f += 15) {
    render_terrain(&cam, &scroll, &telem);
    EXPECT_GE(g_frame.geom.tris_total, (uint32_t)(DEMO_TERRAIN_TRIS / 4))
        << "near-plane crossing at scripted frame " << cam.frame
        << ": tris_total=" << g_frame.geom.tris_total << " collapsed";
    for (int k = 0; k < 15; ++k) {
      demo_camera_advance(&cam, 0, 0);
      demo_scroll_advance(&scroll);
    }
  }
}

// Locate the PROJECTION-target SET_MATRIX command in a built stream; returns
// its matrix pointer, or null if absent.
static const struct Mat4fx* find_proj_matrix(const struct Command* cmds,
                                             uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) {
    if (cmds[i].op == (uint8_t)CMD_SET_MATRIX &&
        cmds[i].u.set_matrix.target == (uint8_t)MTX_PROJECTION) {
      return cmds[i].u.set_matrix.mat;
    }
  }
  return nullptr;
}

// SCRIPTED PER-FRAME PATH IS FLOAT-FREE (Q5/TW-05): the V*P the scripted scene
// submits must come ONLY from the committed Q16.16 table g_scripted_mvp — i.e.
// it must bit-equal g_scripted_mvp[frame % SCRIPTED_FRAME_COUNT]. A regression
// that reintroduces a per-frame float bake (or a wrong index) breaks this. The
// committed bytes are identical host and device, so this also pins the
// bit-identical fb_crc invariant the Lead decision is about.
TEST(TerrainSceneGuard, ScriptedMvpComesOnlyFromCommittedTable) {
  struct DemoCamera cam;
  struct DemoScroll scroll;
  struct DemoTelemetry telem;
  demo_camera_init(&cam);
  demo_scroll_init(&scroll);
  ASSERT_EQ(cam.mode, (uint8_t)DEMO_CAM_SCRIPTED);

  // Probe several frames spread across the loop (incl. a wrap past the end).
  uint32_t const probes[6] = {0,   1,   119,
                              240, 479, (uint32_t)SCRIPTED_FRAME_COUNT + 7};
  for (int p = 0; p < 6; ++p) {
    // Advance the integer frame counter to the probe frame.
    while (cam.frame < probes[p]) {
      demo_camera_advance(&cam, 0, 0);
    }
    struct Command cmds[DEMO_TERRAIN_CMD_CAP];
    uint32_t const n =
        demo_terrain_build(cmds, DEMO_TERRAIN_CMD_CAP, &cam, &scroll, &telem);
    const struct Mat4fx* vp = find_proj_matrix(cmds, n);
    ASSERT_NE(vp, nullptr) << "no PROJECTION matrix in scripted stream";
    uint32_t const idx = cam.frame % (uint32_t)SCRIPTED_FRAME_COUNT;
    for (int e = 0; e < 16; ++e) {
      EXPECT_EQ(vp->m[e], g_scripted_mvp[idx][e])
          << "scripted V*P[" << e << "] != committed table at frame "
          << cam.frame << " (a float bake re-crept in?)";
    }
  }
}

// FLASH-CONST GEOMETRY MATCHES THE FILL API (integration-gate RAM fix): the
// committed const arrays (g_debug_*) the device demo submits MUST be byte-
// identical to what the runtime fill functions (demo_terrain_geometry /
// demo_tree_geometry) produce. This is the closed-loop guard: if someone edits
// the placeholder fill math without regenerating debug_terrain_gen.h (via
// gen_debug_terrain.py), this fails — keeping the committed flash geometry
// honest. (The fill API is retained for host tests + the T0 D.1 swap seam; the
// device demo reads the flash arrays so the input geometry stays out of .bss.)
// Field-wise vertex compare (NOT memcmp — Vtx has a union, so its object
// representation is not unique and a raw memcmp is undefined / tidy-flagged).
static int vtx_eq(const struct Vtx* a, const struct Vtx* b) {
  for (int i = 0; i < 3; ++i) {
    if (a->pos[i] != b->pos[i]) {
      return 0;
    }
  }
  for (int i = 0; i < 2; ++i) {
    if (a->uv[i] != b->uv[i]) {
      return 0;
    }
  }
  for (int i = 0; i < 4; ++i) {
    if (a->c.rgba[i] != b->c.rgba[i]) {
      return 0;
    }
  }
  return 1;
}

TEST(TerrainSceneGuard, FlashConstGeometryMatchesFillApi) {
  static struct Vtx tv[DEMO_TERRAIN_VERTS];
  static uint16_t ti[DEMO_TERRAIN_IDX];
  static struct Vtx rv[DEMO_TREE_VERTS];
  static uint16_t ri[DEMO_TREE_IDX];
  uint32_t const nv = demo_terrain_geometry(tv, ti);
  uint32_t const nrv = demo_tree_geometry(rv, ri);

  // Counts agree with the committed const-array sizes.
  uint32_t const gv =
      (uint32_t)(sizeof g_debug_terrain_vtx / sizeof g_debug_terrain_vtx[0]);
  uint32_t const grv =
      (uint32_t)(sizeof g_debug_tree_vtx / sizeof g_debug_tree_vtx[0]);
  ASSERT_EQ(nv, gv);
  ASSERT_EQ(nrv, grv);

  // Positions/colors byte-identical (field-wise; covers winding via indices).
  for (uint32_t i = 0; i < nv; ++i) {
    EXPECT_TRUE(vtx_eq(&tv[i], &g_debug_terrain_vtx[i]))
        << "terrain vert " << i << " drifted from gen_debug_terrain.py";
  }
  for (uint32_t i = 0; i < nrv; ++i) {
    EXPECT_TRUE(vtx_eq(&rv[i], &g_debug_tree_vtx[i]))
        << "tree vert " << i << " drifted — regenerate debug_terrain_gen.h";
  }
  // Index arrays are plain uint16 (no union/padding) — memcmp is fine.
  EXPECT_EQ(memcmp(ti, g_debug_terrain_idx, sizeof ti), 0)
      << "terrain indices drifted — regenerate debug_terrain_gen.h";
  EXPECT_EQ(memcmp(ri, g_debug_tree_idx, sizeof ri), 0)
      << "tree indices drifted — regenerate debug_terrain_gen.h";
}
