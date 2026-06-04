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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "demo/camera_path_gen.h"  // committed scripted V*P + sky table (float-free)
#include "demo/debug_terrain_gen.h"  // committed FLASH-CONST placeholder geometry
#include "fixed/fixed.h"             // Vec4fx, fx_* (Δ4 u_iw overflow probe)
#include "geom/geom.h"               // geom_transform_clip/project (Δ4 probe)
#include "gfx/framebuffer.h"
#include "gtest/gtest.h"
#include "platform/platform.h"  // enum Button (camera trace)
#include "png_io.h"             // png_write_rgb8 (env-gated visual dump)
#include "rdr/config.h"
#include "rdr/frame.h"
#include "rdr/rdr.h"
#include "shade/shade.h"  // shade_pixel + CC_* (ENV-wiring proof)

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
  // T3a: fill the sky-blit list exactly as main.cc does (after begin, before
  // end), so the host-rendered fb mirrors the device frame for frame (and the
  // fb_crc golden / on-target reference includes the scrolling panorama).
  demo_fill_blits(&g_frame, cam);
  ASSERT_EQ(rdr_submit(&g_frame, cmds), RDR_OK);
  ASSERT_EQ(rdr_end_frame(&g_frame), RDR_OK);
}

// Δ4 MEASUREMENT (the most load-bearing T1 guard): the textured terrain submits
// REAL absolute-atlas UVs in S10.5 (u up to ~512*32 = 16384 raw). geom births
// TVtx.u_iw = (int16_t)(u_s105 * inv_w_real) — an int16. If the perspective
// scale (inv_w) pushes |u*inv_w| past 32767, that int16 OVERFLOWS and the
// textured sampler reads garbage texels. This sweeps the WHOLE scripted camera
// loop over every terrain vertex, replicating geom's project math at FULL int32
// width (the value BEFORE the int16 narrow), and asserts the true peak fits
// int16. peak >= 32767 => Δ4 FIRES (widen TVtx.u_iw int16->int32 in the FROZEN
// types.h, a Lead-owned contract delta — out of T1 scope). Modelview is
// identity (verts authored in world), so the MVP is g_scripted_mvp[frame % N].
// u_iw = u * inv_w is viewport-INDEPENDENT (the viewport map only scales x/y),
// so no Viewport is needed here.
TEST(TerrainSceneGuard, TexcoordTimesInvWFitsInt16) {
  int32_t peak = 0;
  for (uint32_t frame = 0; frame < (uint32_t)SCRIPTED_FRAME_COUNT; ++frame) {
    const struct Mat4fx* mvp = (const struct Mat4fx*)
        g_scripted_mvp[frame % (uint32_t)SCRIPTED_FRAME_COUNT];
    for (int i = 0; i < DEMO_TERRAIN_VERTS; ++i) {
      const struct Vtx* sv = &g_debug_terrain_vtx[i];
      struct Vec4fx clip;
      geom_transform_clip(&clip, mvp, sv);
      if (clip.w <= 0) {
        continue;  // behind near plane; geom near-clips (not a u_iw source)
      }
      // Replicate geom_project's u_iw/v_iw math at FULL width (no int16 narrow)
      // so an overflow is VISIBLE rather than wrapped away.
      fx_invw const inv_w = fx_div(fx_from_int(1), clip.w);
      int32_t const u_full = fx_to_int(fx_mul(fx_from_int(sv->uv[0]), inv_w));
      int32_t const v_full = fx_to_int(fx_mul(fx_from_int(sv->uv[1]), inv_w));
      int32_t const au = (u_full < 0) ? -u_full : u_full;
      int32_t const av = (v_full < 0) ? -v_full : v_full;
      if (au > peak) {
        peak = au;
      }
      if (av > peak) {
        peak = av;
      }
    }
  }
  fprintf(stderr, "[delta4] peak |u_iw/v_iw| = %d\n", peak);
  EXPECT_LT(peak, 32768)
      << "Δ4 FIRES: terrain u*inv_w overflows int16 TVtx.u_iw (peak=" << peak
      << ") — escalate to Lead to widen TVtx.u_iw/v_iw int16->int32";
}

// ENV-WIRING PROOF: the terrain material binds the gutter'd atlas with the
// P4-2 ENV combiner (TEXEL0 x ENV). Asserts the descriptor shape AND that the
// combiner functionally applies the ENV tint per channel (recomputed with the
// same N64 RDP combine math shade.cc uses).
static void unpack565_8(uint16_t c, int* r, int* g, int* b) {
  int const r5 = (c >> 11) & 0x1F;
  int const g6 = (c >> 5) & 0x3F;
  int const b5 = c & 0x1F;
  *r = (r5 << 3) | (r5 >> 2);
  *g = (g6 << 2) | (g6 >> 4);
  *b = (b5 << 3) | (b5 >> 2);
}
// One channel of (A-B)*C+D, N64 RDP rounding/scale/clamp (mirror combine_chan).
static int combine_chan_ref(int a, int b, int c, int d) {
  int out = ((a - b) * c) + (d << 8) + 0x80;
  out >>= 8;
  if (out < 0) {
    out = 0;
  }
  if (out > 255) {
    out = 255;
  }
  return out;
}
TEST(TerrainSceneGuard, TerrainMaterialAppliesEnvTint) {
  struct RenderState rs;
  demo_terrain_material(&rs);
  ASSERT_NE(rs.tex.data, (const void*)0);
  EXPECT_EQ(rs.tex.format, (uint8_t)TEXFMT_RGBA5551);
  EXPECT_EQ(rs.combiner.mode, (uint8_t)COMBINE_CUSTOM);
  EXPECT_EQ(rs.combiner.c, (uint8_t)CC_ENVIRONMENT);
  EXPECT_EQ(rs.cull, (uint8_t)CULL_BACK);
  // T2: terrain samples bilinear (gutter'd atlas -> seam-correct).
  EXPECT_EQ(rs.tex.filter, (uint8_t)FILTER_THREE_POINT);

  // Functional: TEXEL0 x ENV on a known non-black texel must equal the per-
  // channel combine of (texel) and (env). Pick a mid green-ish 565 texel.
  uint16_t const texel = rgb565(120, 200, 80);
  uint8_t keep = 0;
  uint16_t const got = shade_pixel(&rs, texel, 0xFFFF, &keep);
  int tr;
  int tg;
  int tb;
  unpack565_8(texel, &tr, &tg, &tb);
  int er;
  int eg;
  int eb;
  unpack565_8(rs.env_color, &er, &eg, &eb);
  // a=texel, b=0, c=env, d=0  ->  out = clamp((texel*env + 0x80) >> 8).
  uint16_t const want = rgb565((uint8_t)combine_chan_ref(tr, 0, er, 0),
                               (uint8_t)combine_chan_ref(tg, 0, eg, 0),
                               (uint8_t)combine_chan_ref(tb, 0, eb, 0));
  EXPECT_EQ(got, want) << "terrain combiner is not TEXEL0 x ENV";
}

// TREE MATERIAL (opaque cutout, T2): faithful sprite descriptor (32x64
// RGBA5551), MODULATE (TEXEL x SHADE), double-sided billboard (CULL_NONE, N6),
// crisp POINT filter, and the alpha-cutout threshold that kills the
// transparent billboard background (G_RM_AA_ZB_TEX_EDGE, the visible T2 win).
TEST(TerrainSceneGuard, TreeMaterialIsDoubleSidedTextured) {
  struct RenderState rs;
  demo_tree_material(&rs);
  ASSERT_NE(rs.tex.data, (const void*)0);
  EXPECT_EQ(rs.tex.w, (uint16_t)32);
  EXPECT_EQ(rs.tex.h, (uint16_t)64);
  EXPECT_EQ(rs.tex.format, (uint8_t)TEXFMT_RGBA5551);
  EXPECT_EQ(rs.cull, (uint8_t)CULL_NONE);
  EXPECT_EQ(rs.combiner.mode, (uint8_t)COMBINE_MODULATE);
  EXPECT_EQ(rs.tex.filter, (uint8_t)FILTER_POINT);  // crisp cutout edge
  EXPECT_EQ(rs.zmode, (uint8_t)ZMODE_OPAQUE);
  EXPECT_NE(rs.alpha_cmp, (uint8_t)0)
      << "tree cutout pass must alpha-test (else the black billboard "
         "background is not discarded)";
}

// TERRAIN-SCENE GUARD: real-density TEXTURED terrain + trees produce VARIED
// output, catching a one-blob / flat-fill / wrong-UV regression. (Pre-T1 this
// guarded debug colors; now the atlas sample + ENV tint must still light many
// hue buckets — a flat-fill regression or a collapsed UV map would not.)
TEST(TerrainSceneGuard, RealDensityRendersTexturedTerrain) {
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
  // scene-build work), distinct from the geom-accepted count above. T2 ships
  // the cutout pass only (the XLU pass is deferred to T4), so trees draw ONCE.
  EXPECT_EQ(telem.cmdgen_tris, (uint32_t)(DEMO_TERRAIN_TRIS + DEMO_TREE_TRIS));
  // Two draws: terrain, tree-cutout.
  EXPECT_EQ(telem.cmdgen_draws, 2U);
  EXPECT_EQ(telem.cmdgen_verts,
            (uint32_t)(DEMO_TERRAIN_VERTS + DEMO_TREE_VERTS));

  // The scene must rasterize.
  uint16_t const clear = g_frame.clear_color;
  int const drawn =
      count_non_clear(g_frame.fb, RDR_SCREEN_W * RDR_SCREEN_H, clear);
  EXPECT_GT(drawn, 1000) << "terrain binned but barely drew — drawn=" << drawn;

  // HUE HISTOGRAM: count distinct coarse hue buckets among non-clear pixels. A
  // correct TEXTURED render (atlas sample x light-sage ENV + gray-gradient tree
  // sprites) lights up many buckets; a flat-fill regression, a collapsed UV
  // map, or an over-saturated ENV that washes out the atlas hue variety would
  // collapse toward ~1.
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
  fprintf(stderr, "[hist] distinct hue buckets = %d\n", distinct);
  // actual ~35; 20 leaves margin while still biting a partial collapse.
  EXPECT_GE(distinct, 20)
      << "too few distinct hues — flat-fill / wrong-UV regression? distinct="
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

// PEAK TVERT PROBE (T0 cap-drop driver): with G1 transform-once sharing, geom
// transforms each LOAD_VERTS window ONCE into the pool and the all-inside fast
// path bins shared indices (zero new tverts), so peak pool occupancy is
// ~terrain_verts + tree_verts (+ near-clip fans, none on this path) regardless
// of how the field frames. Sweep the whole scripted loop and record the high-
// water geom.tvert_count; it must stay under RDR_MAX_TVERTS and pins the
// headroom that justifies dropping the cap. Prints the measured peak for the T0
// report.
TEST(TerrainSceneGuard, PeakTvertUnderCapAcrossScriptedLoop) {
  struct DemoCamera cam;
  struct DemoScroll scroll;
  struct DemoTelemetry telem;
  demo_camera_init(&cam);
  demo_scroll_init(&scroll);

  uint32_t peak = 0;
  uint32_t const total_frames = (uint32_t)SCRIPTED_FRAME_COUNT;
  for (uint32_t f = 0; f < total_frames; ++f) {
    render_terrain(&cam, &scroll, &telem);
    if (g_frame.geom.tvert_count > peak) {
      peak = g_frame.geom.tvert_count;
    }
    demo_camera_advance(&cam, 0, 0);
    demo_scroll_advance(&scroll);
  }
  fprintf(
      stderr, "[T0] peak tverts = %u (cap RDR_MAX_TVERTS=%d, scene verts=%u)\n",
      peak, RDR_MAX_TVERTS, (unsigned)(DEMO_TERRAIN_VERTS + DEMO_TREE_VERTS));
  // Sharing holds: peak ~= scene vert count + a small guard-band screen-clip
  // fan slack (NOT tris*3 — the pre-G1 no-sharing worst case 1024*3+252*3=3828
  // would blow the cap). A regression that loses sharing (re-emitting per tri)
  // spikes this well past the bound below. 512 = clip-fan headroom (fresh fan
  // tverts from tris crossing the screen guard band).
  uint32_t const scene_verts = (uint32_t)(DEMO_TERRAIN_VERTS + DEMO_TREE_VERTS);
  EXPECT_LT(peak, scene_verts + 512U)
      << "peak tverts far above the loaded vert count — vertex sharing "
         "regressed?";
  EXPECT_LT(peak, (uint32_t)RDR_MAX_TVERTS) << "peak tverts over the pool cap";
}

// CRC32 (IEEE, reflected; poly 0xEDB88320) over the framebuffer BYTES — must be
// byte-identical to main.cc's fb_crc32 so the host reference equals the device
// serial CRC. Both targets are little-endian → identical fb bytes → bit-equal
// CRC whenever the pipeline is float-free (the offline-baked Q16.16 camera
// guarantees that — TW-05 / ScriptedMvpComesOnlyFromCommittedTable).
static uint32_t host_fb_crc32(const uint16_t* fb, int n) {
  const uint8_t* p = (const uint8_t*)fb;
  size_t const len = (size_t)n * sizeof(uint16_t);
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t i = 0; i < len; ++i) {
    crc ^= p[i];
    for (int k = 0; k < 8; ++k) {
      uint32_t const mask = (uint32_t)(-(int32_t)(crc & 1U));
      crc = (crc >> 1) ^ (0xEDB88320U & mask);
    }
  }
  return crc ^ 0xFFFFFFFFU;
}

// HOST fb_crc REFERENCE DUMP (T0 host↔device cross-check generator). Env-gated
// so normal ctest SKIPS it; set DUMP_FB_CRC_FRAMES=N to replay main.cc's
// scripted loop for N frames on host and print `frame=K fb_crc=...` per frame —
// the reference the on-target probe diffs the device USB-CDC serial against.
// Mirrors main.cc EXACTLY: init cam/scroll, then per frame build→render→crc→
// advance (cam.frame==loop index, scripted ignores input).
TEST(TerrainSceneGuard, DumpFbCrcReferenceSequence) {
  const char* env = getenv("DUMP_FB_CRC_FRAMES");
  if (env == nullptr) {
    GTEST_SKIP()
        << "set DUMP_FB_CRC_FRAMES=N to emit the host fb_crc reference";
  }
  long const frames = strtol(env, nullptr, 10);
  if (frames < 1) {
    GTEST_SKIP() << "DUMP_FB_CRC_FRAMES must be >= 1";
  }
  struct DemoCamera cam;
  struct DemoScroll scroll;
  struct DemoTelemetry telem;
  demo_camera_init(&cam);
  demo_scroll_init(&scroll);
  for (long f = 0; f < frames; ++f) {
    render_terrain(&cam, &scroll, &telem);
    uint32_t const crc = host_fb_crc32(g_fb.px, RDR_SCREEN_W * RDR_SCREEN_H);
    // Sum per-tile bin overflow (shortfall when the shared pool can't give a
    // tile its full demand). With arena bins sized to the worst-frame total
    // demand this is 0 — binof>0 is the missing-geometry alarm. maxtile = worst
    // single-tile demand; poolused = shared-pool high-water this frame.
    uint32_t binof = 0;
    uint32_t maxtile = 0;
    for (int t = 0; t < GEOM_NUM_TILES; ++t) {
      binof += g_frame.geom.tiles[t].dropped;
      uint32_t const demand =
          g_frame.geom.tiles[t].count + g_frame.geom.tiles[t].dropped;
      if (demand > maxtile) {
        maxtile = demand;
      }
    }
    printf(
        "frame=%u fb_crc=%08x dropped=%u tris=%u binof=%u maxtile=%u "
        "poolused=%u jobs=%u\n",
        (unsigned)f, crc, (unsigned)g_frame.geom.tris_dropped,
        (unsigned)g_frame.geom.tris_total, (unsigned)binof, (unsigned)maxtile,
        (unsigned)g_frame.geom.pool_used, (unsigned)g_frame.geom.jobs_count);
    demo_camera_advance(&cam, 0, 0);
    demo_scroll_advance(&scroll);
  }
}

// ENV-GATED PNG DUMP (visual inspection). The host fb_crc digest pins
// determinism but NOT visual correctness — the T3a panorama's vertical
// orientation + overall look need an eyeball (the T2 lesson). Set
// DUMP_FB_PNG=<dir> to write frame_<K>.png for a few representative scripted
// frames (full sky+terrain, exactly as the device renders); SKIPs otherwise.
TEST(TerrainSceneGuard, DumpFramePng) {
  const char* dir = getenv("DUMP_FB_PNG");
  if (dir == nullptr) {
    GTEST_SKIP() << "set DUMP_FB_PNG=<dir> to write frame PNGs";
  }
  static uint8_t rgb[RDR_SCREEN_W * RDR_SCREEN_H * 3];
  int const want[] = {0, 120, 240, 360};
  for (int wi = 0; wi < 4; ++wi) {
    struct DemoCamera cam;
    struct DemoScroll scroll;
    struct DemoTelemetry telem;
    demo_camera_init(&cam);
    demo_scroll_init(&scroll);
    for (int f = 0; f < want[wi]; ++f) {
      demo_camera_advance(&cam, 0, 0);
      demo_scroll_advance(&scroll);
    }
    render_terrain(&cam, &scroll, &telem);
    for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
      int r;
      int g;
      int b;
      unpack565(g_fb.px[i], &r, &g, &b);
      rgb[(i * 3) + 0] = (uint8_t)r;
      rgb[(i * 3) + 1] = (uint8_t)g;
      rgb[(i * 3) + 2] = (uint8_t)b;
    }
    char path[256];
    snprintf(path, sizeof path, "%s/frame_%03d.png", dir, want[wi]);
    EXPECT_EQ(png_write_rgb8(path, rgb, RDR_SCREEN_W, RDR_SCREEN_H), 0)
        << "png_write failed: " << path;
  }
}

// ARENA-BINS GUARD (Wave-E): the T0 probe found the OLD fixed per-tile bins
// (RDR_REFS_PER_TILE=256) overflowed every frame — worst single tile demanded
// 759 tris, dropping real geometry. Arena-backed variable bins draw from one
// shared pool sized to the worst-frame TOTAL demand, so NO geometry is lost.
// Sweep the whole scripted loop and assert: zero per-tile overflow, and the
// shared-pool + job-buffer high-water stay under their caps (with headroom). A
// regression that under-sizes a cap or loses the variable-bin behavior trips
// this. Prints the high-water marks for the T0 report.
//
// T2 NOTE: the faithful tree XLU pass (a SECOND tree draw) doubled the tree
// per-tile spans and pushed worst-frame bin_pool demand 1756 -> 2310, over
// RDR_BIN_POOL_REFS=2048. Per the Lead decision the XLU demo pass is DEFERRED
// to T4 (visually identical at 1-bit alpha / no-AA), so trees draw ONCE again
// and demand returns to ~1756 < 2048 — this guard passes with frame.h
// UNTOUCHED.
TEST(TerrainSceneGuard, ArenaBinsNoOverflowAcrossScriptedLoop) {
  struct DemoCamera cam;
  struct DemoScroll scroll;
  struct DemoTelemetry telem;
  demo_camera_init(&cam);
  demo_scroll_init(&scroll);

  uint32_t peak_pool = 0;
  uint32_t peak_jobs = 0;
  uint32_t total_overflow = 0;
  uint32_t const total_frames = (uint32_t)SCRIPTED_FRAME_COUNT;
  for (uint32_t f = 0; f < total_frames; ++f) {
    render_terrain(&cam, &scroll, &telem);
    if (g_frame.geom.pool_used > peak_pool) {
      peak_pool = g_frame.geom.pool_used;
    }
    if (g_frame.geom.jobs_count > peak_jobs) {
      peak_jobs = g_frame.geom.jobs_count;
    }
    for (int t = 0; t < GEOM_NUM_TILES; ++t) {
      total_overflow += g_frame.geom.tiles[t].dropped;
    }
    demo_camera_advance(&cam, 0, 0);
    demo_scroll_advance(&scroll);
  }
  fprintf(stderr,
          "[arena-bins] peak pool=%u/%d  peak jobs=%u/%d  bin overflow=%u\n",
          peak_pool, RDR_BIN_POOL_REFS, peak_jobs, RDR_BIN_MAX_JOBS,
          total_overflow);
  // No geometry lost to bins (the whole point of the fix).
  EXPECT_EQ(total_overflow, 0U) << "shared pool too small — bins still drop";
  // High-water under cap, with headroom (caps are sized to the measured worst).
  EXPECT_LT(peak_pool, (uint32_t)RDR_BIN_POOL_REFS)
      << "pool high-water at/over cap — size RDR_BIN_POOL_REFS up";
  EXPECT_LT(peak_jobs, (uint32_t)RDR_BIN_MAX_JOBS)
      << "job high-water at/over cap — size RDR_BIN_MAX_JOBS up";
}

// ===========================================================================
// T3a panorama sky blit
// ===========================================================================

// The committed sky table must stay in range, actually scroll (azimuth-coupled,
// not a constant), and keep the horizon in the upper frame for the look-down
// scripted path — the sky band anchors there to meet the 3D terrain horizon.
TEST(TerrainSceneGuard, SkyTableInRangeAndScrolls) {
  int16_t const first = g_scripted_sky[0][0];
  bool varies = false;
  for (uint32_t f = 0; f < (uint32_t)SCRIPTED_FRAME_COUNT; ++f) {
    int const scroll_x = g_scripted_sky[f][0];
    int const horizon = g_scripted_sky[f][1];
    EXPECT_GE(scroll_x, 0);
    EXPECT_LT(scroll_x, DEMO_PANORAMA_W)
        << "scroll_x out of cylinder at f=" << f;
    EXPECT_GE(horizon, 0);
    EXPECT_LT(horizon, RDR_SCREEN_H / 2)
        << "horizon not in the upper frame at f=" << f << " (=" << horizon
        << ")";
    if (g_scripted_sky[f][0] != first) {
      varies = true;
    }
  }
  EXPECT_TRUE(varies) << "panorama scroll never changes — azimuth not coupled";
}

// The blit fill must be DETERMINISTIC and PERIODIC with the camera loop: frame
// K and frame K+SCRIPTED_FRAME_COUNT produce the SAME descriptor. This is what
// keeps the on-target fb_crc cross-check aligned on frame%480 now that the sky
// renders (T1/T2 relied on no scroll rendering; T3a preserves the period).
// Compares the per-frame-varying fields explicitly (not memcmp — Blit2dRect has
// padding, no unique object representation).
TEST(TerrainSceneGuard, PanoramaBlitDeterministicAndPeriodic) {
  g_frame.width = RDR_SCREEN_W;
  g_frame.height = RDR_SCREEN_H;
  struct DemoCamera cam;
  demo_camera_init(&cam);
  for (int i = 0; i < 37; ++i) {
    demo_camera_advance(&cam, 0, 0);  // -> scripted frame 37
  }
  demo_fill_blits(&g_frame, &cam);
  EXPECT_EQ(g_frame.blit_count, 1U);
  struct Blit2dRect const a = g_frame.blits[0];
  EXPECT_EQ(a.mode, (uint8_t)BLIT2D_PANORAMA);
  EXPECT_NE(a.src, (const void*)nullptr);   // bound to the committed panorama
  EXPECT_NE(a.tlut, (const void*)nullptr);  // bound to the committed TLUT
  EXPECT_EQ(a.dst_w, (uint16_t)RDR_SCREEN_W);
  for (int i = 0; i < SCRIPTED_FRAME_COUNT; ++i) {
    demo_camera_advance(&cam, 0, 0);  // -> frame 37 + 480
  }
  demo_fill_blits(&g_frame, &cam);
  struct Blit2dRect const b = g_frame.blits[0];
  // The azimuth scroll + projected horizon are the per-frame fields; equal over
  // one full loop => period 480 holds (the on-target alignment invariant).
  EXPECT_EQ(a.scroll_x, b.scroll_x)
      << "panorama scroll not periodic over the camera loop — breaks frame%480";
  EXPECT_EQ(a.horizon_row, b.horizon_row) << "horizon not periodic over loop";
  EXPECT_EQ(a.mode, b.mode);
  EXPECT_EQ(a.src, b.src);
  EXPECT_EQ(a.dst_w, b.dst_w);
  EXPECT_EQ(a.dst_h, b.dst_h);
}

// The panorama must actually render INTO the frame as a FULL-FRAME backdrop:
// every pixel is panorama, or terrain rasterized over it, so the clear color is
// essentially absent (a real gap => the backdrop is not wired / not covering).
// It must also produce decoded color variety (CI8+TLUT), not a flat fill.
// Renders the scripted start frame.
TEST(TerrainSceneGuard, PanoramaBackdropCoversFrame) {
  struct DemoCamera cam;
  struct DemoScroll scroll;
  struct DemoTelemetry telem;
  demo_camera_init(&cam);
  demo_scroll_init(&scroll);
  render_terrain(&cam, &scroll, &telem);

  // Full-frame backdrop + terrain over it => clear color essentially absent
  // (only a stray pixel that coincidentally equals it). A genuine gap (backdrop
  // missing) lights up thousands.
  uint16_t const clear = g_frame.clear_color;
  int clear_px = 0;
  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    if (g_frame.fb[i] == clear) {
      ++clear_px;
    }
  }
  EXPECT_LT(clear_px, 200) << "clear-color gaps (" << clear_px
                           << ") — backdrop not covering frame";
  // The upper sky+hills region (rows above the look-down terrain edge ~row 90)
  // is pure panorama backdrop: scanning it must yield several distinct colors
  // (sky -> hill scenery) — a flat fill would mean a broken sampler / a
  // single-index source window.
  unsigned char seen[512];
  memset(seen, 0, sizeof seen);
  int distinct = 0;
  for (int y = 0; y < 70; ++y) {
    for (int x = 0; x < RDR_SCREEN_W; ++x) {
      int const b = hue_bucket(g_frame.fb[(y * RDR_SCREEN_W) + x]);
      if (!seen[b]) {
        seen[b] = 1;
        ++distinct;
      }
    }
  }
  EXPECT_GE(distinct, 3) << "panorama backdrop is a flat fill — decode broken?";
}

// FB_CRC STREAM GOLDEN (NIT-1): pin the demo scene's full per-frame fb_crc
// stream — the arena-bins model's actual rendered OUTPUT — so any change to
// determinism or visible geometry trips a host test. There is no golden vs the
// OLD (fixed-bin) model: it overflowed and dropped visible geometry, so its
// output was wrong; the on-device host==device cross-check is the only other
// anchor. Folds all SCRIPTED_FRAME_COUNT per-frame CRCs into one CRC32 digest
// (same poly as host_fb_crc32). On mismatch: if the change is intended, rebake
// the constant from the printed digest; else it's a regression — localize via
// `DUMP_FB_CRC_FRAMES=480 <demo_scene_test> --gtest_filter=*DumpFbCrc*`.
TEST(TerrainSceneGuard, FbCrcStreamMatchesGolden) {
  struct DemoCamera cam;
  struct DemoScroll scroll;
  struct DemoTelemetry telem;
  demo_camera_init(&cam);
  demo_scroll_init(&scroll);
  uint32_t digest = 0xFFFFFFFFU;
  for (uint32_t f = 0; f < (uint32_t)SCRIPTED_FRAME_COUNT; ++f) {
    render_terrain(&cam, &scroll, &telem);
    uint32_t const crc = host_fb_crc32(g_fb.px, RDR_SCREEN_W * RDR_SCREEN_H);
    for (int k = 0; k < 4; ++k) {  // fold the 4 CRC bytes into the digest
      digest ^= (uint32_t)((crc >> (k * 8)) & 0xFFU);
      for (int b = 0; b < 8; ++b) {
        uint32_t const mask = (uint32_t)(-(int32_t)(digest & 1U));
        digest = (digest >> 1) ^ (0xEDB88320U & mask);
      }
    }
    demo_camera_advance(&cam, 0, 0);
    demo_scroll_advance(&scroll);
  }
  digest ^= 0xFFFFFFFFU;
  fprintf(stderr, "[golden] fb_crc stream digest = 0x%08x\n", digest);
  EXPECT_EQ(digest, 0x22856499U)
      << "demo scene fb_crc stream changed — rebake if intended, else regress";
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
