// raster_test.cc — Stream B.1-gamma raster tests (flat fill + per-tile Z).
// Host-first TDD. Golden reference = tests/harness oracle_fill_tri (top-left
// fill rule + winding normalization). We render one tile with raster_tile and
// compare its color output pixel-for-pixel against the oracle filling the same
// triangle (tile-local coordinates), proving fill-rule + coverage parity.
#include "raster/raster.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#include "golden.h"
#include "gtest/gtest.h"
#include "oracle.h"
#include "rdr/config.h"
#include "rdr/types.h"

namespace {

// RGB565 flat color used across tests (a non-trivial value to catch packing
// bugs). 565: R=10110(22), G=011010(26), B=01011(11).
const uint16_t K_FLAT565 = (uint16_t)((22U << 11) | (26U << 5) | 11U);

// rgb565 packer matching gfx/framebuffer.h rgb565() (avoid pulling that
// header).
uint16_t rgb565_pack(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)((((uint16_t)(r & 0xF8)) << 8) |
                    (((uint16_t)(g & 0xFC)) << 3) | (uint16_t)(b >> 3));
}

// A 1-entry render-state table whose material 0 has NO texture (tex zeroed) so
// every flat-fill test takes raster's BIT-IDENTICAL fast path. raster_tile reads
// this read-only; one shared instance suffices for all flat tests. (The textured
// tests below build their own tables.)
const struct RenderState* no_texture_table() {
  static struct RenderState rstate;  // zero-init: tex.data/w/h = 0 -> fast path
  static int init = 0;
  if (!init) {
    memset(&rstate, 0, sizeof(rstate));
    init = 1;
  }
  return &rstate;
}

// Build a TVtx at pixel (px,py) with subpixel offsets (in 1/16 px), depth, and
// the flat color. Q12.4: screen value = px*16 + sub.
TVtx mk_vtx(int px, int py, int sub_x, int sub_y, int32_t inv_w_q16) {
  TVtx v;
  memset(&v, 0, sizeof(v));
  v.x = (fx12_4)((px * 16) + sub_x);
  v.y = (fx12_4)((py * 16) + sub_y);
  v.inv_w = (fx_invw)inv_w_q16;
  v.rgba = K_FLAT565;
  return v;
}

// One-triangle bin over an explicit 3-vertex pool.
struct OneTri {
  TVtx pool[3];
  TriRef ref;
  TileBin bin;
};

void one_tri(OneTri* o, TVtx a, TVtx b, TVtx c) {
  o->pool[0] = a;
  o->pool[1] = b;
  o->pool[2] = c;
  o->ref.v0 = 0;
  o->ref.v1 = 1;
  o->ref.v2 = 2;
  o->ref.material = 0;
  o->bin.refs = &o->ref;
  o->bin.count = 1;
  o->bin.cap = 1;
  o->bin.dropped = 0;
}

// Unpack the test flat color to RGB8 (oracle reference fill color).
void flat_rgb(uint8_t* r, uint8_t* g, uint8_t* b) {
  oracle_unpack565(K_FLAT565, r, g, b);
}

// Render the bin into a full framebuffer at tile 0, then extract the tile's
// pixels into an RGB8 image for comparison. tile 0 = top-left, pixels
// [0,RDR_TILE_W) x [0,RDR_TILE_H).
void render_tile0_to_rgb(const TileBin* bin, const TVtx* pool, uint8_t* rgb_out,
                         uint8_t clr_r, uint8_t clr_g, uint8_t clr_b) {
  static uint16_t fb[RDR_SCREEN_W * RDR_SCREEN_H];
  static uint16_t zbuf[RDR_TILE_W * RDR_TILE_H];
  // Clear FB to background color (use 0 -> black so coverage shows).
  uint16_t const bg = rgb565_pack(clr_r, clr_g, clr_b);
  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    fb[i] = bg;
  }
  raster_tile(0, bin, pool, fb, zbuf, no_texture_table());
  for (int y = 0; y < RDR_TILE_H; ++y) {
    for (int x = 0; x < RDR_TILE_W; ++x) {
      uint16_t const px = fb[(y * RDR_SCREEN_W) + x];
      uint8_t r;
      uint8_t g;
      uint8_t b;
      oracle_unpack565(px, &r, &g, &b);
      size_t const o = (((size_t)y * RDR_TILE_W) + x) * 3;
      rgb_out[o + 0] = r;
      rgb_out[o + 1] = g;
      rgb_out[o + 2] = b;
    }
  }
}

// Compare raster tile-0 output to the oracle filling the same triangle in
// tile-local pixel space, returns count of mismatching pixels.
int compare_to_oracle(const OneTri* o) {
  uint8_t fr;
  uint8_t fg;
  uint8_t fb;
  flat_rgb(&fr, &fg, &fb);
  // Background = black (0,0,0).
  uint8_t rast[RDR_TILE_W * RDR_TILE_H * 3];
  render_tile0_to_rgb(&o->bin, o->pool, rast, 0, 0, 0);

  // Oracle: fill same tri in tile-local pixel coords (Q12.4 -> px).
  uint8_t ref[RDR_TILE_W * RDR_TILE_H * 3];
  OImage img;
  img.rgb = ref;
  img.w = RDR_TILE_W;
  img.h = RDR_TILE_H;
  oracle_image_clear(&img, 0, 0, 0);
  oracle_fill_tri(&img, (float)o->pool[0].x / 16.0F,
                  (float)o->pool[0].y / 16.0F, (float)o->pool[1].x / 16.0F,
                  (float)o->pool[1].y / 16.0F, (float)o->pool[2].x / 16.0F,
                  (float)o->pool[2].y / 16.0F, fr, fg, fb);

  int diff = 0;
  for (int i = 0; i < RDR_TILE_W * RDR_TILE_H * 3; ++i) {
    if (rast[i] != ref[i]) {
      ++diff;
    }
  }
  return diff;
}

// Render one triangle (tile 0) onto a black FB, return a tile-local coverage
// mask (1 where the flat color was written). Caller-supplied mask sized
// RDR_TILE_W*RDR_TILE_H.
void coverage_of(const OneTri* o, uint8_t* mask) {
  uint8_t fr;
  uint8_t fg;
  uint8_t fb;
  flat_rgb(&fr, &fg, &fb);
  uint8_t rast[RDR_TILE_W * RDR_TILE_H * 3];
  render_tile0_to_rgb(&o->bin, o->pool, rast, 0, 0, 0);
  for (int i = 0; i < RDR_TILE_W * RDR_TILE_H; ++i) {
    mask[i] = (uint8_t)(rast[(i * 3) + 0] == fr && rast[(i * 3) + 1] == fg &&
                        rast[(i * 3) + 2] == fb);
  }
}

}  // namespace

TEST(Raster, LinksAndContractCompiles) { SUCCEED(); }

// A clearly-interior triangle (no edges through pixel centers) must match the
// oracle exactly — pure coverage parity.
TEST(Raster, InteriorTriangleMatchesOracle) {
  OneTri o;
  one_tri(&o, mk_vtx(10, 8, 3, 1, 0x10000), mk_vtx(45, 12, 7, 5, 0x10000),
          mk_vtx(20, 48, 1, 9, 0x10000));
  EXPECT_EQ(compare_to_oracle(&o), 0);
}

// Reversed-winding triangle: the rasterizer normalizes winding internally, so
// the same three points in CW order fill identically (and still match oracle).
TEST(Raster, ReversedWindingFillsSame) {
  OneTri o;
  one_tri(&o, mk_vtx(10, 8, 3, 1, 0x10000), mk_vtx(20, 48, 1, 9, 0x10000),
          mk_vtx(45, 12, 7, 5, 0x10000));
  EXPECT_EQ(compare_to_oracle(&o), 0);
}

// Triangle whose vertices land on integer pixel grid (edges pass through pixel
// centers at +0.5) — exercises the top-left tie-break against the oracle.
TEST(Raster, AxisAlignedEdgesMatchOracle) {
  OneTri o;
  one_tri(&o, mk_vtx(5, 5, 0, 0, 0x10000), mk_vtx(40, 5, 0, 0, 0x10000),
          mk_vtx(5, 40, 0, 0, 0x10000));
  EXPECT_EQ(compare_to_oracle(&o), 0);
}

// Top-left fill rule watertightness: split a quad into two triangles sharing
// the diagonal. Every pixel of the quad's interior must be covered EXACTLY
// once across the two tris — no double-cover, no gap on the shared edge.
TEST(Raster, SharedEdgeWatertight) {
  // Quad corners (Q12.4 via integer pixels): A(8,8) B(50,8) C(50,46) D(8,46).
  TVtx const a = mk_vtx(8, 8, 0, 0, 0x10000);
  TVtx const b = mk_vtx(50, 8, 0, 0, 0x10000);
  TVtx const c = mk_vtx(50, 46, 0, 0, 0x10000);
  TVtx const d = mk_vtx(8, 46, 0, 0, 0x10000);

  OneTri t1;
  one_tri(&t1, a, b, c);  // A,B,C
  OneTri t2;
  one_tri(&t2, a, c, d);  // A,C,D  (shares edge A->C)

  uint8_t m1[RDR_TILE_W * RDR_TILE_H];
  uint8_t m2[RDR_TILE_W * RDR_TILE_H];
  coverage_of(&t1, m1);
  coverage_of(&t2, m2);

  int both = 0;     // covered by both -> double-cover (bad)
  int covered = 0;  // covered by exactly one
  for (int i = 0; i < RDR_TILE_W * RDR_TILE_H; ++i) {
    if (m1[i] && m2[i]) {
      ++both;
    }
    if (m1[i] || m2[i]) {
      ++covered;
    }
  }
  EXPECT_EQ(both, 0) << "shared edge double-covered (fill rule broken)";

  // No gap: the union must exactly equal the oracle's fill of the whole quad
  // expressed as the same two tris unioned (a watertight quad). Reference fill
  // = oracle over both tris into one image.
  uint8_t fr;
  uint8_t fg;
  uint8_t fb;
  flat_rgb(&fr, &fg, &fb);
  uint8_t ref[RDR_TILE_W * RDR_TILE_H * 3];
  OImage img;
  img.rgb = ref;
  img.w = RDR_TILE_W;
  img.h = RDR_TILE_H;
  oracle_image_clear(&img, 0, 0, 0);
  oracle_fill_tri(&img, (float)a.x / 16.0F, (float)a.y / 16.0F,
                  (float)b.x / 16.0F, (float)b.y / 16.0F, (float)c.x / 16.0F,
                  (float)c.y / 16.0F, fr, fg, fb);
  oracle_fill_tri(&img, (float)a.x / 16.0F, (float)a.y / 16.0F,
                  (float)c.x / 16.0F, (float)c.y / 16.0F, (float)d.x / 16.0F,
                  (float)d.y / 16.0F, fr, fg, fb);
  int ref_covered = 0;
  int mismatch = 0;
  for (int i = 0; i < RDR_TILE_W * RDR_TILE_H; ++i) {
    int const orc = (ref[(i * 3) + 0] == fr);
    if (orc) {
      ++ref_covered;
    }
    int const ours = (m1[i] || m2[i]);
    if (orc != ours) {
      ++mismatch;
    }
  }
  EXPECT_EQ(mismatch, 0) << "quad coverage diverges from oracle union";
  EXPECT_EQ(covered, ref_covered);
}

// Degenerate (zero-area / collinear) triangles are rejected at setup: nothing
// is filled, and no div-by-zero crash.
TEST(Raster, DegenerateZeroAreaRejected) {
  OneTri o;
  one_tri(&o, mk_vtx(10, 10, 0, 0, 0x10000), mk_vtx(10, 10, 0, 0, 0x10000),
          mk_vtx(10, 10, 0, 0, 0x10000));
  uint8_t mask[RDR_TILE_W * RDR_TILE_H];
  coverage_of(&o, mask);
  int cov = 0;
  for (int i = 0; i < RDR_TILE_W * RDR_TILE_H; ++i) {
    cov += mask[i];
  }
  EXPECT_EQ(cov, 0);
}

TEST(Raster, DegenerateCollinearRejected) {
  OneTri o;
  one_tri(&o, mk_vtx(5, 5, 0, 0, 0x10000), mk_vtx(15, 15, 0, 0, 0x10000),
          mk_vtx(30, 30, 0, 0, 0x10000));  // all on y=x
  uint8_t mask[RDR_TILE_W * RDR_TILE_H];
  coverage_of(&o, mask);
  int cov = 0;
  for (int i = 0; i < RDR_TILE_W * RDR_TILE_H; ++i) {
    cov += mask[i];
  }
  EXPECT_EQ(cov, 0);
}

// A sub-epsilon sliver (|2*area| <= 256 Q24.8 units = <= 1px^2) is rejected:
// nearly-collinear, area below the degenerate threshold. v0=(0,0)px,
// v1=(40,0)px+0/16, v2=(20,0)px+1/16 -> 2*area = 40*16 * 1 = 640? compute
// below.
TEST(Raster, SubPixelSliverRejected) {
  // v0 (10.0,10.0), v1 (26.0,10.0), v2 (18.0, 10+1/16). 2*area =
  // (16*16)*(1) - 0 = 256 -> NOT strictly > eps(256) -> rejected.
  OneTri o;
  one_tri(&o, mk_vtx(10, 10, 0, 0, 0x10000), mk_vtx(26, 10, 0, 0, 0x10000),
          mk_vtx(18, 10, 0, 1, 0x10000));
  uint8_t mask[RDR_TILE_W * RDR_TILE_H];
  coverage_of(&o, mask);
  int cov = 0;
  for (int i = 0; i < RDR_TILE_W * RDR_TILE_H; ++i) {
    cov += mask[i];
  }
  EXPECT_EQ(cov, 0);
}

// A small but legitimate triangle (well above epsilon, covering several pixel
// centers) DOES fill — epsilon rejects only true slivers, not real geometry.
TEST(Raster, SmallValidTriangleFills) {
  OneTri o;
  one_tri(&o, mk_vtx(10, 10, 0, 0, 0x10000), mk_vtx(16, 10, 0, 0, 0x10000),
          mk_vtx(13, 16, 0, 0, 0x10000));
  uint8_t mask[RDR_TILE_W * RDR_TILE_H];
  coverage_of(&o, mask);
  int cov = 0;
  for (int i = 0; i < RDR_TILE_W * RDR_TILE_H; ++i) {
    cov += mask[i];
  }
  EXPECT_GT(cov, 0);
  // And it must match the oracle exactly.
  EXPECT_EQ(compare_to_oracle(&o), 0);
}

// Clipping: a triangle that extends beyond the tile only writes inside the tile
// rect. Use a triangle straddling the tile-0 right boundary (x up to 100, but
// tile-0 is x in [0,RDR_TILE_W)). Pixels at x >= RDR_TILE_W must be untouched
// (they belong to a different tile).
TEST(Raster, ClipsToTileBounds) {
  static uint16_t fb[RDR_SCREEN_W * RDR_SCREEN_H];
  static uint16_t zbuf[RDR_TILE_W * RDR_TILE_H];
  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    fb[i] = 0;  // black
  }
  OneTri o;
  one_tri(&o, mk_vtx(40, 5, 0, 0, 0x10000), mk_vtx(100, 30, 0, 0, 0x10000),
          mk_vtx(40, 55, 0, 0, 0x10000));
  raster_tile(0, &o.bin, o.pool, fb, zbuf, no_texture_table());
  // No pixel with screen x >= RDR_TILE_W should be written by tile 0.
  int leaked = 0;
  for (int y = 0; y < RDR_SCREEN_H; ++y) {
    for (int x = RDR_TILE_W; x < RDR_SCREEN_W; ++x) {
      if (fb[(y * RDR_SCREEN_W) + x] != 0) {
        ++leaked;
      }
    }
  }
  EXPECT_EQ(leaked, 0);
  // And no pixel with screen y >= RDR_TILE_H either (below tile 0).
  int leaked_y = 0;
  for (int y = RDR_TILE_H; y < RDR_SCREEN_H; ++y) {
    for (int x = 0; x < RDR_SCREEN_W; ++x) {
      if (fb[(y * RDR_SCREEN_W) + x] != 0) {
        ++leaked_y;
      }
    }
  }
  EXPECT_EQ(leaked_y, 0);
}

// Per-tile Z: a nearer triangle (larger inv_w) drawn AFTER a farther one
// overwrites it; a farther triangle drawn after a nearer one does NOT. Two
// overlapping coplanar-in-screen tris with different inv_w.
TEST(Raster, ZTestNearOccludesFar) {
  // Same screen triangle, two depths. inv_w big = near.
  int32_t const near_iw = 0x20000;                // 2.0
  int32_t const far_iw = 0x08000;                 // 0.5
  uint16_t const c_near = (uint16_t)(31U << 11);  // red
  uint16_t const c_far = (uint16_t)31U;           // blue

  static uint16_t fb[RDR_SCREEN_W * RDR_SCREEN_H];
  static uint16_t zbuf[RDR_TILE_W * RDR_TILE_H];
  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    fb[i] = 0;
  }

  // Build a 2-tri bin: tri0 far (blue), tri1 near (red), same geometry.
  TVtx pool[6];
  pool[0] = mk_vtx(10, 8, 0, 0, far_iw);
  pool[1] = mk_vtx(45, 12, 0, 0, far_iw);
  pool[2] = mk_vtx(20, 48, 0, 0, far_iw);
  pool[3] = mk_vtx(10, 8, 0, 0, near_iw);
  pool[4] = mk_vtx(45, 12, 0, 0, near_iw);
  pool[5] = mk_vtx(20, 48, 0, 0, near_iw);
  pool[0].rgba = c_far;
  pool[1].rgba = c_far;
  pool[2].rgba = c_far;
  pool[3].rgba = c_near;
  pool[4].rgba = c_near;
  pool[5].rgba = c_near;
  TriRef refs[2];
  refs[0].v0 = 0;
  refs[0].v1 = 1;
  refs[0].v2 = 2;
  refs[0].material = 0;
  refs[1].v0 = 3;
  refs[1].v1 = 4;
  refs[1].v2 = 5;
  refs[1].material = 0;
  TileBin bin;
  bin.refs = refs;
  bin.count = 2;
  bin.cap = 2;
  bin.dropped = 0;
  raster_tile(0, &bin, pool, fb, zbuf, no_texture_table());

  // A pixel inside the triangle must be the NEAR color (red), not far (blue).
  uint16_t const px = fb[(20 * RDR_SCREEN_W) + 25];
  EXPECT_EQ(px, c_near);

  // Now reverse draw order: near first, far second -> far must NOT overwrite.
  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    fb[i] = 0;
  }
  refs[0].v0 = 3;
  refs[0].v1 = 4;
  refs[0].v2 = 5;  // near first
  refs[1].v0 = 0;
  refs[1].v1 = 1;
  refs[1].v2 = 2;  // far second
  raster_tile(0, &bin, pool, fb, zbuf, no_texture_table());
  uint16_t const px2 = fb[(20 * RDR_SCREEN_W) + 25];
  EXPECT_EQ(px2, c_near);
}

// Non-zero tile index maps to the correct screen rect. Tile 5 (4 tiles/row) is
// col 1, row 1 -> screen origin (60,60). A triangle in that tile's screen
// coords fills there and nowhere else, matching the oracle shifted into the
// tile.
TEST(Raster, NonZeroTileMapsToScreenRect) {
  int const tiles_per_row = RDR_SCREEN_W / RDR_TILE_W;
  int const tile = 5;
  int const x0 = (tile % tiles_per_row) * RDR_TILE_W;  // 60
  int const y0 = (tile / tiles_per_row) * RDR_TILE_H;  // 60

  static uint16_t fb[RDR_SCREEN_W * RDR_SCREEN_H];
  static uint16_t zbuf[RDR_TILE_W * RDR_TILE_H];
  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    fb[i] = 0;
  }
  // Triangle in screen coords inside tile 5's rect [60,120)x[60,120).
  OneTri o;
  one_tri(&o, mk_vtx(x0 + 10, y0 + 8, 0, 0, 0x10000),
          mk_vtx(x0 + 45, y0 + 12, 0, 0, 0x10000),
          mk_vtx(x0 + 20, y0 + 48, 0, 0, 0x10000));
  raster_tile(tile, &o.bin, o.pool, fb, zbuf, no_texture_table());

  // Oracle reference at the same absolute screen coords (full-screen image).
  uint8_t fr;
  uint8_t fg;
  uint8_t fb8;
  flat_rgb(&fr, &fg, &fb8);
  uint8_t* ref = (uint8_t*)malloc((size_t)RDR_SCREEN_W * RDR_SCREEN_H * 3);
  OImage img;
  img.rgb = ref;
  img.w = RDR_SCREEN_W;
  img.h = RDR_SCREEN_H;
  oracle_image_clear(&img, 0, 0, 0);
  oracle_fill_tri(&img, (float)o.pool[0].x / 16.0F, (float)o.pool[0].y / 16.0F,
                  (float)o.pool[1].x / 16.0F, (float)o.pool[1].y / 16.0F,
                  (float)o.pool[2].x / 16.0F, (float)o.pool[2].y / 16.0F, fr,
                  fg, fb8);

  int diff = 0;
  for (int y = 0; y < RDR_SCREEN_H; ++y) {
    for (int x = 0; x < RDR_SCREEN_W; ++x) {
      uint16_t const px = fb[(y * RDR_SCREEN_W) + x];
      uint8_t r;
      uint8_t g;
      uint8_t b;
      oracle_unpack565(px, &r, &g, &b);
      size_t const o2 = (((size_t)y * RDR_SCREEN_W) + x) * 3;
      if (r != ref[o2] || g != ref[o2 + 1] || b != ref[o2 + 2]) {
        ++diff;
      }
    }
  }
  free(ref);
  EXPECT_EQ(diff, 0);
}

// raster_tile clears the per-tile Z scratch on entry: stale large values from a
// previous tile must not block the first triangle.
TEST(Raster, ClearsZScratchOnEntry) {
  static uint16_t fb[RDR_SCREEN_W * RDR_SCREEN_H];
  static uint16_t zbuf[RDR_TILE_W * RDR_TILE_H];
  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    fb[i] = 0;
  }
  for (int i = 0; i < RDR_TILE_W * RDR_TILE_H; ++i) {
    zbuf[i] = 0xFFFF;  // stale "very near" from a prior tile
  }
  OneTri o;
  one_tri(&o, mk_vtx(10, 8, 0, 0, 0x10000), mk_vtx(45, 12, 0, 0, 0x10000),
          mk_vtx(20, 48, 0, 0, 0x10000));
  raster_tile(0, &o.bin, o.pool, fb, zbuf, no_texture_table());
  uint16_t const px = fb[(20 * RDR_SCREEN_W) + 25];
  EXPECT_EQ(px, K_FLAT565) << "Z scratch not cleared: stale depth blocked fill";
}

// Committed golden-image regression anchor: a deterministic 2-triangle,
// Z-tested scene rendered by the REAL raster_tile into a tile-sized RGB8 image
// and compared to a PNG checked into tests/raster/golden/. This locks the full
// raster output (coverage + depth resolution + color packing) against
// regression. Set GOLDEN_REGEN=1 to refresh. Never expected to FAIL/ERROR.
TEST(Raster, GoldenTileRegression) {
  // Two overlapping triangles at different depths in tile 0.
  TVtx pool[6];
  // Far triangle (blue), left-leaning.
  pool[0] = mk_vtx(6, 6, 0, 0, 0x08000);
  pool[1] = mk_vtx(50, 14, 0, 0, 0x08000);
  pool[2] = mk_vtx(12, 52, 0, 0, 0x08000);
  // Near triangle (yellow), overlapping the far one.
  pool[3] = mk_vtx(20, 4, 0, 0, 0x20000);
  pool[4] = mk_vtx(54, 40, 0, 0, 0x20000);
  pool[5] = mk_vtx(8, 44, 0, 0, 0x20000);
  uint16_t const blue = rgb565_pack(40, 80, 220);
  uint16_t const yellow = rgb565_pack(220, 200, 40);
  pool[0].rgba = blue;
  pool[1].rgba = blue;
  pool[2].rgba = blue;
  pool[3].rgba = yellow;
  pool[4].rgba = yellow;
  pool[5].rgba = yellow;
  TriRef refs[2];
  refs[0].v0 = 0;
  refs[0].v1 = 1;
  refs[0].v2 = 2;
  refs[0].material = 0;
  refs[1].v0 = 3;
  refs[1].v1 = 4;
  refs[1].v2 = 5;
  refs[1].material = 0;
  TileBin bin;
  bin.refs = refs;
  bin.count = 2;
  bin.cap = 2;
  bin.dropped = 0;

  uint8_t rgb[RDR_TILE_W * RDR_TILE_H * 3];
  uint8_t bgr;
  uint8_t bgg;
  uint8_t bgb;
  oracle_unpack565(rgb565_pack(16, 16, 32), &bgr, &bgg, &bgb);
  render_tile0_to_rgb(&bin, pool, rgb, bgr, bgg, bgb);

  struct GoldenParams p;
  memset(&p, 0, sizeof p);
  p.golden_path = RASTER_GOLDEN_DIR "/tile_ztest.png";
  std::string const tmp_dir = ::testing::TempDir();
  p.dump_dir = tmp_dir.c_str();
  p.per_channel = 0;  // raster is deterministic; exact match expected
  p.max_diff_pixels = 0;

  struct GoldenReport rep;
  int const r = golden_check(&p, rgb, RDR_TILE_W, RDR_TILE_H, &rep);
  EXPECT_NE(r, GOLDEN_FAIL)
      << "first offending pixel (" << rep.first_x << "," << rep.first_y << ")";
  EXPECT_NE(r, GOLDEN_ERROR);
}
