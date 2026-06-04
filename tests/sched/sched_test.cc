// sched_test.cc — C2 dual-core back-end raster determinism guard. gtest.
//
// THE C2 INVARIANT (dispatch.h): raster_tile is per-tile-independent — each
// tile writes a DISJOINT framebuffer pixel-rect and uses a PRIVATE per-worker
// depth scratch — so the dual-core sweep MUST be bit-identical to the
// single-core serial sweep, for ANY tile-claim order. This file proves that on
// host by rasterizing the REAL demo scene (via geom) two ways and comparing
// CRC32:
//   (a) serial, one worker, one zbuf          -> fb_serial
//   (b) 2-worker simulation, INTERLEAVED claim,
//       TWO distinct zbufs                     -> fb_par
//   crc32(fb_serial) == crc32(fb_par)  (bit-for-bit).
//
// And it proves the test has TEETH: a (broken) variant where both simulated
// workers share ONE zbuf MUST differ — demonstrating the per-worker depth
// isolation is exactly what makes the parallel path correct.
#include "sched/sched.h"

#include <stdint.h>
#include <string.h>

#include "demo/demo_scene.h"
#include "geom/geom.h"
#include "gtest/gtest.h"
#include "raster/raster.h"
#include "rdr/config.h"
#include "rdr/frame.h"
#include "rdr/rdr.h"

namespace {

// CRC32 (IEEE, reflected) — same polynomial as tests/harness/png_io.cc.
uint32_t crc32_buf(const uint8_t* buf, size_t len) {
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t i = 0; i < len; ++i) {
    crc ^= buf[i];
    for (int k = 0; k < 8; ++k) {
      uint32_t const mask = (uint32_t)(-(int32_t)(crc & 1U));
      crc = (crc >> 1) ^ (0xEDB88320U & mask);
    }
  }
  return crc ^ 0xFFFFFFFFU;
}

uint32_t crc32_fb(const uint16_t* fb, int n) {
  return crc32_buf((const uint8_t*)fb, (size_t)n * sizeof(uint16_t));
}

// Frame is large (pool + bins) -> BSS, not the stack.
struct Frame g_frame;
uint16_t g_fb_serial[RDR_SCREEN_W * RDR_SCREEN_H];
uint16_t g_fb_par[RDR_SCREEN_W * RDR_SCREEN_H];
uint16_t g_fb_shared[RDR_SCREEN_W * RDR_SCREEN_H];

// Two distinct per-tile depth scratches for the 2-worker simulation, plus a
// single shared one for the teeth-proving negative variant.
uint16_t g_z0[RDR_TILE_W * RDR_TILE_H];
uint16_t g_z1[RDR_TILE_W * RDR_TILE_H];
uint16_t g_zshared[RDR_TILE_W * RDR_TILE_H];

// Run geom for the real demo scene at a fixed angle into g_frame (populates
// g_frame.geom: the transformed-vertex pool + per-tile bins). Mirrors how
// tests/demo/demo_scene_test.cc drives the real scene through rdr.
void build_scene_geom() {
  struct Command cmds[DEMO_CMD_CAP];
  uint32_t const n = demo_scene_build(cmds, DEMO_CMD_CAP, 37.0F, -21.0F);
  ASSERT_GT(n, 0U);

  g_frame.fb = g_fb_serial;  // any valid fb; geom does not write it
  g_frame.width = RDR_SCREEN_W;
  g_frame.height = RDR_SCREEN_H;
  ASSERT_EQ(rdr_begin_frame(&g_frame), RDR_OK);
  ASSERT_EQ(rdr_submit(&g_frame, cmds), RDR_OK);  // runs geom -> g_frame.geom
  ASSERT_GT(g_frame.geom.tris_total, 0U)
      << "scene produced no tris — determinism test would be vacuous";
}

// Serial sweep: every tile in order, one worker, one zbuf -> `fb`.
void raster_serial(uint16_t* fb) {
  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    fb[i] = 0;
  }
  for (int tile = 0; tile < GEOM_NUM_TILES; ++tile) {
    raster_tile(tile, &g_frame.geom.tiles[tile], g_frame.geom.tverts, fb, g_z0,
                &g_frame.rstate_table[0]);
  }
}

// 2-worker simulation, INTERLEAVED claim order (worker0/worker1 alternate),
// each worker draining into its OWN zbuf -> `fb`. This is the dual-core
// dispatch reproduced on host: any claim order, disjoint pixel-rects, private
// depth. raster_tile re-clears each worker's scratch on entry (the per-tile
// clear), exactly as the runtime dual-core path does into f->zbuf[core_id].
void raster_two_workers_interleaved(uint16_t* fb, uint16_t* z0, uint16_t* z1) {
  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    fb[i] = 0;
  }
  for (int tile = 0; tile < GEOM_NUM_TILES; ++tile) {
    uint16_t* const z = (tile & 1) ? z1 : z0;
    raster_tile(tile, &g_frame.geom.tiles[tile], g_frame.geom.tverts, fb, z,
                &g_frame.rstate_table[0]);
  }
}

// BROKEN model: two workers RACING on ONE shared scratch. At runtime the two
// cores rasterize DIFFERENT tiles concurrently into the same zbuf; tile-local
// depth indices COLLIDE across tiles, so one core's stored depth leaks into the
// other's depth test and wrongly rejects/keeps fragments. The clear-on-entry
// that protects the private case races away. We model the race
// deterministically with raster_tile_noclear over a SINGLE scratch cleared just
// once: every tile after the first inherits the prior tiles' depths at
// colliding tile-local cells (a "very near" leftover wrongly rejects a real
// fragment — the same hazard tests/raster's ClearsZScratchOnEntry guards
// against). The output MUST differ from the correct (per-worker, re-cleared)
// result.
void raster_shared_zbuf_race(uint16_t* fb, uint16_t* zshared) {
  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    fb[i] = 0;
  }
  for (int i = 0; i < RDR_TILE_W * RDR_TILE_H; ++i) {
    zshared[i] =
        RASTER_Z_CLEAR;  // single shared clear; the per-tile clear races
  }
  for (int tile = 0; tile < GEOM_NUM_TILES; ++tile) {
    raster_tile_noclear(tile, &g_frame.geom.tiles[tile], g_frame.geom.tverts,
                        fb, zshared, &g_frame.rstate_table[0]);
  }
}

}  // namespace

// (a) == (b): serial and 2-worker (distinct zbufs) are bit-identical.
TEST(SchedDeterminism, SerialEqualsTwoWorkerInterleaved) {
  build_scene_geom();

  raster_serial(g_fb_serial);
  raster_two_workers_interleaved(g_fb_par, g_z0, g_z1);

  uint32_t const crc_serial =
      crc32_fb(g_fb_serial, RDR_SCREEN_W * RDR_SCREEN_H);
  uint32_t const crc_par = crc32_fb(g_fb_par, RDR_SCREEN_W * RDR_SCREEN_H);

  EXPECT_EQ(crc_serial, crc_par)
      << "dual-core invariant VIOLATED: serial crc=0x" << std::hex << crc_serial
      << " 2-worker crc=0x" << crc_par
      << " — per-tile rasterization is not order-independent";
  // Also assert raw bytes (CRC is a hash; the contract is bit-for-bit).
  EXPECT_EQ(memcmp(g_fb_serial, g_fb_par,
                   (size_t)(RDR_SCREEN_W * RDR_SCREEN_H) * sizeof(uint16_t)),
            0);
}

// The FROZEN SEAM itself: sched_rasterize (-> sched_dispatch_tiles ->
// dispatch_host on host) must yield the SAME framebuffer as the hand-rolled
// serial sweep. The two tests above prove raster_tile is order-independent;
// this one proves the production dispatch primitive actually drains every tile
// into the result the rest of the renderer relies on. On device the same seam
// runs the dual-core path; the on-target fb_crc telemetry closes that half.
TEST(SchedDeterminism, FrozenSeamEqualsSerial) {
  build_scene_geom();

  raster_serial(g_fb_serial);

  // Drive the real seam into g_fb_par (sched_rasterize does not clear the fb —
  // rdr_end_frame does — so match raster_serial's clear-to-0 first).
  g_frame.fb = g_fb_par;
  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    g_fb_par[i] = 0;
  }
  ASSERT_EQ(sched_rasterize(&g_frame), RDR_OK);

  EXPECT_EQ(memcmp(g_fb_serial, g_fb_par,
                   (size_t)(RDR_SCREEN_W * RDR_SCREEN_H) * sizeof(uint16_t)),
            0)
      << "sched_rasterize seam diverged from the serial sweep";
}

// TEETH: two workers SHARING one zbuf MUST differ from the serial result. This
// is the broken configuration; if it accidentally matched, the test above would
// not be proving the per-worker isolation is load-bearing.
TEST(SchedDeterminism, SharedZbufDiffersProvesIsolationMatters) {
  build_scene_geom();

  raster_serial(g_fb_serial);
  // Two workers RACING on one shared, un-recleared scratch (see model above).
  raster_shared_zbuf_race(g_fb_shared, g_zshared);

  uint32_t const crc_serial =
      crc32_fb(g_fb_serial, RDR_SCREEN_W * RDR_SCREEN_H);
  uint32_t const crc_shared =
      crc32_fb(g_fb_shared, RDR_SCREEN_W * RDR_SCREEN_H);

  EXPECT_NE(crc_serial, crc_shared)
      << "shared-zbuf variant matched serial — the test lacks teeth: the demo "
         "scene must have tiles whose depth state would collide under a shared "
         "scratch (otherwise per-worker zbuf isolation is unproven)";
}

// ===========================================================================
// R.2 XLU determinism gate: a MIXED opaque + translucent scene spanning several
// tiles must be bit-identical between the serial sweep and the 2-worker
// interleaved sweep. The XLU sweep sorts per tile (back-to-front) with a
// deterministic tie-break and each tile is drawn by exactly one worker, so
// serial == 2-worker holds. The demo does not yet emit XLU (T2 wires that), so
// this builds a hand-rolled mixed scene directly (mirrors the raster lane's
// textured dual-worker test, now with ZMODE_XLU materials + overlap).
// ===========================================================================
namespace {

enum { K_XTEXW = 8, K_XTEXH = 8 };

uint16_t xpack565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)((((uint16_t)(r & 0xF8)) << 8) |
                    (((uint16_t)(g & 0xFC)) << 3) | (uint16_t)(b >> 3));
}
uint16_t xpack4444(int r4, int g4, int b4, int a4) {
  return (uint16_t)(((r4 & 0xf) << 12) | ((g4 & 0xf) << 8) | ((b4 & 0xf) << 4) |
                    (a4 & 0xf));
}

// 8x8 RGBA4444 texture, fractional alpha (nibble 0x8 -> 136) so the XLU blend
// is non-degenerate; distinct RGB per texel.
uint16_t g_xlu_tex[K_XTEXW * K_XTEXH];
void make_xlu_tex(void) {
  for (int v = 0; v < K_XTEXH; ++v) {
    for (int u = 0; u < K_XTEXW; ++u) {
      g_xlu_tex[(v * K_XTEXW) + u] =
          xpack4444((u + 2) & 0xf, (v + 3) & 0xf, ((u ^ v) + 1) & 0xf, 0x8);
    }
  }
}

// geom_project's u_iw birth: trunc(round_fxmul(u_s105<<16, inv_w)).
int32_t xgeom_uiw(int s105, int32_t inv_w_q16) {
  int64_t const prod = ((int64_t)s105 << 16) * (int64_t)inv_w_q16;
  int64_t const bias = (prod < 0) ? -(1LL << 15) : (1LL << 15);
  int64_t const q16 = (prod + bias) >> 16;
  int64_t const i = (q16 < 0) ? -((-q16) >> 16) : (q16 >> 16);
  return (int32_t)i;
}

struct TVtx mk_xv(int px, int py, int32_t inv_w_q16, int u_s105, int v_s105,
                  uint16_t shade) {
  struct TVtx v;
  memset(&v, 0, sizeof(v));
  v.x = (fx12_4)(px * 16);
  v.y = (fx12_4)(py * 16);
  v.inv_w = (fx_invw)inv_w_q16;
  v.u_iw = (int16_t)xgeom_uiw(u_s105, inv_w_q16);
  v.v_iw = (int16_t)xgeom_uiw(v_s105, inv_w_q16);
  v.rgba = shade;
  return v;
}

// Mixed scene materials: 0 = opaque flat, 1 = opaque textured-ish (flat here,
// kept opaque), 2 = XLU textured (RGBA4444 frac alpha).
struct RenderState g_xlu_table[3];
void make_xlu_table(void) {
  memset(g_xlu_table, 0, sizeof(g_xlu_table));
  g_xlu_table[0].zmode = ZMODE_OPAQUE;  // flat opaque
  g_xlu_table[1].zmode = ZMODE_OPAQUE;  // flat opaque (2nd opaque material)
  // material 2: XLU textured.
  g_xlu_table[2].tex.data = g_xlu_tex;
  g_xlu_table[2].tex.w = K_XTEXW;
  g_xlu_table[2].tex.h = K_XTEXH;
  g_xlu_table[2].tex.format = TEXFMT_RGBA4444;
  g_xlu_table[2].tex.wrap_s = WRAP_REPEAT;
  g_xlu_table[2].tex.wrap_t = WRAP_REPEAT;
  g_xlu_table[2].tex.filter = FILTER_POINT;
  g_xlu_table[2].tex.mip_levels = 1;
  g_xlu_table[2].combiner.mode = 0;  // COMBINE_MODULATE
  g_xlu_table[2].zmode = ZMODE_XLU;
}

// A handful of tris across several tiles, mixing opaque + OVERLAPPING XLU (so
// the back-to-front sort + alpha-over are actually exercised). Returns the bin
// (binned to ALL tiles; raster_one clips per tile, same refs/order regardless
// of worker).
enum { K_XNTRI = 8 };
struct TVtx g_xlu_pool[K_XNTRI * 3];
struct TriRef g_xlu_refs[K_XNTRI];
void build_xlu_scene(struct TileBin* bin) {
  make_xlu_tex();
  make_xlu_table();
  uint16_t const white = xpack565(255, 255, 255);
  uint16_t const red = xpack565(220, 40, 40);
  uint16_t const grn = xpack565(40, 200, 60);

  // 2 opaque tris (tiles 0, 5), then 6 XLU tris in OVERLAPPING clusters across
  // tiles so several land in the SAME tile at DIFFERENT depths (exercises the
  // per-tile sort). Each tri's 3 verts share inv_w/material.
  // opaque tri 0 (tile 0)
  g_xlu_pool[0] = mk_xv(6, 6, 0x10000, 0, 0, red);
  g_xlu_pool[1] = mk_xv(54, 12, 0x10000, 0, 0, red);
  g_xlu_pool[2] = mk_xv(10, 54, 0x10000, 0, 0, red);
  // opaque tri 1 (tile 5, x~70-110 y~70-110)
  g_xlu_pool[3] = mk_xv(70, 70, 0x10000, 0, 0, grn);
  g_xlu_pool[4] = mk_xv(110, 78, 0x10000, 0, 0, grn);
  g_xlu_pool[5] = mk_xv(76, 110, 0x10000, 0, 0, grn);
  // XLU cluster A: 3 overlapping tris in tile 0 at distinct depths.
  g_xlu_pool[6] = mk_xv(8, 8, 0x08000, 0, 0, white);
  g_xlu_pool[7] = mk_xv(50, 14, 0x08000, 7 * 32, 0, white);
  g_xlu_pool[8] = mk_xv(14, 50, 0x08000, 0, 7 * 32, white);
  g_xlu_pool[9] = mk_xv(8, 8, 0x18000, 0, 0, white);
  g_xlu_pool[10] = mk_xv(50, 14, 0x18000, 7 * 32, 0, white);
  g_xlu_pool[11] = mk_xv(14, 50, 0x18000, 0, 7 * 32, white);
  g_xlu_pool[12] = mk_xv(8, 8, 0x12000, 0, 0, white);
  g_xlu_pool[13] = mk_xv(50, 14, 0x12000, 7 * 32, 0, white);
  g_xlu_pool[14] = mk_xv(14, 50, 0x12000, 0, 7 * 32, white);
  // XLU cluster B: 3 overlapping tris spanning tiles 5/6/9/10 area.
  g_xlu_pool[15] = mk_xv(72, 72, 0x0A000, 0, 0, white);
  g_xlu_pool[16] = mk_xv(150, 90, 0x0A000, 7 * 32, 0, white);
  g_xlu_pool[17] = mk_xv(80, 160, 0x0A000, 0, 7 * 32, white);
  g_xlu_pool[18] = mk_xv(72, 72, 0x1C000, 0, 0, white);
  g_xlu_pool[19] = mk_xv(150, 90, 0x1C000, 7 * 32, 0, white);
  g_xlu_pool[20] = mk_xv(80, 160, 0x1C000, 0, 7 * 32, white);
  g_xlu_pool[21] = mk_xv(72, 72, 0x10000, 0, 0, white);
  g_xlu_pool[22] = mk_xv(150, 90, 0x10000, 7 * 32, 0, white);
  g_xlu_pool[23] = mk_xv(80, 160, 0x10000, 0, 7 * 32, white);

  uint8_t const mats[K_XNTRI] = {0, 0, 2, 2, 2, 2, 2, 2};
  for (int i = 0; i < K_XNTRI; ++i) {
    g_xlu_refs[i].v0 = (uint16_t)(i * 3);
    g_xlu_refs[i].v1 = (uint16_t)((i * 3) + 1);
    g_xlu_refs[i].v2 = (uint16_t)((i * 3) + 2);
    g_xlu_refs[i].material = mats[i];
  }
  bin->refs = g_xlu_refs;
  bin->count = K_XNTRI;
  bin->cap = K_XNTRI;
  bin->dropped = 0;
}

}  // namespace

// serial == 2-worker interleaved, for a MIXED opaque + XLU scene. Same bin
// binned to every tile; serial uses one zbuf, 2-worker uses two interleaved per
// tile parity. Both clear-on-entry (raster_tile). The XLU sweep's per-tile sort
// is deterministic, so the two sweeps are bit-identical.
TEST(SchedXluDeterminism, MixedSceneSerialEqualsTwoWorker) {
  struct TileBin bin;
  build_xlu_scene(&bin);

  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    g_fb_serial[i] = 0;
    g_fb_par[i] = 0;
  }
  for (int tile = 0; tile < GEOM_NUM_TILES; ++tile) {
    raster_tile(tile, &bin, g_xlu_pool, g_fb_serial, g_z0, g_xlu_table);
  }
  for (int tile = 0; tile < GEOM_NUM_TILES; ++tile) {
    uint16_t* const z = (tile & 1) ? g_z1 : g_z0;
    raster_tile(tile, &bin, g_xlu_pool, g_fb_par, z, g_xlu_table);
  }

  uint32_t const crc_serial =
      crc32_fb(g_fb_serial, RDR_SCREEN_W * RDR_SCREEN_H);
  uint32_t const crc_par = crc32_fb(g_fb_par, RDR_SCREEN_W * RDR_SCREEN_H);
  EXPECT_EQ(crc_serial, crc_par)
      << "XLU mixed-scene dual-core invariant VIOLATED: serial crc=0x"
      << std::hex << crc_serial << " 2-worker crc=0x" << crc_par;
  EXPECT_EQ(memcmp(g_fb_serial, g_fb_par,
                   (size_t)(RDR_SCREEN_W * RDR_SCREEN_H) * sizeof(uint16_t)),
            0);
}

// TEETH: the XLU path must actually have RUN (otherwise the determinism test is
// vacuous). Re-render the SAME scene with every material forced OPAQUE; the XLU
// scene MUST differ (the translucent blend changed pixels the opaque sweep
// would not have).
TEST(SchedXluDeterminism, XluActuallyBlends) {
  struct TileBin bin;
  build_xlu_scene(&bin);

  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    g_fb_serial[i] = 0;
    g_fb_par[i] = 0;
  }
  for (int tile = 0; tile < GEOM_NUM_TILES; ++tile) {
    raster_tile(tile, &bin, g_xlu_pool, g_fb_serial, g_z0, g_xlu_table);
  }
  // Force the XLU material opaque and re-render into g_fb_par.
  struct RenderState all_opaque[3];
  memcpy(all_opaque, g_xlu_table, sizeof(all_opaque));
  all_opaque[2].zmode = ZMODE_OPAQUE;
  for (int tile = 0; tile < GEOM_NUM_TILES; ++tile) {
    raster_tile(tile, &bin, g_xlu_pool, g_fb_par, g_z0, all_opaque);
  }
  EXPECT_NE(memcmp(g_fb_serial, g_fb_par,
                   (size_t)(RDR_SCREEN_W * RDR_SCREEN_H) * sizeof(uint16_t)),
            0)
      << "XLU-as-XLU matched XLU-as-opaque — the translucent sweep did not "
         "blend, the determinism test would be vacuous";
}

// ===========================================================================
// R.3 FOG determinism gate: fog is per-pixel, derived ONLY from per-vertex
// TVtx.fog (immutable in the pool) + the read-only rstate_table fog state, so a
// fog-enabled scene MUST stay bit-identical between the serial and 2-worker
// sweeps (each tile drawn by exactly one worker; no shared mutable state). The
// demo runs fog OFF, so this builds a fog-enabled scene by hand: the SAME mixed
// opaque+XLU geometry as the XLU gate, now with fog enabled on every material
// and a per-vertex fog gradient set on the pool.
// ===========================================================================
namespace {

// Constant-expression 565 pack of (248,200,40) (avoid a non-constexpr call in a
// file-scope initializer): (248&0xF8)<<8 | (200&0xFC)<<3 | (40>>3).
const uint16_t K_FOGCOL565 = (uint16_t)((248U << 8) | (200U << 3) | (40U >> 3));

// Fog-enabled copy of the mixed material table (opaque flat, opaque flat, XLU
// textured), each with fog enabled toward K_FOGCOL565.
struct RenderState g_fog_table[3];
void make_fog_table(void) {
  make_xlu_table();  // fills g_xlu_table
  memcpy(g_fog_table, g_xlu_table, sizeof(g_fog_table));
  for (int i = 0; i < 3; ++i) {
    g_fog_table[i].fog.enabled = 1;
    g_fog_table[i].fog.color = K_FOGCOL565;
    g_fog_table[i].fog.near_z = 0;
    g_fog_table[i].fog.far_z = (fx16_16)(4 << 16);
  }
}

// Build the mixed scene then stamp a per-vertex fog GRADIENT onto every pooled
// vertex (a function of its screen x so neighbouring tiles see distinct factors
// -> the interp is exercised across tile seams). build_xlu_scene must run
// first.
void set_scene_fog_gradient(void) {
  for (int i = 0; i < K_XNTRI * 3; ++i) {
    int const px = (int)(g_xlu_pool[i].x >> 4);
    int f = (px * 255) / RDR_SCREEN_W;
    if (f < 0) {
      f = 0;
    }
    if (f > 255) {
      f = 255;
    }
    g_xlu_pool[i].fog = (uint8_t)f;
  }
}

}  // namespace

// serial == 2-worker, for a FOG-ENABLED mixed scene. Fog reads only immutable
// per-vertex TVtx.fog + read-only rstate, so the per-tile-independent invariant
// holds with fog exactly as without it.
TEST(SchedFogDeterminism, FogSceneSerialEqualsTwoWorker) {
  struct TileBin bin;
  build_xlu_scene(&bin);
  make_fog_table();
  set_scene_fog_gradient();

  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    g_fb_serial[i] = 0;
    g_fb_par[i] = 0;
  }
  for (int tile = 0; tile < GEOM_NUM_TILES; ++tile) {
    raster_tile(tile, &bin, g_xlu_pool, g_fb_serial, g_z0, g_fog_table);
  }
  for (int tile = 0; tile < GEOM_NUM_TILES; ++tile) {
    uint16_t* const z = (tile & 1) ? g_z1 : g_z0;
    raster_tile(tile, &bin, g_xlu_pool, g_fb_par, z, g_fog_table);
  }

  uint32_t const crc_serial =
      crc32_fb(g_fb_serial, RDR_SCREEN_W * RDR_SCREEN_H);
  uint32_t const crc_par = crc32_fb(g_fb_par, RDR_SCREEN_W * RDR_SCREEN_H);
  EXPECT_EQ(crc_serial, crc_par)
      << "fog mixed-scene dual-core invariant VIOLATED: serial crc=0x"
      << std::hex << crc_serial << " 2-worker crc=0x" << crc_par;
  EXPECT_EQ(memcmp(g_fb_serial, g_fb_par,
                   (size_t)(RDR_SCREEN_W * RDR_SCREEN_H) * sizeof(uint16_t)),
            0);
}

// TEETH: fog must actually have CHANGED pixels (otherwise the determinism test
// is vacuous). Re-render the SAME scene with fog DISABLED; the fog scene MUST
// differ (fog-on != fog-off). (This proves only that the fog step is live; the
// bit-identical-no-fog claim for the DISABLED path rests on the unchanged host
// goldens + gate, not on this inequality.)
TEST(SchedFogDeterminism, FogActuallyChangesPixels) {
  struct TileBin bin;
  build_xlu_scene(&bin);
  make_fog_table();
  set_scene_fog_gradient();

  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    g_fb_serial[i] = 0;
    g_fb_par[i] = 0;
  }
  // Fog ON.
  for (int tile = 0; tile < GEOM_NUM_TILES; ++tile) {
    raster_tile(tile, &bin, g_xlu_pool, g_fb_serial, g_z0, g_fog_table);
  }
  // Fog OFF (same table, fog disabled) into g_fb_par.
  struct RenderState fog_off[3];
  memcpy(fog_off, g_fog_table, sizeof(fog_off));
  for (int i = 0; i < 3; ++i) {
    fog_off[i].fog.enabled = 0;
  }
  for (int tile = 0; tile < GEOM_NUM_TILES; ++tile) {
    raster_tile(tile, &bin, g_xlu_pool, g_fb_par, g_z0, fog_off);
  }
  EXPECT_NE(memcmp(g_fb_serial, g_fb_par,
                   (size_t)(RDR_SCREEN_W * RDR_SCREEN_H) * sizeof(uint16_t)),
            0)
      << "fog-on matched fog-off — the fog step did not change any pixel, the "
         "determinism test would be vacuous";
}
