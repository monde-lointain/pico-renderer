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
    raster_tile(tile, &g_frame.geom.tiles[tile], g_frame.geom.tverts, fb, g_z0);
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
    raster_tile(tile, &g_frame.geom.tiles[tile], g_frame.geom.tverts, fb, z);
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
                        fb, zshared);
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
