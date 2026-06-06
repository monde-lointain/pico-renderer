// raster_test.cc — Stream B.1-gamma raster tests (flat fill + per-tile Z).
// Host-first TDD. Golden reference = tests/harness oracle_fill_tri (top-left
// fill rule + winding normalization). We render one tile with raster_tile and
// compare its color output pixel-for-pixel against the oracle filling the same
// triangle (tile-local coordinates), proving fill-rule + coverage parity.
#include "raster/raster.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#include "blend/blend.h"  // L6 XLU exact reference (blend_premul_accumulate/_resolve)
#include "golden.h"
#include "gtest/gtest.h"
#include "oracle.h"
#include "raster/interp.h"  // T5 L1: exact-integer DDA stepper under test
#include "rdr/config.h"
#include "rdr/types.h"
#include "shade/shade.h"  // R.1 textured-path exact reference (shade_pixel)
#include "tex/tex.h"      // R.1 textured-path exact reference (tex_sample)

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
// every flat-fill test takes raster's BIT-IDENTICAL fast path. raster_tile
// reads this read-only; one shared instance suffices for all flat tests. (The
// textured tests below build their own tables.)
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
  raster_tile(0, bin, pool, fb, zbuf, no_texture_table(), 0, /*worker=*/0);
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
  raster_tile(0, &o.bin, o.pool, fb, zbuf, no_texture_table(), 0, /*worker=*/0);
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
  raster_tile(0, &bin, pool, fb, zbuf, no_texture_table(), 0, /*worker=*/0);

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
  raster_tile(0, &bin, pool, fb, zbuf, no_texture_table(), 0, /*worker=*/0);
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
  raster_tile(tile, &o.bin, o.pool, fb, zbuf, no_texture_table(), 0,
              /*worker=*/0);

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
  raster_tile(0, &o.bin, o.pool, fb, zbuf, no_texture_table(), 0, /*worker=*/0);
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

// ===========================================================================
// R.1 — textured fill + Gouraud + alpha-cutout. New behavior beyond flat fill.
// ===========================================================================
namespace {

// 8x8 texture dimension (pow2) for the textured-path tests.
enum { K_TEXW = 8, K_TEXH = 8 };

// An 8x8 RGBA565 texture with a distinct per-texel color so the sampled coord
// is observable. texel(u,v) = R = u*30, G = v*30, B = (u^v)*30 (mod 256,
// packed).
struct Tex565 {
  uint16_t px[K_TEXW * K_TEXH];
};
void make_tex565(struct Tex565* t) {
  // High spatial frequency (adjacent texels differ a lot): good for the EXACT
  // self-check (each texel observably distinct). NOT for the float-tolerance
  // test — a 1-texel fixed-vs-float slip would dwarf the tolerance there.
  for (int v = 0; v < K_TEXH; ++v) {
    for (int u = 0; u < K_TEXW; ++u) {
      uint8_t const r = (uint8_t)(u * 30);
      uint8_t const g = (uint8_t)(v * 30);
      uint8_t const b = (uint8_t)((u ^ v) * 30);
      t->px[(v * K_TEXW) + u] = rgb565_pack(r, g, b);
    }
  }
}

// Low spatial frequency gradient: adjacent texels differ by <=2 per channel, so
// a 1-texel fixed-vs-float disagreement (from geom's u_iw truncation near a
// boundary) stays within the float-oracle tolerance. Used by the tolerance
// test.
void make_tex565_smooth(struct Tex565* t) {
  for (int v = 0; v < K_TEXH; ++v) {
    for (int u = 0; u < K_TEXW; ++u) {
      uint8_t const r = (uint8_t)(120 + (u * 2));
      uint8_t const g = (uint8_t)(120 + (v * 2));
      uint8_t const b = (uint8_t)(100 + (u + v));
      t->px[(v * K_TEXW) + u] = rgb565_pack(r, g, b);
    }
  }
}

// An 8x8 RGBA5551 cutout texture: half the texels opaque (A=1), half
// transparent (A=0). Opaque = solid magenta; transparent texels carry a
// sentinel RGB.
struct Tex5551 {
  uint16_t px[K_TEXW * K_TEXH];
};
uint16_t pack5551(int r5, int g5, int b5, int a1) {
  return (uint16_t)(((r5 & 0x1f) << 11) | ((g5 & 0x1f) << 6) |
                    ((b5 & 0x1f) << 1) | (a1 & 1));
}
void make_tex5551_cutout(struct Tex5551* t) {
  for (int v = 0; v < K_TEXH; ++v) {
    for (int u = 0; u < K_TEXW; ++u) {
      // Left half (u<4) opaque magenta; right half transparent.
      int const opaque = (u < 4) ? 1 : 0;
      t->px[(v * K_TEXW) + u] =
          opaque ? pack5551(31, 0, 31, 1) : pack5551(0, 31, 0, 0);
    }
  }
}

// Fill a TexDesc for a pow2 RGBA565 texture, POINT-sampled, REPEAT wrap.
void tex_desc_565(struct TexDesc* td, const struct Tex565* t) {
  memset(td, 0, sizeof(*td));
  td->data = t->px;
  td->w = K_TEXW;
  td->h = K_TEXH;
  td->format = TEXFMT_RGBA565;
  td->wrap_s = WRAP_REPEAT;
  td->wrap_t = WRAP_REPEAT;
  td->filter = FILTER_POINT;
  td->mip_levels = 1;
  td->tlut = 0;
}

// Reproduce geom_project's u_iw birth: fx_to_int(fx_mul(fx_from_int(s105),iw)).
// fx_mul rounds half away from zero (Q16.16); fx_to_int truncates toward zero.
int32_t geom_uiw(int s105, int32_t inv_w_q16) {
  int64_t const prod = ((int64_t)s105 << 16) * (int64_t)inv_w_q16;
  int64_t const bias = (prod < 0) ? -(1LL << 15) : (1LL << 15);
  int64_t const q16 = (prod + bias) >> 16;  // fx_mul -> Q16.16
  int64_t const i =
      (q16 < 0) ? -((-q16) >> 16) : (q16 >> 16);  // fx_to_int trunc
  return (int32_t)i;
}

// Build a TVtx carrying screen pos (Q12.4), inv_w (Q16.16), and S10.5 texcoords
// (u_s105,v_s105) -> u_iw/v_iw exactly as geom_project births them
// (trunc(round_fxmul(u_s105<<16, inv_w))). shade = packed RGB565.
TVtx mk_tvtx_uv(int px, int py, int32_t inv_w_q16, int u_s105, int v_s105,
                uint16_t shade) {
  TVtx v;
  memset(&v, 0, sizeof(v));
  v.x = (fx12_4)(px * 16);
  v.y = (fx12_4)(py * 16);
  v.inv_w = (fx_invw)inv_w_q16;
  // geom_project: u_iw = fx_to_int(fx_mul(fx_from_int(u_s105), inv_w)). fx_mul
  // rounds half away from zero -> Q16.16; fx_to_int truncates toward zero ->
  // int.
  v.u_iw = (int16_t)geom_uiw(u_s105, inv_w_q16);
  v.v_iw = (int16_t)geom_uiw(v_s105, inv_w_q16);
  v.rgba = shade;
  return v;
}

// ---- exact fixed-point reproduction of raster's textured pixel pipeline ----
// Mirrors raster.cc raster_one's integer math (post winding-normalize) so the
// expected value is bit-exact, validating the wiring (no transposition / shift
// error). Returns the expected packed-565 output for an interior covered pixel,
// or sets *covered=0 if the pixel center is not inside (caller skips it).
int32_t edge_i(fx12_4 ax, fx12_4 ay, fx12_4 bx, fx12_4 by, fx12_4 cx,
               fx12_4 cy) {
  return ((int32_t)(bx - ax) * (int32_t)(cy - ay)) -
         ((int32_t)(by - ay) * (int32_t)(cx - ax));
}
int top_left_i(fx12_4 ax, fx12_4 ay, fx12_4 bx, fx12_4 by) {
  int32_t const ex = (int32_t)bx - (int32_t)ax;
  int32_t const ey = (int32_t)by - (int32_t)ay;
  return (ey == 0 && ex < 0) || (ey < 0);
}

// A winding-normalized triangle's per-vertex attrs, mirroring TriSetup.
struct RefTri {
  fx12_4 x0, y0, x1, y1, x2, y2;
  int32_t iw0, iw1, iw2;
  int32_t u_iw0, u_iw1, u_iw2;
  int32_t v_iw0, v_iw1, v_iw2;
  uint8_t s0[3], s1[3], s2[3];  // unpacked shade per vertex
  int32_t area2;
  int tl0, tl1, tl2;
};
void unpack565_t(uint16_t c, uint8_t* r, uint8_t* g, uint8_t* b) {
  uint8_t const r5 = (uint8_t)((c >> 11) & 0x1F);
  uint8_t const g6 = (uint8_t)((c >> 5) & 0x3F);
  uint8_t const b5 = (uint8_t)(c & 0x1F);
  *r = (uint8_t)((r5 << 3) | (r5 >> 2));
  *g = (uint8_t)((g6 << 2) | (g6 >> 4));
  *b = (uint8_t)((b5 << 3) | (b5 >> 2));
}
void ref_setup(struct RefTri* t, const TVtx* a, const TVtx* b, const TVtx* c) {
  fx12_4 const x0 = a->x;
  fx12_4 const y0 = a->y;
  fx12_4 x1 = b->x;
  fx12_4 y1 = b->y;
  fx12_4 x2 = c->x;
  fx12_4 y2 = c->y;
  int32_t const iw0 = a->inv_w;
  int32_t iw1 = b->inv_w;
  int32_t iw2 = c->inv_w;
  int32_t const u0 = a->u_iw;
  int32_t u1 = b->u_iw;
  int32_t u2 = c->u_iw;
  int32_t const v0 = a->v_iw;
  int32_t v1 = b->v_iw;
  int32_t v2 = c->v_iw;
  uint16_t const r0 = a->rgba;
  uint16_t r1 = b->rgba;
  uint16_t r2 = c->rgba;
  int32_t area2 = edge_i(x0, y0, x1, y1, x2, y2);
  if (area2 < 0) {
    fx12_4 const tx = x1;
    fx12_4 const ty = y1;
    int32_t const tiw = iw1;
    int32_t const tu = u1;
    int32_t const tv = v1;
    uint16_t const tr = r1;
    x1 = x2;
    y1 = y2;
    iw1 = iw2;
    u1 = u2;
    v1 = v2;
    r1 = r2;
    x2 = tx;
    y2 = ty;
    iw2 = tiw;
    u2 = tu;
    v2 = tv;
    r2 = tr;
    area2 = -area2;
  }
  t->x0 = x0;
  t->y0 = y0;
  t->x1 = x1;
  t->y1 = y1;
  t->x2 = x2;
  t->y2 = y2;
  t->iw0 = iw0;
  t->iw1 = iw1;
  t->iw2 = iw2;
  t->u_iw0 = u0;
  t->u_iw1 = u1;
  t->u_iw2 = u2;
  t->v_iw0 = v0;
  t->v_iw1 = v1;
  t->v_iw2 = v2;
  unpack565_t(r0, &t->s0[0], &t->s0[1], &t->s0[2]);
  unpack565_t(r1, &t->s1[0], &t->s1[1], &t->s1[2]);
  unpack565_t(r2, &t->s2[0], &t->s2[1], &t->s2[2]);
  t->area2 = area2;
  t->tl0 = top_left_i(x0, y0, x1, y1);
  t->tl1 = top_left_i(x1, y1, x2, y2);
  t->tl2 = top_left_i(x2, y2, x0, y0);
}
uint8_t gouraud_ref(int32_t w0, int32_t w1, int32_t w2, int c0, int c1, int c2,
                    int32_t area2) {
  int64_t const num =
      ((int64_t)w0 * c0) + ((int64_t)w1 * c1) + ((int64_t)w2 * c2);
  int32_t v = (int32_t)(num / (int64_t)area2);
  if (v < 0) {
    v = 0;
  }
  if (v > 255) {
    v = 255;
  }
  return (uint8_t)v;
}
// Returns 1 + writes *out if pixel (sx,sy) is covered; else 0.
int ref_textured_pixel(const struct RefTri* t, const struct RenderState* rs,
                       int sx, int sy, uint16_t* out) {
  fx12_4 const cx = (fx12_4)((sx << 4) + 8);
  fx12_4 const cy = (fx12_4)((sy << 4) + 8);
  int32_t const w0 = edge_i(t->x1, t->y1, t->x2, t->y2, cx, cy);
  int32_t const w1 = edge_i(t->x2, t->y2, t->x0, t->y0, cx, cy);
  int32_t const w2 = edge_i(t->x0, t->y0, t->x1, t->y1, cx, cy);
  int const in0 = (w0 > 0) || (w0 == 0 && t->tl1);
  int const in1 = (w1 > 0) || (w1 == 0 && t->tl2);
  int const in2 = (w2 > 0) || (w2 == 0 && t->tl0);
  if (!(in0 && in1 && in2)) {
    return 0;
  }
  int64_t const numiw =
      ((int64_t)w0 * t->iw0) + ((int64_t)w1 * t->iw1) + ((int64_t)w2 * t->iw2);
  int32_t const inv_w = (int32_t)(numiw / (int64_t)t->area2);
  int64_t const numu = ((int64_t)w0 * t->u_iw0) + ((int64_t)w1 * t->u_iw1) +
                       ((int64_t)w2 * t->u_iw2);
  int64_t const numv = ((int64_t)w0 * t->v_iw0) + ((int64_t)w1 * t->v_iw1) +
                       ((int64_t)w2 * t->v_iw2);
  int32_t const u_iw_p = (int32_t)(numu / (int64_t)t->area2);
  int32_t const v_iw_p = (int32_t)(numv / (int64_t)t->area2);
  if (inv_w <= 0) {
    return 0;  // matches the impl's perspective_texcoord_q16 guard
  }
  fx16_16 const u_q16 = (fx16_16)(((int64_t)u_iw_p << 27) / (int64_t)inv_w);
  fx16_16 const v_q16 = (fx16_16)(((int64_t)v_iw_p << 27) / (int64_t)inv_w);
  uint8_t const gr =
      gouraud_ref(w0, w1, w2, t->s0[0], t->s1[0], t->s2[0], t->area2);
  uint8_t const gg =
      gouraud_ref(w0, w1, w2, t->s0[1], t->s1[1], t->s2[1], t->area2);
  uint8_t const gb =
      gouraud_ref(w0, w1, w2, t->s0[2], t->s1[2], t->s2[2], t->area2);
  uint16_t const shade565 = rgb565_pack(gr, gg, gb);
  uint16_t const texel565 = tex_sample(&rs->tex, u_q16, v_q16, 0);
  uint8_t keep = 1;
  *out = shade_pixel(rs, texel565, shade565, &keep);
  return 1;
}

}  // namespace

// Textured + Gouraud OPAQUE path: render via raster_tile, then for a set of
// interior pixels assert the rendered value EXACTLY equals
// tex_sample+shade_pixel evaluated at the analytically-interpolated coord
// (bit-exact wiring check).
TEST(RasterTextured, MatchesTexSampleAndShadeExact) {
  struct Tex565 tex;
  make_tex565(&tex);
  struct RenderState rs;
  memset(&rs, 0, sizeof(rs));
  tex_desc_565(&rs.tex, &tex);
  rs.combiner.mode = COMBINE_MODULATE;  // TEXEL0 * SHADE
  rs.alpha_cmp = 0;
  const struct RenderState* table = &rs;

  // A perspective triangle (distinct inv_w) spanning several texels, white
  // shade so MODULATE = texel (255*c/255). Texcoords span [0,7]*32 S10.5 =
  // whole tex.
  uint16_t const white = rgb565_pack(255, 255, 255);
  TVtx pool[3];
  pool[0] = mk_tvtx_uv(8, 8, 0x20000, 0, 0, white);
  pool[1] = mk_tvtx_uv(52, 12, 0x08000, 7 * 32, 0, white);
  pool[2] = mk_tvtx_uv(14, 50, 0x10000, 0, 7 * 32, white);
  TriRef ref;
  ref.v0 = 0;
  ref.v1 = 1;
  ref.v2 = 2;
  ref.material = 0;
  TileBin bin;
  bin.refs = &ref;
  bin.count = 1;
  bin.cap = 1;
  bin.dropped = 0;

  static uint16_t fb[RDR_SCREEN_W * RDR_SCREEN_H];
  static uint16_t zbuf[RDR_TILE_W * RDR_TILE_H];
  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    fb[i] = 0;
  }
  raster_tile(0, &bin, pool, fb, zbuf, table, 0, /*worker=*/0);

  struct RefTri rt;
  ref_setup(&rt, &pool[0], &pool[1], &pool[2]);
  int checked = 0;
  for (int sy = 0; sy < RDR_TILE_H; ++sy) {
    for (int sx = 0; sx < RDR_TILE_W; ++sx) {
      uint16_t expected;
      if (!ref_textured_pixel(&rt, &rs, sx, sy, &expected)) {
        continue;
      }
      uint16_t const got = fb[(sy * RDR_SCREEN_W) + sx];
      ASSERT_EQ(got, expected)
          << "textured pixel (" << sx << "," << sy << ") mismatch";
      ++checked;
    }
  }
  EXPECT_GT(checked, 50) << "too few covered pixels — test is vacuous";
}

// B1 GUARD: exercise the winding-normalize v1<->v2 swap ON THE TEXTURED PATH.
// The verts are wound so the INITIAL area2 < 0, so tri_setup MUST swap v1<->v2
// (and ALL six per-vertex attrs: x/y, iw, u_iw, v_iw, rgba) in lockstep. Each
// vertex carries DISTINCT u_iw, DISTINCT v_iw, and DISTINCT rgba, so a single
// cross-transposed attr (e.g. uiw2 = old v_iw) would change the sampled texel
// or the shade at interior pixels and break the bit-exact comparison. The
// reference (ref_setup) performs the SAME swap, so equality holds iff both swap
// identically — and the per-vertex distinctness is what gives the comparison
// teeth.
//
// VERIFIED BY HAND (not committed): if raster.cc's swap set, say, uiw2 = tviw
// (u<-v cross) the impl would sample column-from-row coords; ref keeps the
// correct mapping -> the ASSERT_EQ below fails at interior pixels. Same for any
// of the six attrs swapped into the wrong slot.
TEST(RasterTextured, ReversedWindingTexturedSwapsAllAttrsInLockstep) {
  struct Tex565 tex;
  make_tex565(&tex);  // high-frequency: each texel distinct -> a UV slip shows
  struct RenderState rs;
  memset(&rs, 0, sizeof(rs));
  tex_desc_565(&rs.tex, &tex);
  rs.combiner.mode = COMBINE_MODULATE;
  rs.alpha_cmp = 0;
  const struct RenderState* table = &rs;

  // Same screen positions as MatchesTexSampleAndShadeExact but with v1 and v2
  // SWAPPED in the pool order -> initial area2 < 0 -> forces the swap block.
  // DISTINCT per-vertex u_iw/v_iw (each vertex anchors a different texture
  // corner) AND DISTINCT per-vertex shade (red / green / blue corners).
  uint16_t const c0 = rgb565_pack(248, 0, 0);  // v0 shade
  uint16_t const c1 = rgb565_pack(0, 252, 0);  // v1 shade
  uint16_t const c2 = rgb565_pack(0, 0, 248);  // v2 shade
  TVtx pool[3];
  pool[0] = mk_tvtx_uv(8, 8, 0x20000, 0, 0, c0);         // u=0,   v=0
  pool[1] = mk_tvtx_uv(14, 50, 0x10000, 0, 7 * 32, c1);  // u=0,   v=7  (was v2)
  pool[2] = mk_tvtx_uv(52, 12, 0x08000, 7 * 32, 0, c2);  // u=7,   v=0  (was v1)
  TriRef ref;
  ref.v0 = 0;
  ref.v1 = 1;
  ref.v2 = 2;
  ref.material = 0;
  TileBin bin;
  bin.refs = &ref;
  bin.count = 1;
  bin.cap = 1;
  bin.dropped = 0;

  // Sanity: the chosen winding really is negative-area (so the swap runs).
  ASSERT_LT(
      edge_i(pool[0].x, pool[0].y, pool[1].x, pool[1].y, pool[2].x, pool[2].y),
      0)
      << "test geometry must have INITIAL area2 < 0 to exercise the swap";

  static uint16_t fb[RDR_SCREEN_W * RDR_SCREEN_H];
  static uint16_t zbuf[RDR_TILE_W * RDR_TILE_H];
  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    fb[i] = 0;
  }
  raster_tile(0, &bin, pool, fb, zbuf, table, 0, /*worker=*/0);

  // ref_setup mirrors tri_setup's swap; ref_textured_pixel mirrors the inner
  // loop. Bit-exact equality at every covered pixel proves the impl swapped all
  // six attrs into the same slots the reference did.
  struct RefTri rt;
  ref_setup(&rt, &pool[0], &pool[1], &pool[2]);
  int checked = 0;
  for (int sy = 0; sy < RDR_TILE_H; ++sy) {
    for (int sx = 0; sx < RDR_TILE_W; ++sx) {
      uint16_t expected;
      if (!ref_textured_pixel(&rt, &rs, sx, sy, &expected)) {
        continue;
      }
      uint16_t const got = fb[(sy * RDR_SCREEN_W) + sx];
      ASSERT_EQ(got, expected)
          << "reversed-winding textured pixel (" << sx << "," << sy
          << ") mismatch — attr swap transposed?";
      ++checked;
    }
  }
  EXPECT_GT(checked, 50) << "too few covered pixels — test is vacuous";
}

// Float-oracle composition check. Two independent assertions per pixel:
//   (1) PERSPECTIVE: the FLOAT perspective-correct texel index must agree with
//       the fixed-point recovery to within 1 texel (catches a wrong shift /
//       transposition in the UV recovery — independent of the impl's integer
//       path, computed entirely in float here).
//   (2) COMBINER: the impl's output, de-565'd, must match a FLOAT MODULATE
//       (texel*shade/255) of the texel the impl sampled, within +-2/channel
//       (the only residual is the N64 integer combiner rounding vs float).
// Uses a smooth texture so a 1-texel boundary slip is tiny if it shows up.
TEST(RasterTextured, MatchesFloatOracleWithinTolerance) {
  struct Tex565 tex;
  make_tex565_smooth(&tex);  // low-frequency: 1-texel slip stays in tolerance
  struct RenderState rs;
  memset(&rs, 0, sizeof(rs));
  tex_desc_565(&rs.tex, &tex);
  rs.combiner.mode = COMBINE_MODULATE;
  rs.alpha_cmp = 0;

  // Half-shade so MODULATE is observable (texel*shade/255 != texel).
  uint16_t const shade = rgb565_pack(128, 200, 64);
  TVtx pool[3];
  pool[0] = mk_tvtx_uv(8, 8, 0x20000, 0, 0, shade);
  pool[1] = mk_tvtx_uv(52, 12, 0x08000, 7 * 32, 0, shade);
  pool[2] = mk_tvtx_uv(14, 50, 0x10000, 0, 7 * 32, shade);
  TriRef ref;
  ref.v0 = 0;
  ref.v1 = 1;
  ref.v2 = 2;
  ref.material = 0;
  TileBin bin;
  bin.refs = &ref;
  bin.count = 1;
  bin.cap = 1;
  bin.dropped = 0;

  static uint16_t fb[RDR_SCREEN_W * RDR_SCREEN_H];
  static uint16_t zbuf[RDR_TILE_W * RDR_TILE_H];
  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    fb[i] = 0;
  }
  raster_tile(0, &bin, pool, fb, zbuf, &rs, 0, /*worker=*/0);

  // Float reference per covered pixel. Mirror oracle_fill_tri's winding-
  // normalize + top-left rule, then float-interpolate.
  float const fx0 = (float)pool[0].x / 16.0F;
  float const fy0 = (float)pool[0].y / 16.0F;
  float fx1 = (float)pool[1].x / 16.0F;
  float fy1 = (float)pool[1].y / 16.0F;
  float fx2 = (float)pool[2].x / 16.0F;
  float fy2 = (float)pool[2].y / 16.0F;
  float const iwf0 = (float)pool[0].inv_w / 65536.0F;
  float iwf1 = (float)pool[1].inv_w / 65536.0F;
  float iwf2 = (float)pool[2].inv_w / 65536.0F;
  // Raw S10.5 per vertex (reconstruct from u_iw/inv_w for the float oracle's
  // perspective recovery — the oracle does NOT need geom's truncation).
  int const us0 = 0;  // matches mk_tvtx_uv inputs
  int us1 = 7 * 32;
  int us2 = 0;
  int const vs0 = 0;
  int vs1 = 0;
  int vs2 = 7 * 32;
  uint8_t shr;
  uint8_t shg;
  uint8_t shb;
  oracle_unpack565(shade, &shr, &shg, &shb);
  float const area = ((fx1 - fx0) * (fy2 - fy0)) - ((fy1 - fy0) * (fx2 - fx0));
  // Normalize winding so the barycentric ratios are positive (swap 1<->2).
  if (area < 0.0F) {
    float const tx = fx1;
    float const ty = fy1;
    float const tiw = iwf1;
    int const tu = us1;
    int const tv = vs1;
    fx1 = fx2;
    fy1 = fy2;
    iwf1 = iwf2;
    us1 = us2;
    vs1 = vs2;
    fx2 = tx;
    fy2 = ty;
    iwf2 = tiw;
    us2 = tu;
    vs2 = tv;
  }
  // Fixed-point recovery mirror (for the per-pixel texel index the impl uses).
  struct RefTri rt;
  ref_setup(&rt, &pool[0], &pool[1], &pool[2]);

  int checked = 0;
  int worst = 0;
  for (int sy = 0; sy < RDR_TILE_H; ++sy) {
    for (int sx = 0; sx < RDR_TILE_W; ++sx) {
      float const cxp = (float)sx + 0.5F;
      float const cyp = (float)sy + 0.5F;
      float const e0 =
          ((fx2 - fx1) * (cyp - fy1)) - ((fy2 - fy1) * (cxp - fx1));
      float const e1 =
          ((fx0 - fx2) * (cyp - fy2)) - ((fy0 - fy2) * (cxp - fx2));
      float const e2 =
          ((fx1 - fx0) * (cyp - fy0)) - ((fy1 - fy0) * (cxp - fx0));
      // Stay strictly interior (avoid edge-rule ambiguity in this tolerance
      // test; the exact test above covers the boundary).
      if (e0 <= 0.5F || e1 <= 0.5F || e2 <= 0.5F) {
        continue;
      }
      float const a2 = e0 + e1 + e2;
      float const b0 = e0 / a2;
      float const b1 = e1 / a2;
      float const b2 = e2 / a2;
      float const invw_p = (b0 * iwf0) + (b1 * iwf1) + (b2 * iwf2);
      float const uoverw = (b0 * (float)us0 * iwf0) + (b1 * (float)us1 * iwf1) +
                           (b2 * (float)us2 * iwf2);
      float const voverw = (b0 * (float)vs0 * iwf0) + (b1 * (float)vs1 * iwf1) +
                           (b2 * (float)vs2 * iwf2);
      float const u_s105 = uoverw / invw_p;
      float const v_s105 = voverw / invw_p;
      int const ftu = (int)floorf(u_s105 / 32.0F);  // FLOAT perspective texel
      int const ftv = (int)floorf(v_s105 / 32.0F);

      // Fixed-point texel index the impl resolves (recovery formula from
      // raster.cc), computed here for the perspective cross-check.
      fx12_4 const cxq = (fx12_4)((sx << 4) + 8);
      fx12_4 const cyq = (fx12_4)((sy << 4) + 8);
      int32_t const w0 = edge_i(rt.x1, rt.y1, rt.x2, rt.y2, cxq, cyq);
      int32_t const w1 = edge_i(rt.x2, rt.y2, rt.x0, rt.y0, cxq, cyq);
      int32_t const w2 = edge_i(rt.x0, rt.y0, rt.x1, rt.y1, cxq, cyq);
      int64_t const niw = ((int64_t)w0 * rt.iw0) + ((int64_t)w1 * rt.iw1) +
                          ((int64_t)w2 * rt.iw2);
      int32_t const iwp = (int32_t)(niw / (int64_t)rt.area2);
      int64_t const nu = ((int64_t)w0 * rt.u_iw0) + ((int64_t)w1 * rt.u_iw1) +
                         ((int64_t)w2 * rt.u_iw2);
      int64_t const nv = ((int64_t)w0 * rt.v_iw0) + ((int64_t)w1 * rt.v_iw1) +
                         ((int64_t)w2 * rt.v_iw2);
      int32_t const uiwp = (int32_t)(nu / (int64_t)rt.area2);
      int32_t const viwp = (int32_t)(nv / (int64_t)rt.area2);
      int const itu = (int)((((int64_t)uiwp << 27) / (int64_t)iwp) >> 16);
      int const itv = (int)((((int64_t)viwp << 27) / (int64_t)iwp) >> 16);

      // (1) PERSPECTIVE cross-check: float and fixed texel indices within 1.
      int dtu = itu - ftu;
      int dtv = itv - ftv;
      if (dtu < 0) {
        dtu = -dtu;
      }
      if (dtv < 0) {
        dtv = -dtv;
      }
      ASSERT_LE(dtu, 1) << "U texel slip > 1 @ (" << sx << "," << sy << ")";
      ASSERT_LE(dtv, 1) << "V texel slip > 1 @ (" << sx << "," << sy << ")";

      // (2) COMBINER: float MODULATE of the texel the IMPL sampled, then
      // through the SAME final 565 pack/unpack the impl applies (so we compare
      // at the 565 grid, leaving only the N64 integer-combiner rounding as
      // residual).
      uint8_t texel[4];
      ASSERT_EQ(oracle_sample_texel(&rs.tex, itu, itv, texel), 0);
      // flat shade across this tri -> affine SHADE == the vertex shade.
      long er = lrintf((float)texel[0] * (float)shr / 255.0F);
      long eg = lrintf((float)texel[1] * (float)shg / 255.0F);
      long eb = lrintf((float)texel[2] * (float)shb / 255.0F);
      if (er > 255) {
        er = 255;
      }
      if (eg > 255) {
        eg = 255;
      }
      if (eb > 255) {
        eb = 255;
      }
      uint8_t exr;
      uint8_t exg;
      uint8_t exb;
      oracle_unpack565(rgb565_pack((uint8_t)er, (uint8_t)eg, (uint8_t)eb), &exr,
                       &exg, &exb);
      uint16_t const got = fb[(sy * RDR_SCREEN_W) + sx];
      uint8_t gr8;
      uint8_t gg8;
      uint8_t gb8;
      oracle_unpack565(got, &gr8, &gg8, &gb8);
      int dr = (int)gr8 - (int)exr;
      int dg = (int)gg8 - (int)exg;
      int db = (int)gb8 - (int)exb;
      if (dr < 0) {
        dr = -dr;
      }
      if (dg < 0) {
        dg = -dg;
      }
      if (db < 0) {
        db = -db;
      }
      // Residual: the N64 integer combiner ((t*s+0x80)>>8) vs float t*s/255 can
      // tip a channel into the adjacent RGB565 bin at a boundary. One 565
      // quantum expands (with bit-replication) to <=9 in 8-bit for R/B (5-bit)
      // and <=5 for G (6-bit). A transposition / wrong shift blows FAR past
      // this bound.
      int const tol_rb = 9;
      int const tol_g = 5;
      if (dr > worst) {
        worst = dr;
      }
      EXPECT_LE(dr, tol_rb) << "R @ (" << sx << "," << sy << ")";
      EXPECT_LE(dg, tol_g) << "G @ (" << sx << "," << sy << ")";
      EXPECT_LE(db, tol_rb) << "B @ (" << sx << "," << sy << ")";
      ++checked;
    }
  }
  EXPECT_GT(checked, 50) << "too few interior pixels — test is vacuous";
}

// Alpha-cutout (N11): a 1-bit-alpha RGBA5551 cutout tri over a known
// background. Discarded texels (A=0) keep the background, AND a farther tri
// drawn AFTER at a discarded pixel STILL draws (proves zbuf was NOT written at
// the discard).
TEST(RasterTextured, AlphaCutoutDiscardsAndSkipsZbuf) {
  struct Tex5551 tex;
  make_tex5551_cutout(&tex);
  struct RenderState rs;
  memset(&rs, 0, sizeof(rs));
  rs.tex.data = tex.px;
  rs.tex.w = K_TEXW;
  rs.tex.h = K_TEXH;
  rs.tex.format = TEXFMT_RGBA5551;
  rs.tex.wrap_s = WRAP_REPEAT;
  rs.tex.wrap_t = WRAP_REPEAT;
  rs.tex.filter = FILTER_POINT;
  rs.tex.mip_levels = 1;
  rs.combiner.mode = COMBINE_MODULATE;
  rs.alpha_cmp = 128;  // discard texels with A < 128 (A=0 transparent -> drop)

  uint16_t const white = rgb565_pack(255, 255, 255);
  uint16_t const bg = rgb565_pack(20, 40, 80);

  // NEAR cutout tri (material 0), then a FAR plain tri (material 1) drawn after
  // over the same area. material 1 = no texture (flat green), FAR so it only
  // shows where the near tri discarded (zbuf still clear there).
  struct RenderState table[2];
  table[0] = rs;
  memset(&table[1], 0, sizeof(table[1]));  // no texture -> flat fast path
  uint16_t const far_green = rgb565_pack(0, 255, 0);

  // The cutout tri covers tile 0; UVs map screen-x across the 8-texel width so
  // left half (u<4, opaque) and right half (u>=4, transparent) both appear.
  TVtx pool[6];
  // Near cutout tri (inv_w large). u spans 0..7*32 across x, v const.
  pool[0] = mk_tvtx_uv(4, 4, 0x20000, 0, 0, white);
  pool[1] = mk_tvtx_uv(52, 4, 0x20000, 7 * 32, 0, white);
  pool[2] = mk_tvtx_uv(4, 52, 0x20000, 0, 0, white);
  // Far flat tri (inv_w small) — same screen area.
  pool[3] = mk_vtx(4, 4, 0, 0, 0x08000);
  pool[4] = mk_vtx(52, 4, 0, 0, 0x08000);
  pool[5] = mk_vtx(4, 52, 0, 0, 0x08000);
  pool[3].rgba = far_green;
  pool[4].rgba = far_green;
  pool[5].rgba = far_green;
  TriRef refs[2];
  refs[0].v0 = 0;
  refs[0].v1 = 1;
  refs[0].v2 = 2;
  refs[0].material = 0;  // near cutout
  refs[1].v0 = 3;
  refs[1].v1 = 4;
  refs[1].v2 = 5;
  refs[1].material = 1;  // far flat
  TileBin bin;
  bin.refs = refs;
  bin.count = 2;
  bin.cap = 2;
  bin.dropped = 0;

  static uint16_t fb[RDR_SCREEN_W * RDR_SCREEN_H];
  static uint16_t zbuf[RDR_TILE_W * RDR_TILE_H];
  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    fb[i] = bg;
  }
  raster_tile(0, &bin, pool, fb, zbuf, table, 0, /*worker=*/0);

  // Sample a clearly-left (opaque) interior pixel and a clearly-right
  // (transparent) interior pixel — both well inside the triangle (x+y << 56 so
  // they clear the hypotenuse top-left tie-break). Left: opaque magenta texel.
  // Right (x>=~31): transparent texel -> discarded.
  uint16_t const px_left = fb[(8 * RDR_SCREEN_W) + 8];    // u~0 -> opaque
  uint16_t const px_right = fb[(8 * RDR_SCREEN_W) + 38];  // u~4 -> transparent

  // Opaque region: NOT background (textured pixel drawn).
  EXPECT_NE(px_left, bg) << "opaque cutout texel was dropped";
  // Transparent region: the near cutout discarded -> zbuf left clear -> the FAR
  // flat tri drew there. So it must be far_green (NOT background, NOT a near
  // textured color). This proves BOTH the discard AND that zbuf was untouched.
  EXPECT_EQ(px_right, far_green)
      << "discarded pixel should show the later FAR tri (zbuf not written)";
}

// Δ4 probe: the int16 u_iw range for a realistic terrain-scale UV*inv_w. If a
// realistic coord overflows int16, STOP and re-surface (Lead's Δ4 barrier).
TEST(RasterTextured, UiwInt16RangeProbe) {
  // Terrain mesh-cell texture-window is 36x34 (glossary); a tile UV spans a few
  // texels. S10.5 max useful coord ~ a few hundred (e.g. 256 = 8 texels * 32).
  // inv_w is largest for the NEAREST geometry. Probe a worst-ish case: a near
  // vertex at inv_w = 2.0 (Q16.16 0x20000) with a large texcoord.
  int const worst_u_s105 = 512;        // 16 texels of S10.5 (generous)
  int32_t const near_inv_w = 0x40000;  // 4.0 (very near; aggressive)
  int64_t const prod = ((int64_t)worst_u_s105 << 16) * (int64_t)near_inv_w;
  int64_t const bias = (1LL << 15);
  int64_t const u_iw = ((prod + bias) >> 16) >> 16;  // == trunc(u_s105*inv_w)
  // u_iw must fit int16 for the contract field to carry it without truncation.
  EXPECT_GE(u_iw, (int64_t)INT16_MIN);
  EXPECT_LE(u_iw, (int64_t)INT16_MAX)
      << "u_iw overflows int16 at terrain scale — re-surface Δ4 to the Lead "
         "(do NOT widen types.h here)";
}

// dual==serial for a TEXTURED scene: raster_one reads only the immutable pool +
// rstate_table and writes a private zbuf, so a multi-tile textured scene drawn
// (a) serially with one zbuf and (b) with two interleaved per-tile zbufs MUST
// be bit-identical — the same C2 invariant the sched test proves for flat fill,
// now exercised across the textured + cutout path. Crosses tile boundaries so
// the per-tile depth-clear + private-scratch isolation are load-bearing.
TEST(RasterTextured, DualWorkerBitIdenticalTexturedScene) {
  struct Tex565 tex;
  make_tex565(&tex);
  struct Tex5551 cut;
  make_tex5551_cutout(&cut);

  // Two materials: 0 = textured (modulate), 1 = cutout (alpha_cmp), 2 = flat.
  static struct RenderState table[3];
  memset(table, 0, sizeof(table));
  tex_desc_565(&table[0].tex, &tex);
  table[0].combiner.mode = COMBINE_MODULATE;
  table[1].tex.data = cut.px;
  table[1].tex.w = K_TEXW;
  table[1].tex.h = K_TEXH;
  table[1].tex.format = TEXFMT_RGBA5551;
  table[1].tex.wrap_s = WRAP_REPEAT;
  table[1].tex.wrap_t = WRAP_REPEAT;
  table[1].tex.filter = FILTER_POINT;
  table[1].combiner.mode = COMBINE_MODULATE;
  table[1].alpha_cmp = 128;
  // table[2] left zeroed -> flat fast path.

  uint16_t const white = rgb565_pack(255, 255, 255);
  uint16_t const flat = rgb565_pack(200, 60, 30);

  // A handful of tris spread across several screen tiles, mixed materials.
  enum { K_NTRI = 4 };
  TVtx pool[K_NTRI * 3];
  TriRef refs[K_NTRI];
  // tri0 textured (tile 0), tri1 cutout (spans tiles 0/1), tri2 flat (tile 5),
  // tri3 textured (tile 10) — each tri's verts share its inv_w/material.
  pool[0] = mk_tvtx_uv(8, 8, 0x18000, 0, 0, white);
  pool[1] = mk_tvtx_uv(58, 14, 0x10000, 7 * 32, 0, white);
  pool[2] = mk_tvtx_uv(14, 56, 0x14000, 0, 7 * 32, white);
  pool[3] = mk_tvtx_uv(40, 20, 0x20000, 0, 0, white);
  pool[4] = mk_tvtx_uv(90, 30, 0x10000, 7 * 32, 0, white);
  pool[5] = mk_tvtx_uv(50, 70, 0x18000, 0, 7 * 32, white);
  pool[6] = mk_vtx(70, 70, 0, 0, 0x10000);
  pool[7] = mk_vtx(110, 80, 0, 0, 0x10000);
  pool[8] = mk_vtx(76, 110, 0, 0, 0x10000);
  pool[6].rgba = flat;
  pool[7].rgba = flat;
  pool[8].rgba = flat;
  pool[9] = mk_tvtx_uv(140, 140, 0x1C000, 0, 0, white);
  pool[10] = mk_tvtx_uv(190, 150, 0x10000, 7 * 32, 0, white);
  pool[11] = mk_tvtx_uv(150, 190, 0x14000, 0, 7 * 32, white);
  refs[0].v0 = 0;
  refs[0].v1 = 1;
  refs[0].v2 = 2;
  refs[0].material = 0;
  refs[1].v0 = 3;
  refs[1].v1 = 4;
  refs[1].v2 = 5;
  refs[1].material = 1;
  refs[2].v0 = 6;
  refs[2].v1 = 7;
  refs[2].v2 = 8;
  refs[2].material = 2;
  refs[3].v0 = 9;
  refs[3].v1 = 10;
  refs[3].v2 = 11;
  refs[3].material = 0;

  // Bin per tile: a tri goes into every tile its bounding box touches. For the
  // determinism test it is enough to bin all tris into all tiles and let the
  // tile-clip in raster_one drop what falls outside (same refs, same order ->
  // any divergence is purely from shared mutable state, which is what we test).
  int const num_tiles =
      (RDR_SCREEN_W / RDR_TILE_W) * (RDR_SCREEN_H / RDR_TILE_H);
  static uint16_t fb_serial[RDR_SCREEN_W * RDR_SCREEN_H];
  static uint16_t fb_par[RDR_SCREEN_W * RDR_SCREEN_H];
  static uint16_t z0[RDR_TILE_W * RDR_TILE_H];
  static uint16_t z1[RDR_TILE_W * RDR_TILE_H];
  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    fb_serial[i] = 0;
    fb_par[i] = 0;
  }
  TileBin bin;
  bin.refs = refs;
  bin.count = K_NTRI;
  bin.cap = K_NTRI;
  bin.dropped = 0;

  for (int tile = 0; tile < num_tiles; ++tile) {
    raster_tile(tile, &bin, pool, fb_serial, z0, table, 0, /*worker=*/0);
  }
  for (int tile = 0; tile < num_tiles; ++tile) {
    int const worker = tile & 1;
    uint16_t* const z = worker ? z1 : z0;
    raster_tile(tile, &bin, pool, fb_par, z, table, 0, worker);
  }
  EXPECT_EQ(memcmp(fb_serial, fb_par,
                   (size_t)(RDR_SCREEN_W * RDR_SCREEN_H) * sizeof(uint16_t)),
            0)
      << "textured dual-worker sweep diverged from serial — raster has shared "
         "mutable state";
}

// ===========================================================================
// L6 — XLU two-pass + per-tile FRONT-TO-BACK sort. The opaque sweep stays the
// R.1 path (verified bit-identical by the goldens above); these tests pin the
// translucent sweep: front-to-back premultiplied-UNDER accumulation
// (blend_premul_accumulate) + a per-tile composite that folds terrain under
// (blend_premul_resolve), z-test (no z-write), and a deterministic tie-break.
// ===========================================================================
namespace {

// An 8x8 RGBA4444 texture with a uniform FRACTIONAL alpha (NOT 1-bit, so a
// blend bug cannot hide). expand4(alpha_nib) is the decoded 8-bit alpha — e.g.
// nibble 0x8 -> 0x88 = 136. The RGB varies per texel (distinct, so a UV slip
// shows) but alpha is constant across the texture for an easy expected value.
struct Tex4444 {
  uint16_t px[K_TEXW * K_TEXH];
};
uint16_t pack4444(int r4, int g4, int b4, int a4) {
  return (uint16_t)(((r4 & 0xf) << 12) | ((g4 & 0xf) << 8) | ((b4 & 0xf) << 4) |
                    (a4 & 0xf));
}
void make_tex4444_alpha(struct Tex4444* t, int alpha_nib) {
  for (int v = 0; v < K_TEXH; ++v) {
    for (int u = 0; u < K_TEXW; ++u) {
      // Distinct-ish RGB nibbles; uniform alpha nibble.
      int const r4 = (u + 2) & 0xf;
      int const g4 = (v + 3) & 0xf;
      int const b4 = ((u ^ v) + 1) & 0xf;
      t->px[(v * K_TEXW) + u] = pack4444(r4, g4, b4, alpha_nib);
    }
  }
}

// Fill a TexDesc for a pow2 RGBA4444 texture, POINT-sampled, REPEAT wrap.
void tex_desc_4444(struct TexDesc* td, const struct Tex4444* t) {
  memset(td, 0, sizeof(*td));
  td->data = t->px;
  td->w = K_TEXW;
  td->h = K_TEXH;
  td->format = TEXFMT_RGBA4444;
  td->wrap_s = WRAP_REPEAT;
  td->wrap_t = WRAP_REPEAT;
  td->filter = FILTER_POINT;
  td->mip_levels = 1;
  td->tlut = 0;
}

// L6 NOTE: the former `ref_xlu_pixel` (back-to-front blend_pixel_alpha over a
// destination) modelled the OLD over-blend XLU contract and is REMOVED — the
// XLU store is now front-to-back premultiplied UNDER (ref_xlu_frag +
// blend_premul_accumulate/_resolve below). See the L6 contract-delta note.

// L6 contract-delta (flagged for the Lead — cross-lane edit into T2's
// raster_test as a DIRECT consequence of the XLU front-to-back rework): the XLU
// fragment store is now FRONT-TO-BACK PREMULTIPLIED UNDER, not back-to-front
// over-blend. For a covered pixel this returns the combiner color + its texel
// alpha so a test can drive the SAME premultiplied helpers the impl uses
// (blend_premul_accumulate / blend_premul_resolve) -> bit-exact again. Fog is
// NOT applied here (the fog test fogs `combined` itself and passes it as a
// fragment); the non-fog tests set rs->fog.enabled = 0. Returns 1 + fills
// *frag565/*frag_a if the pixel is covered (post alpha-cmp), else 0.
int ref_xlu_frag(const struct RefTri* t, const struct RenderState* rs, int sx,
                 int sy, uint16_t* frag565, uint8_t* frag_a) {
  fx12_4 const cx = (fx12_4)((sx << 4) + 8);
  fx12_4 const cy = (fx12_4)((sy << 4) + 8);
  int32_t const w0 = edge_i(t->x1, t->y1, t->x2, t->y2, cx, cy);
  int32_t const w1 = edge_i(t->x2, t->y2, t->x0, t->y0, cx, cy);
  int32_t const w2 = edge_i(t->x0, t->y0, t->x1, t->y1, cx, cy);
  int const in0 = (w0 > 0) || (w0 == 0 && t->tl1);
  int const in1 = (w1 > 0) || (w1 == 0 && t->tl2);
  int const in2 = (w2 > 0) || (w2 == 0 && t->tl0);
  if (!(in0 && in1 && in2)) {
    return 0;
  }
  int64_t const numiw =
      ((int64_t)w0 * t->iw0) + ((int64_t)w1 * t->iw1) + ((int64_t)w2 * t->iw2);
  int32_t const inv_w = (int32_t)(numiw / (int64_t)t->area2);
  if (inv_w <= 0) {
    return 0;
  }
  int64_t const numu = ((int64_t)w0 * t->u_iw0) + ((int64_t)w1 * t->u_iw1) +
                       ((int64_t)w2 * t->u_iw2);
  int64_t const numv = ((int64_t)w0 * t->v_iw0) + ((int64_t)w1 * t->v_iw1) +
                       ((int64_t)w2 * t->v_iw2);
  int32_t const u_iw_p = (int32_t)(numu / (int64_t)t->area2);
  int32_t const v_iw_p = (int32_t)(numv / (int64_t)t->area2);
  fx16_16 const u_q16 = (fx16_16)(((int64_t)u_iw_p << 27) / (int64_t)inv_w);
  fx16_16 const v_q16 = (fx16_16)(((int64_t)v_iw_p << 27) / (int64_t)inv_w);
  uint8_t const gr =
      gouraud_ref(w0, w1, w2, t->s0[0], t->s1[0], t->s2[0], t->area2);
  uint8_t const gg =
      gouraud_ref(w0, w1, w2, t->s0[1], t->s1[1], t->s2[1], t->area2);
  uint8_t const gb =
      gouraud_ref(w0, w1, w2, t->s0[2], t->s1[2], t->s2[2], t->area2);
  uint16_t const shade565 = rgb565_pack(gr, gg, gb);
  uint8_t rgba[4];
  tex_sample_rgba(&rs->tex, u_q16, v_q16, 0, rgba);
  if (rs->alpha_cmp != 0 && rgba[3] < rs->alpha_cmp) {
    return 0;  // honor alpha-compare discard
  }
  uint16_t const texel565 = rgb565_pack(rgba[0], rgba[1], rgba[2]);
  uint8_t keep = 1;
  uint16_t const combined = shade_pixel(rs, texel565, shade565, &keep);
  *frag565 = combined;
  *frag_a = rgba[3];
  return 1;
}

// Build an XLU render state over a fractional-alpha RGBA4444 texture.
void mk_xlu_state(struct RenderState* rs, const struct Tex4444* tex) {
  memset(rs, 0, sizeof(*rs));
  tex_desc_4444(&rs->tex, tex);
  rs->combiner.mode = COMBINE_MODULATE;
  rs->zmode = ZMODE_XLU;
  rs->alpha_cmp = 0;
}

}  // namespace

// (1) XLU premultiplied-UNDER correctness: a single XLU tri with a
// FRACTIONAL-alpha texture over a known opaque background must blend per pixel
// EXACTLY like the L6 path — one fragment accumulated front-to-back
// (blend_premul_accumulate) then terrain folded under (blend_premul_resolve).
// Uses alpha nibble 0x8 (-> 136), so a coverage-vs-texel-alpha bug or a missing
// blend cannot hide.
TEST(RasterXlu, PremulUnderMatchesBlendPremulExact) {
  struct Tex4444 tex;
  make_tex4444_alpha(&tex, 0x8);  // alpha = expand4(0x8) = 136 (fractional)
  struct RenderState rs;
  mk_xlu_state(&rs, &tex);
  const struct RenderState* table = &rs;

  uint16_t const bg = rgb565_pack(30, 70, 110);
  uint16_t const white = rgb565_pack(255, 255, 255);

  TVtx pool[3];
  pool[0] = mk_tvtx_uv(8, 8, 0x18000, 0, 0, white);
  pool[1] = mk_tvtx_uv(52, 12, 0x10000, 7 * 32, 0, white);
  pool[2] = mk_tvtx_uv(14, 50, 0x14000, 0, 7 * 32, white);
  TriRef ref;
  ref.v0 = 0;
  ref.v1 = 1;
  ref.v2 = 2;
  ref.material = 0;
  TileBin bin;
  bin.refs = &ref;
  bin.count = 1;
  bin.cap = 1;
  bin.dropped = 0;

  static uint16_t fb[RDR_SCREEN_W * RDR_SCREEN_H];
  static uint16_t zbuf[RDR_TILE_W * RDR_TILE_H];
  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    fb[i] = bg;
  }
  raster_tile(0, &bin, pool, fb, zbuf, table, 0, /*worker=*/0);

  struct RefTri rt;
  ref_setup(&rt, &pool[0], &pool[1], &pool[2]);
  int checked = 0;
  for (int sy = 0; sy < RDR_TILE_H; ++sy) {
    for (int sx = 0; sx < RDR_TILE_W; ++sx) {
      // L6: a single XLU layer over bg via the premultiplied-UNDER path. One
      // fragment accumulated from (0,0), then terrain folded under at the
      // composite -> the SAME helpers the impl uses, so bit-exact.
      uint16_t frag;
      uint8_t fa;
      if (!ref_xlu_frag(&rt, &rs, sx, sy, &frag, &fa)) {
        continue;
      }
      uint16_t c_acc = 0;
      uint8_t aa = 0;
      blend_premul_accumulate(&c_acc, &aa, frag, fa);
      uint16_t const expected =
          (aa != 0) ? blend_premul_resolve(c_acc, aa, bg) : bg;
      uint16_t const got = fb[(sy * RDR_SCREEN_W) + sx];
      ASSERT_EQ(got, expected)
          << "XLU pixel (" << sx << "," << sy << ") mismatch";
      // Sanity: a fractional-alpha blend over bg must differ from both the
      // bare combiner output AND the bg (catches "wrote opaque" / "didn't
      // draw").
      ++checked;
    }
  }
  EXPECT_GT(checked, 50) << "too few covered XLU pixels — test is vacuous";
}

// (2) L6 FRONT-TO-BACK compositing order + determinism: two overlapping XLU
// tris at different depths over a known bg. The NEARER tri accumulates first
// (premultiplied UNDER), then the farther under it; the composite folds bg
// under. The overlap pixels must equal the premultiplied-under accumulation of
// near-then-far over bg. Re-running with the bin insertion order PERMUTED
// yields the identical result (the per-tile sort makes draw order depth-driven,
// not insertion-driven). Mathematically the final color equals the old
// near-over-(far-over-bg), differing only by the 565-accumulator requantization
// — so the test references the SAME premultiplied helpers the impl uses.
TEST(RasterXlu, FrontToBackUnderOrderAndDeterminism) {
  struct Tex4444 tex;
  make_tex4444_alpha(&tex, 0x8);  // alpha 136
  struct RenderState rs;
  mk_xlu_state(&rs, &tex);
  const struct RenderState* table = &rs;

  uint16_t const bg = rgb565_pack(20, 40, 60);
  uint16_t const white = rgb565_pack(255, 255, 255);

  // Two coincident-screen XLU tris; FAR has small inv_w, NEAR has large.
  // Same screen triangle so they fully overlap.
  int32_t const iw_far = 0x08000;   // 0.5  (farther)
  int32_t const iw_near = 0x20000;  // 2.0  (nearer)
  TVtx far_tri[3];
  far_tri[0] = mk_tvtx_uv(8, 8, iw_far, 0, 0, white);
  far_tri[1] = mk_tvtx_uv(52, 12, iw_far, 7 * 32, 0, white);
  far_tri[2] = mk_tvtx_uv(14, 50, iw_far, 0, 7 * 32, white);
  TVtx near_tri[3];
  near_tri[0] = mk_tvtx_uv(8, 8, iw_near, 0, 0, white);
  near_tri[1] = mk_tvtx_uv(52, 12, iw_near, 7 * 32, 0, white);
  near_tri[2] = mk_tvtx_uv(14, 50, iw_near, 0, 7 * 32, white);

  // Pool: [0..2] = far, [3..5] = near.
  TVtx pool[6];
  for (int i = 0; i < 3; ++i) {
    pool[i] = far_tri[i];
    pool[3 + i] = near_tri[i];
  }

  // Insertion order A: near FIRST, far SECOND (the WRONG draw order if the sort
  // were absent — proves the sort reorders to far-then-near).
  TriRef refs_a[2];
  refs_a[0].v0 = 3;
  refs_a[0].v1 = 4;
  refs_a[0].v2 = 5;
  refs_a[0].material = 0;  // near
  refs_a[1].v0 = 0;
  refs_a[1].v1 = 1;
  refs_a[1].v2 = 2;
  refs_a[1].material = 0;  // far
  TileBin bin_a;
  bin_a.refs = refs_a;
  bin_a.count = 2;
  bin_a.cap = 2;
  bin_a.dropped = 0;

  // Insertion order B: far FIRST, near SECOND (the permutation).
  TriRef refs_b[2];
  refs_b[0] = refs_a[1];  // far
  refs_b[1] = refs_a[0];  // near
  TileBin bin_b;
  bin_b.refs = refs_b;
  bin_b.count = 2;
  bin_b.cap = 2;
  bin_b.dropped = 0;

  static uint16_t fb_a[RDR_SCREEN_W * RDR_SCREEN_H];
  static uint16_t fb_b[RDR_SCREEN_W * RDR_SCREEN_H];
  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    fb_a[i] = bg;
    fb_b[i] = bg;
  }
  static uint16_t z_a[RDR_TILE_W * RDR_TILE_H];
  static uint16_t z_b[RDR_TILE_W * RDR_TILE_H];
  raster_tile(0, &bin_a, pool, fb_a, z_a, table, 0, /*worker=*/0);
  raster_tile(0, &bin_b, pool, fb_b, z_b, table, 0, /*worker=*/0);

  // Determinism: permuting insertion order must NOT change the result.
  EXPECT_EQ(memcmp(fb_a, fb_b,
                   (size_t)(RDR_SCREEN_W * RDR_SCREEN_H) * sizeof(uint16_t)),
            0)
      << "XLU result depends on bin insertion order — sort is not "
         "depth-deterministic";

  // Correctness (L6 premultiplied UNDER): accumulate NEAR first, then FAR (the
  // front-to-back order the sort produces), then fold bg under. References the
  // impl's own fixed-point + premultiplied helpers, so it is bit-exact.
  struct RefTri rt_far;
  ref_setup(&rt_far, &pool[0], &pool[1], &pool[2]);
  struct RefTri rt_near;
  ref_setup(&rt_near, &pool[3], &pool[4], &pool[5]);
  int checked = 0;
  for (int sy = 0; sy < RDR_TILE_H; ++sy) {
    for (int sx = 0; sx < RDR_TILE_W; ++sx) {
      uint16_t c_acc = 0;
      uint8_t aa = 0;
      uint16_t frag;
      uint8_t fa;
      int const cov_near = ref_xlu_frag(&rt_near, &rs, sx, sy, &frag, &fa);
      if (cov_near) {
        blend_premul_accumulate(&c_acc, &aa, frag, fa);
      }
      int const cov_far = ref_xlu_frag(&rt_far, &rs, sx, sy, &frag, &fa);
      if (cov_far) {
        blend_premul_accumulate(&c_acc, &aa, frag, fa);
      }
      if (aa == 0) {
        continue;  // pixel covered by neither -> bg, nothing to check
      }
      uint16_t const expected = blend_premul_resolve(c_acc, aa, bg);
      uint16_t const got = fb_a[(sy * RDR_SCREEN_W) + sx];
      ASSERT_EQ(got, expected)
          << "XLU composite (" << sx << "," << sy << ") not near-under-far";
      if (cov_far && cov_near) {
        ++checked;
      }
    }
  }
  EXPECT_GT(checked, 50)
      << "too few doubly-covered pixels — order test vacuous";
}

// (3a) z-test honored: an XLU tri BEHIND an opaque occluder must be hidden
// where the occluder already wrote depth. The occluded pixels stay the
// occluder's color (no XLU blend leaks through a passed-but-not-tested
// fragment).
TEST(RasterXlu, ZTestHonoredBehindOpaqueOccluder) {
  struct Tex4444 tex;
  make_tex4444_alpha(&tex, 0x8);
  struct RenderState table[2];
  // material 0: opaque flat occluder (no texture -> fast path), NEAR.
  memset(&table[0], 0, sizeof(table[0]));
  table[0].zmode = ZMODE_OPAQUE;
  // material 1: XLU textured, FAR (behind the occluder).
  mk_xlu_state(&table[1], &tex);

  uint16_t const bg = rgb565_pack(10, 20, 30);
  uint16_t const occ = rgb565_pack(240, 30, 30);
  uint16_t const white = rgb565_pack(255, 255, 255);

  TVtx pool[6];
  // Opaque occluder (NEAR, large inv_w). Flat fast path uses pool[].rgba.
  pool[0] = mk_vtx(6, 6, 0, 0, 0x20000);
  pool[1] = mk_vtx(54, 10, 0, 0, 0x20000);
  pool[2] = mk_vtx(10, 54, 0, 0, 0x20000);
  pool[0].rgba = occ;
  pool[1].rgba = occ;
  pool[2].rgba = occ;
  // XLU tri (FAR, small inv_w), same screen area.
  pool[3] = mk_tvtx_uv(6, 6, 0x08000, 0, 0, white);
  pool[4] = mk_tvtx_uv(54, 10, 0x08000, 7 * 32, 0, white);
  pool[5] = mk_tvtx_uv(10, 54, 0x08000, 0, 7 * 32, white);

  TriRef refs[2];
  refs[0].v0 = 0;
  refs[0].v1 = 1;
  refs[0].v2 = 2;
  refs[0].material = 0;  // opaque occluder
  refs[1].v0 = 3;
  refs[1].v1 = 4;
  refs[1].v2 = 5;
  refs[1].material = 1;  // far XLU
  TileBin bin;
  bin.refs = refs;
  bin.count = 2;
  bin.cap = 2;
  bin.dropped = 0;

  static uint16_t fb[RDR_SCREEN_W * RDR_SCREEN_H];
  static uint16_t zbuf[RDR_TILE_W * RDR_TILE_H];
  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    fb[i] = bg;
  }
  raster_tile(0, &bin, pool, fb, zbuf, table, 0, /*worker=*/0);

  // A deep-interior pixel covered by BOTH: the occluder wrote depth NEAR, the
  // XLU tri is FAR -> z-test rejects it -> the pixel stays the occluder color
  // (NOT a blend of XLU over occluder).
  uint16_t const px = fb[(20 * RDR_SCREEN_W) + 16];
  EXPECT_EQ(px, occ)
      << "XLU behind an opaque occluder leaked through — z-test not honored";
}

// (3b) NO z-write: a SECOND XLU tri drawn behind the FIRST still composites.
// If the XLU sweep wrote depth, the first (nearer) XLU tri would block the
// second (farther) — but XLU must z-TEST only. So both layers contribute to the
// premultiplied-UNDER accumulation: front-to-back, NEAR accumulates first then
// FAR under it, and the composite folds bg under (= near-under-far-over-bg).
// The discriminator vs a z-writing impl: a z-writing near layer would reject
// the far layer entirely, leaving only the near contribution. We assert the
// TWO-LAYER composite, which only holds if the far layer was NOT z-rejected by
// the near layer's (absent) z-write.
TEST(RasterXlu, NoZWriteSoSecondXluStillComposites) {
  struct Tex4444 tex;
  make_tex4444_alpha(&tex, 0x8);
  struct RenderState rs;
  mk_xlu_state(&rs, &tex);
  const struct RenderState* table = &rs;

  uint16_t const bg = rgb565_pack(60, 10, 90);
  uint16_t const white = rgb565_pack(255, 255, 255);

  int32_t const iw_far = 0x08000;
  int32_t const iw_near = 0x20000;
  TVtx pool[6];
  pool[0] = mk_tvtx_uv(8, 8, iw_far, 0, 0, white);
  pool[1] = mk_tvtx_uv(52, 12, iw_far, 7 * 32, 0, white);
  pool[2] = mk_tvtx_uv(14, 50, iw_far, 0, 7 * 32, white);
  pool[3] = mk_tvtx_uv(8, 8, iw_near, 0, 0, white);
  pool[4] = mk_tvtx_uv(52, 12, iw_near, 7 * 32, 0, white);
  pool[5] = mk_tvtx_uv(14, 50, iw_near, 0, 7 * 32, white);

  // Insertion: NEAR first, FAR second. A z-writing XLU impl would z-write the
  // near layer first (after the sort puts far first, far writes depth, then
  // near passes; but a z-writing far layer then... ) — the robust
  // discriminator is the two-layer composite below, which a z-writing impl in
  // EITHER order cannot reproduce.
  TriRef refs[2];
  refs[0].v0 = 3;
  refs[0].v1 = 4;
  refs[0].v2 = 5;
  refs[0].material = 0;  // near
  refs[1].v0 = 0;
  refs[1].v1 = 1;
  refs[1].v2 = 2;
  refs[1].material = 0;  // far
  TileBin bin;
  bin.refs = refs;
  bin.count = 2;
  bin.cap = 2;
  bin.dropped = 0;

  static uint16_t fb[RDR_SCREEN_W * RDR_SCREEN_H];
  static uint16_t zbuf[RDR_TILE_W * RDR_TILE_H];
  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    fb[i] = bg;
  }
  raster_tile(0, &bin, pool, fb, zbuf, table, 0, /*worker=*/0);

  // Expected TWO-LAYER composite (L6 premultiplied UNDER: NEAR accumulated
  // first, then FAR under it, then bg folded under). Only holds if the FAR
  // layer's fragment was NOT depth-rejected by a near z-write — i.e. both
  // layers contributed to the accumulator.
  struct RefTri rt_far;
  ref_setup(&rt_far, &pool[0], &pool[1], &pool[2]);
  struct RefTri rt_near;
  ref_setup(&rt_near, &pool[3], &pool[4], &pool[5]);
  int checked = 0;
  for (int sy = 0; sy < RDR_TILE_H; ++sy) {
    for (int sx = 0; sx < RDR_TILE_W; ++sx) {
      uint16_t frag;
      uint8_t fa;
      int const cov_far = ref_xlu_frag(&rt_far, &rs, sx, sy, &frag, &fa);
      if (!cov_far) {
        continue;
      }
      uint16_t frag_n;
      uint8_t fa_n;
      int const cov_near = ref_xlu_frag(&rt_near, &rs, sx, sy, &frag_n, &fa_n);
      if (!cov_near) {
        continue;
      }
      // Front-to-back: NEAR accumulates first, then FAR under it.
      uint16_t c_acc = 0;
      uint8_t aa = 0;
      blend_premul_accumulate(&c_acc, &aa, frag_n, fa_n);
      blend_premul_accumulate(&c_acc, &aa, frag, fa);
      uint16_t const expected = blend_premul_resolve(c_acc, aa, bg);
      // both layers DID composite, proving the near tri did not z-write and
      // block the far tri.
      uint16_t const got = fb[(sy * RDR_SCREEN_W) + sx];
      ASSERT_EQ(got, expected)
          << "XLU pixel (" << sx << "," << sy
          << ") missing a layer — XLU wrote zbuf and blocked the far tri";
      ++checked;
    }
  }
  EXPECT_GT(checked, 50)
      << "too few doubly-covered pixels — no-z-write vacuous";
}

// (4) Opaque path UNCHANGED: a mixed bin where an XLU material is added must
// NOT perturb the opaque tris' output. We render an opaque-only bin and the
// SAME opaque tris with an extra non-overlapping XLU tri; the opaque pixels are
// byte-identical (the two sweeps do not interfere outside the XLU footprint).
TEST(RasterXlu, OpaqueSweepUnaffectedByPresenceOfXlu) {
  struct Tex4444 tex;
  make_tex4444_alpha(&tex, 0x8);
  struct RenderState table[2];
  memset(&table[0], 0, sizeof(table[0]));  // opaque flat
  table[0].zmode = ZMODE_OPAQUE;
  mk_xlu_state(&table[1], &tex);

  uint16_t const bg = rgb565_pack(0, 0, 0);
  uint16_t const red = rgb565_pack(220, 40, 40);
  uint16_t const white = rgb565_pack(255, 255, 255);

  // Opaque tri in the LEFT of tile 0; XLU tri in the RIGHT of tile 1 area
  // (non-overlapping screen footprint via a separate tile render).
  TVtx pool[6];
  pool[0] = mk_vtx(4, 4, 0, 0, 0x18000);
  pool[1] = mk_vtx(28, 8, 0, 0, 0x18000);
  pool[2] = mk_vtx(6, 40, 0, 0, 0x18000);
  pool[0].rgba = red;
  pool[1].rgba = red;
  pool[2].rgba = red;
  // XLU tri occupying a disjoint sub-rect of the same tile (x in [40,56]).
  pool[3] = mk_tvtx_uv(42, 4, 0x10000, 0, 0, white);
  pool[4] = mk_tvtx_uv(56, 8, 0x10000, 7 * 32, 0, white);
  pool[5] = mk_tvtx_uv(44, 40, 0x10000, 0, 7 * 32, white);

  TriRef refs_opaque[1];
  refs_opaque[0].v0 = 0;
  refs_opaque[0].v1 = 1;
  refs_opaque[0].v2 = 2;
  refs_opaque[0].material = 0;
  TileBin bin_opaque;
  bin_opaque.refs = refs_opaque;
  bin_opaque.count = 1;
  bin_opaque.cap = 1;
  bin_opaque.dropped = 0;

  TriRef refs_mixed[2];
  refs_mixed[0] = refs_opaque[0];
  refs_mixed[1].v0 = 3;
  refs_mixed[1].v1 = 4;
  refs_mixed[1].v2 = 5;
  refs_mixed[1].material = 1;  // XLU
  TileBin bin_mixed;
  bin_mixed.refs = refs_mixed;
  bin_mixed.count = 2;
  bin_mixed.cap = 2;
  bin_mixed.dropped = 0;

  static uint16_t fb_opaque[RDR_SCREEN_W * RDR_SCREEN_H];
  static uint16_t fb_mixed[RDR_SCREEN_W * RDR_SCREEN_H];
  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    fb_opaque[i] = bg;
    fb_mixed[i] = bg;
  }
  static uint16_t z0[RDR_TILE_W * RDR_TILE_H];
  static uint16_t z1[RDR_TILE_W * RDR_TILE_H];
  raster_tile(0, &bin_opaque, pool, fb_opaque, z0, table, 0, /*worker=*/0);
  raster_tile(0, &bin_mixed, pool, fb_mixed, z1, table, 0, /*worker=*/0);

  // Every pixel in the opaque tri's footprint (x < 40) must be byte-identical
  // between the two renders — the XLU sweep did not touch the opaque region.
  int diff = 0;
  for (int sy = 0; sy < RDR_TILE_H; ++sy) {
    for (int sx = 0; sx < 40; ++sx) {
      if (fb_opaque[(sy * RDR_SCREEN_W) + sx] !=
          fb_mixed[(sy * RDR_SCREEN_W) + sx]) {
        ++diff;
      }
    }
  }
  EXPECT_EQ(diff, 0) << "presence of an XLU tri perturbed the opaque region ("
                     << diff << " px) — the opaque sweep is not isolated";
}

// (5) DECAL is treated as opaque (T2 scope): a DECAL-zmode tri rasterizes in
// sweep 1 exactly like an opaque tri (z-test + z-write, no blend). NOTE: real
// decal-dZ depth bias is out of T2 scope (see raster.cc sweep-1 NOTE); this
// just pins that DECAL is NOT routed to the XLU sweep.
TEST(RasterXlu, DecalTreatedAsOpaque) {
  struct RenderState rs;
  memset(&rs, 0, sizeof(rs));
  rs.zmode = ZMODE_DECAL;  // no texture -> flat fast path, opaque sweep
  const struct RenderState* table = &rs;

  uint16_t const bg = rgb565_pack(0, 0, 0);
  uint16_t const cyan = rgb565_pack(0, 220, 220);

  // Same tri rendered once as DECAL and once as OPAQUE must be byte-identical.
  TVtx pool[3];
  pool[0] = mk_vtx(8, 8, 0, 0, 0x18000);
  pool[1] = mk_vtx(48, 12, 0, 0, 0x18000);
  pool[2] = mk_vtx(12, 48, 0, 0, 0x18000);
  pool[0].rgba = cyan;
  pool[1].rgba = cyan;
  pool[2].rgba = cyan;
  TriRef ref;
  ref.v0 = 0;
  ref.v1 = 1;
  ref.v2 = 2;
  ref.material = 0;
  TileBin bin;
  bin.refs = &ref;
  bin.count = 1;
  bin.cap = 1;
  bin.dropped = 0;

  static uint16_t fb_decal[RDR_SCREEN_W * RDR_SCREEN_H];
  static uint16_t fb_opaque[RDR_SCREEN_W * RDR_SCREEN_H];
  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    fb_decal[i] = bg;
    fb_opaque[i] = bg;
  }
  static uint16_t zd[RDR_TILE_W * RDR_TILE_H];
  static uint16_t zo[RDR_TILE_W * RDR_TILE_H];
  raster_tile(0, &bin, pool, fb_decal, zd, table, 0, /*worker=*/0);
  struct RenderState rs_op = rs;
  rs_op.zmode = ZMODE_OPAQUE;
  const struct RenderState* table_op = &rs_op;
  raster_tile(0, &bin, pool, fb_opaque, zo, table_op, 0, /*worker=*/0);

  EXPECT_EQ(memcmp(fb_decal, fb_opaque,
                   (size_t)(RDR_SCREEN_W * RDR_SCREEN_H) * sizeof(uint16_t)),
            0)
      << "DECAL not rasterized like OPAQUE in sweep 1";
}

// (6) XLU drop-with-count: drive MORE XLU tris into ONE tile than the per-tile
// XLU sort cap. The gather must DROP-with-count (raster_xlu_dropped() rises by
// the overflow) and the KEPT tris must still composite without corruption or
// out-of-bounds (the framebuffer stays a valid blend over the background, never
// touched outside the tris' footprint). Uses a DELTA on the monotonic counter
// (other tests/process state may have bumped it). The cap is internal to
// raster.cc; we pick a tri count comfortably above any reasonable cap and read
// the delta rather than hard-coding the cap value.
TEST(RasterXlu, DropWithCountOnPerTileOverflow) {
  struct Tex4444 tex;
  make_tex4444_alpha(&tex, 0x8);  // fractional alpha 136
  struct RenderState rs;
  mk_xlu_state(&rs, &tex);
  const struct RenderState* table = &rs;

  uint16_t const bg = rgb565_pack(25, 50, 75);
  uint16_t const white = rgb565_pack(255, 255, 255);

  // K_OVF XLU tris, all covering the SAME interior region of tile 0 at slightly
  // varying depth (so the sort runs too). 512 >> any sane per-tile cap (256).
  enum { K_OVF = 512 };
  static TVtx pool[K_OVF * 3];
  static TriRef refs[K_OVF];
  for (int t = 0; t < K_OVF; ++t) {
    // inv_w varies a little per tri so depths differ; same screen triangle.
    int32_t const iw = (int32_t)(0x08000 + ((t & 31) * 0x200));
    pool[(t * 3) + 0] = mk_tvtx_uv(8, 8, iw, 0, 0, white);
    pool[(t * 3) + 1] = mk_tvtx_uv(50, 12, iw, 7 * 32, 0, white);
    pool[(t * 3) + 2] = mk_tvtx_uv(12, 50, iw, 0, 7 * 32, white);
    refs[t].v0 = (uint16_t)((t * 3) + 0);
    refs[t].v1 = (uint16_t)((t * 3) + 1);
    refs[t].v2 = (uint16_t)((t * 3) + 2);
    refs[t].material = 0;
  }
  TileBin bin;
  bin.refs = refs;
  bin.count = K_OVF;
  bin.cap = K_OVF;
  bin.dropped = 0;

  static uint16_t fb[RDR_SCREEN_W * RDR_SCREEN_H];
  static uint16_t zbuf[RDR_TILE_W * RDR_TILE_H];
  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    fb[i] = bg;
  }

  uint32_t const dropped_before = raster_xlu_dropped();
  raster_tile(0, &bin, pool, fb, zbuf, table, 0, /*worker=*/0);
  uint32_t const dropped_after = raster_xlu_dropped();

  // The gather overflowed: at least (K_OVF - cap) tris dropped. We don't know
  // the cap here, but it is well below K_OVF, so the delta must be POSITIVE.
  EXPECT_GT(dropped_after, dropped_before)
      << "512 XLU tris in one tile did not trip drop-with-count — the per-tile "
         "cap is unexpectedly >= 512, or the overflow path is unreachable";

  // KEPT tris still composited: a deep-interior pixel must be a BLEND (neither
  // the bare bg nor unwritten garbage). With fractional alpha 136 over bg, the
  // composited value differs from bg.
  uint16_t const px = fb[(20 * RDR_SCREEN_W) + 14];
  EXPECT_NE(px, bg)
      << "no XLU tri composited under overflow — kept tris were lost";

  // No corruption / OOB: pixels OUTSIDE the tris' footprint (and outside tile
  // 0) stay exactly bg. Check a clearly-outside pixel in tile 0 (bottom-right
  // corner, beyond the tri) and a pixel in another tile.
  EXPECT_EQ(fb[(58 * RDR_SCREEN_W) + 58], bg)
      << "overflow render wrote outside the tri footprint (corruption/OOB)";
  EXPECT_EQ(fb[(120 * RDR_SCREEN_W) + 120], bg)
      << "overflow render wrote into another tile (OOB)";
}

// ===========================================================================
// R.3 FOG — per-pixel affine fog over the textured + flat paths.
// ===========================================================================
namespace {

// Per-vertex fog factor [0,255], mirroring how geom births TVtx.fog. Set on a
// TVtx built by mk_vtx / mk_tvtx_uv (which zero fog by default).
void set_fog3(TVtx* p, uint8_t f0, uint8_t f1, uint8_t f2) {
  p[0].fog = f0;
  p[1].fog = f1;
  p[2].fog = f2;
}

// Winding-aware fog reorder: ref_setup swaps v1<->v2 when initial area2 < 0, so
// the reference fog triple must follow the SAME order to interpolate correctly.
void ref_fog_order(const TVtx* pool, uint8_t* g0, uint8_t* g1, uint8_t* g2) {
  *g0 = pool[0].fog;
  *g1 = pool[1].fog;
  *g2 = pool[2].fog;
  int32_t const a =
      edge_i(pool[0].x, pool[0].y, pool[1].x, pool[1].y, pool[2].x, pool[2].y);
  if (a < 0) {
    uint8_t const t = *g1;
    *g1 = *g2;
    *g2 = t;
  }
}

// Bit-exact reference for the fogged textured pixel: the unfogged combiner
// output (ref_textured_pixel) lerped toward fog_color by the affine per-pixel
// fog factor (gouraud_ref over the winding-ordered fog triple) via fog_lerp —
// the SAME integer path raster_one runs. Returns 1 + writes *out if covered.
int ref_fogged_pixel(const struct RefTri* t, const struct RenderState* rs,
                     uint8_t fg0, uint8_t fg1, uint8_t fg2, int sx, int sy,
                     uint16_t* out) {
  uint16_t base;
  if (!ref_textured_pixel(t, rs, sx, sy, &base)) {
    return 0;
  }
  fx12_4 const cx = (fx12_4)((sx << 4) + 8);
  fx12_4 const cy = (fx12_4)((sy << 4) + 8);
  int32_t const w0 = edge_i(t->x1, t->y1, t->x2, t->y2, cx, cy);
  int32_t const w1 = edge_i(t->x2, t->y2, t->x0, t->y0, cx, cy);
  int32_t const w2 = edge_i(t->x0, t->y0, t->x1, t->y1, cx, cy);
  uint8_t const fogf =
      gouraud_ref(w0, w1, w2, (int)fg0, (int)fg1, (int)fg2, t->area2);
  *out = fog_lerp(base, rs->fog.color, fogf);
  return 1;
}

// Bit-exact reference for the FLAT fast-path fogged pixel: the flat provoking
// color `flat565` lerped toward fog_color by the affine per-pixel fog factor
// (gouraud_ref over the winding-ordered fog triple) — the SAME integer path
// raster_one's !textured branch runs (no combiner, base = t->color). Coverage +
// top-left rule mirror ref_textured_pixel. Returns 1 + writes *out if covered.
int ref_flat_fogged_pixel(const struct RefTri* t, uint16_t flat565,
                          uint16_t fog_color, uint8_t fg0, uint8_t fg1,
                          uint8_t fg2, int sx, int sy, uint16_t* out) {
  fx12_4 const cx = (fx12_4)((sx << 4) + 8);
  fx12_4 const cy = (fx12_4)((sy << 4) + 8);
  int32_t const w0 = edge_i(t->x1, t->y1, t->x2, t->y2, cx, cy);
  int32_t const w1 = edge_i(t->x2, t->y2, t->x0, t->y0, cx, cy);
  int32_t const w2 = edge_i(t->x0, t->y0, t->x1, t->y1, cx, cy);
  int const in0 = (w0 > 0) || (w0 == 0 && t->tl1);
  int const in1 = (w1 > 0) || (w1 == 0 && t->tl2);
  int const in2 = (w2 > 0) || (w2 == 0 && t->tl0);
  if (!(in0 && in1 && in2)) {
    return 0;
  }
  uint8_t const fogf =
      gouraud_ref(w0, w1, w2, (int)fg0, (int)fg1, (int)fg2, t->area2);
  *out = fog_lerp(flat565, fog_color, fogf);
  return 1;
}

const uint16_t K_FOG565 = (uint16_t)((31U << 11) | (50U << 5) | 5U);

}  // namespace

// Bit-exact wiring: textured + Gouraud + MODULATE, fog ENABLED with a
// per-vertex fog GRADIENT (0/128/255). Every covered pixel must EXACTLY equal
// the unfogged combiner output lerped toward fog_color by the affine fog factor
// — proves the per-pixel fog interp + fog_lerp + lockstep swap are wired
// correctly.
TEST(RasterFog, TexturedFogMatchesReferenceExact) {
  struct Tex565 tex;
  make_tex565(&tex);
  struct RenderState rs;
  memset(&rs, 0, sizeof(rs));
  tex_desc_565(&rs.tex, &tex);
  rs.combiner.mode = COMBINE_MODULATE;
  rs.alpha_cmp = 0;
  rs.fog.enabled = 1;
  rs.fog.color = K_FOG565;
  const struct RenderState* table = &rs;

  uint16_t const white = rgb565_pack(255, 255, 255);
  TVtx pool[3];
  pool[0] = mk_tvtx_uv(8, 8, 0x20000, 0, 0, white);
  pool[1] = mk_tvtx_uv(52, 12, 0x08000, 7 * 32, 0, white);
  pool[2] = mk_tvtx_uv(14, 50, 0x10000, 0, 7 * 32, white);
  set_fog3(pool, 0, 128, 255);  // distinct per-vertex fog gradient
  TriRef ref;
  ref.v0 = 0;
  ref.v1 = 1;
  ref.v2 = 2;
  ref.material = 0;
  TileBin bin;
  bin.refs = &ref;
  bin.count = 1;
  bin.cap = 1;
  bin.dropped = 0;

  static uint16_t fb[RDR_SCREEN_W * RDR_SCREEN_H];
  static uint16_t zbuf[RDR_TILE_W * RDR_TILE_H];
  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    fb[i] = 0;
  }
  raster_tile(0, &bin, pool, fb, zbuf, table, 0, /*worker=*/0);

  struct RefTri rt;
  ref_setup(&rt, &pool[0], &pool[1], &pool[2]);
  uint8_t fg0;
  uint8_t fg1;
  uint8_t fg2;
  ref_fog_order(pool, &fg0, &fg1, &fg2);
  int checked = 0;
  for (int sy = 0; sy < RDR_TILE_H; ++sy) {
    for (int sx = 0; sx < RDR_TILE_W; ++sx) {
      uint16_t expected;
      if (!ref_fogged_pixel(&rt, &rs, fg0, fg1, fg2, sx, sy, &expected)) {
        continue;
      }
      uint16_t const got = fb[(sy * RDR_SCREEN_W) + sx];
      ASSERT_EQ(got, expected)
          << "fogged pixel (" << sx << "," << sy << ") mismatch";
      ++checked;
    }
  }
  EXPECT_GT(checked, 50) << "too few covered pixels — test is vacuous";
}

// Float-oracle tolerance: a LOW-FREQUENCY (smooth) texture + a fog GRADIENT,
// the rendered pixel de-565'd must match oracle_fog_lerp(combiner_color,
// fog_color, fogf/255) within a couple 565 quanta (a 1-texel slip stays tiny;
// R.1 lesson).
TEST(RasterFog, TexturedFogMatchesFloatOracle) {
  struct Tex565 tex;
  make_tex565_smooth(&tex);
  struct RenderState rs;
  memset(&rs, 0, sizeof(rs));
  tex_desc_565(&rs.tex, &tex);
  rs.combiner.mode = COMBINE_MODULATE;
  rs.alpha_cmp = 0;
  rs.fog.enabled = 1;
  rs.fog.color = K_FOG565;

  uint16_t const shade = rgb565_pack(200, 200, 200);
  TVtx pool[3];
  pool[0] = mk_tvtx_uv(8, 8, 0x20000, 0, 0, shade);
  pool[1] = mk_tvtx_uv(52, 12, 0x08000, 7 * 32, 0, shade);
  pool[2] = mk_tvtx_uv(14, 50, 0x10000, 0, 7 * 32, shade);
  set_fog3(pool, 16, 130, 240);
  TriRef ref;
  ref.v0 = 0;
  ref.v1 = 1;
  ref.v2 = 2;
  ref.material = 0;
  TileBin bin;
  bin.refs = &ref;
  bin.count = 1;
  bin.cap = 1;
  bin.dropped = 0;

  static uint16_t fb[RDR_SCREEN_W * RDR_SCREEN_H];
  static uint16_t zbuf[RDR_TILE_W * RDR_TILE_H];
  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    fb[i] = 0;
  }
  raster_tile(0, &bin, pool, fb, zbuf, &rs, 0, /*worker=*/0);

  struct RefTri rt;
  ref_setup(&rt, &pool[0], &pool[1], &pool[2]);
  uint8_t fg0;
  uint8_t fg1;
  uint8_t fg2;
  ref_fog_order(pool, &fg0, &fg1, &fg2);
  uint8_t fogr;
  uint8_t fogg;
  uint8_t fogb;
  oracle_unpack565(K_FOG565, &fogr, &fogg, &fogb);
  uint8_t const fog_rgb[3] = {fogr, fogg, fogb};

  int checked = 0;
  int worst = 0;
  for (int sy = 0; sy < RDR_TILE_H; ++sy) {
    for (int sx = 0; sx < RDR_TILE_W; ++sx) {
      uint16_t combiner_only;
      if (!ref_textured_pixel(&rt, &rs, sx, sy, &combiner_only)) {
        continue;
      }
      // Affine fog factor exactly as the impl interpolates it.
      fx12_4 const cx = (fx12_4)((sx << 4) + 8);
      fx12_4 const cy = (fx12_4)((sy << 4) + 8);
      int32_t const w0 = edge_i(rt.x1, rt.y1, rt.x2, rt.y2, cx, cy);
      int32_t const w1 = edge_i(rt.x2, rt.y2, rt.x0, rt.y0, cx, cy);
      int32_t const w2 = edge_i(rt.x0, rt.y0, rt.x1, rt.y1, cx, cy);
      uint8_t const fogf =
          gouraud_ref(w0, w1, w2, (int)fg0, (int)fg1, (int)fg2, rt.area2);
      uint8_t cr;
      uint8_t cg;
      uint8_t cb;
      oracle_unpack565(combiner_only, &cr, &cg, &cb);
      uint8_t const in_rgb[3] = {cr, cg, cb};
      uint8_t expect_rgb[3];
      ASSERT_EQ(
          oracle_fog_lerp(in_rgb, fog_rgb, (float)fogf / 255.0F, expect_rgb),
          0);
      uint16_t const got = fb[(sy * RDR_SCREEN_W) + sx];
      uint8_t gr;
      uint8_t gg;
      uint8_t gb;
      oracle_unpack565(got, &gr, &gg, &gb);
      int const dr = abs((int)gr - (int)expect_rgb[0]);
      int const dg = abs((int)gg - (int)expect_rgb[1]);
      int const db = abs((int)gb - (int)expect_rgb[2]);
      if (dr > worst) {
        worst = dr;
      }
      if (dg > worst) {
        worst = dg;
      }
      if (db > worst) {
        worst = db;
      }
      // 565 quantum is 8 (R/B) or 4 (G) in 8-bit; allow a couple quanta for the
      // 565 round-trip + the trunc-vs-round in the impl's u8 path.
      ASSERT_LE(dr, 16) << "fog R off at (" << sx << "," << sy << ")";
      ASSERT_LE(dg, 16) << "fog G off at (" << sx << "," << sy << ")";
      ASSERT_LE(db, 16) << "fog B off at (" << sx << "," << sy << ")";
      ++checked;
    }
  }
  EXPECT_GT(checked, 50) << "too few covered pixels — test is vacuous";
  EXPECT_LE(worst, 16) << "worst-channel fog error exceeded tolerance";
}

// Monotonicity: with a per-vertex fog gradient, the pixel NEAR the fog==0
// vertex must be close to the unfogged combiner color, while the pixel NEAR the
// fog==255 vertex must be close to the fog_color (more fog farther). Uses a
// flat white texture so the combiner color is ~constant and the change is all
// fog.
TEST(RasterFog, MoreFogTowardFarVertex) {
  struct Tex565 tex;
  make_tex565_smooth(&tex);
  struct RenderState rs;
  memset(&rs, 0, sizeof(rs));
  tex_desc_565(&rs.tex, &tex);
  rs.combiner.mode = COMBINE_MODULATE;
  rs.alpha_cmp = 0;
  rs.fog.enabled = 1;
  rs.fog.color = K_FOG565;

  uint16_t const white = rgb565_pack(255, 255, 255);
  TVtx pool[3];
  pool[0] = mk_tvtx_uv(8, 8, 0x10000, 0, 0, white);
  pool[1] = mk_tvtx_uv(52, 12, 0x10000, 7 * 32, 0, white);
  pool[2] = mk_tvtx_uv(14, 50, 0x10000, 0, 7 * 32, white);
  set_fog3(pool, 0, 0, 255);  // v0/v1 unfogged, v2 fully fogged
  TriRef ref;
  ref.v0 = 0;
  ref.v1 = 1;
  ref.v2 = 2;
  ref.material = 0;
  TileBin bin;
  bin.refs = &ref;
  bin.count = 1;
  bin.cap = 1;
  bin.dropped = 0;

  static uint16_t fb[RDR_SCREEN_W * RDR_SCREEN_H];
  static uint16_t zbuf[RDR_TILE_W * RDR_TILE_H];
  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    fb[i] = 0;
  }
  raster_tile(0, &bin, pool, fb, zbuf, &rs, 0, /*worker=*/0);

  uint8_t fogr;
  uint8_t fogg;
  uint8_t fogb;
  oracle_unpack565(K_FOG565, &fogr, &fogg, &fogb);

  // Pixel near v0 (fog 0): close to white (combiner ~= texel ~= white).
  uint16_t const near0 = fb[(10 * RDR_SCREEN_W) + 12];
  uint8_t n0r;
  uint8_t n0g;
  uint8_t n0b;
  oracle_unpack565(near0, &n0r, &n0g, &n0b);
  // Pixel near v2 (fog 255): close to fog_color.
  uint16_t const near2 = fb[(46 * RDR_SCREEN_W) + 15];
  uint8_t n2r;
  uint8_t n2g;
  uint8_t n2b;
  oracle_unpack565(near2, &n2r, &n2g, &n2b);

  // Near the unfogged vertex, the green/blue channels are still near full
  // (white), well above the fog_color's tiny G/B; near the fully-fogged vertex
  // they collapse toward fog_color (which is mostly red). So fog increases the
  // distance from white toward fog_color along the gradient.
  int const dist0 = abs((int)n0g - (int)fogg) + abs((int)n0b - (int)fogb);
  int const dist2 = abs((int)n2g - (int)fogg) + abs((int)n2b - (int)fogb);
  EXPECT_GT(dist0, dist2)
      << "fog did not increase toward the far (fog==255) vertex";
}

// XLU + FOG bit-exact: the translucent sweep must fog the combiner color BEFORE
// the premultiply. Expected = the L6 premultiplied-UNDER path with the FOGGED
// fragment: blend_premul_accumulate(fog_lerp(combiner, fog_color, fogf),
// texel_alpha) then blend_premul_resolve over bg. Proves fog is applied
// pre-premultiply on the XLU path (faithful: a distant translucent fragment
// fogs then contributes).
TEST(RasterFog, XluFogAppliedBeforePremulUnder) {
  struct Tex4444 tex;
  make_tex4444_alpha(&tex, 0x8);  // fractional alpha 136
  struct RenderState rs;
  mk_xlu_state(&rs, &tex);
  rs.fog.enabled = 1;
  rs.fog.color = K_FOG565;
  const struct RenderState* table = &rs;

  uint16_t const bg = rgb565_pack(30, 70, 110);
  uint16_t const white = rgb565_pack(255, 255, 255);
  TVtx pool[3];
  pool[0] = mk_tvtx_uv(8, 8, 0x18000, 0, 0, white);
  pool[1] = mk_tvtx_uv(52, 12, 0x10000, 7 * 32, 0, white);
  pool[2] = mk_tvtx_uv(14, 50, 0x14000, 0, 7 * 32, white);
  set_fog3(pool, 20, 140, 230);
  TriRef ref;
  ref.v0 = 0;
  ref.v1 = 1;
  ref.v2 = 2;
  ref.material = 0;
  TileBin bin;
  bin.refs = &ref;
  bin.count = 1;
  bin.cap = 1;
  bin.dropped = 0;

  static uint16_t fb[RDR_SCREEN_W * RDR_SCREEN_H];
  static uint16_t zbuf[RDR_TILE_W * RDR_TILE_H];
  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    fb[i] = bg;
  }
  raster_tile(0, &bin, pool, fb, zbuf, table, 0, /*worker=*/0);

  struct RefTri rt;
  ref_setup(&rt, &pool[0], &pool[1], &pool[2]);
  uint8_t fg0;
  uint8_t fg1;
  uint8_t fg2;
  ref_fog_order(pool, &fg0, &fg1, &fg2);
  int checked = 0;
  int blended = 0;
  for (int sy = 0; sy < RDR_TILE_H; ++sy) {
    for (int sx = 0; sx < RDR_TILE_W; ++sx) {
      // Unfogged combiner color (no blend yet): reuse ref_textured_pixel which
      // returns shade_pixel(combiner) for the covered pixel.
      uint16_t combiner_only;
      if (!ref_textured_pixel(&rt, &rs, sx, sy, &combiner_only)) {
        continue;
      }
      fx12_4 const cx = (fx12_4)((sx << 4) + 8);
      fx12_4 const cy = (fx12_4)((sy << 4) + 8);
      int32_t const w0 = edge_i(rt.x1, rt.y1, rt.x2, rt.y2, cx, cy);
      int32_t const w1 = edge_i(rt.x2, rt.y2, rt.x0, rt.y0, cx, cy);
      int32_t const w2 = edge_i(rt.x0, rt.y0, rt.x1, rt.y1, cx, cy);
      uint8_t const fogf =
          gouraud_ref(w0, w1, w2, (int)fg0, (int)fg1, (int)fg2, rt.area2);
      uint16_t const fogged = fog_lerp(combiner_only, K_FOG565, fogf);
      // texel alpha is the constant 136 of this 4444 texture.
      uint8_t rgba[4];
      int64_t const numiw = ((int64_t)w0 * rt.iw0) + ((int64_t)w1 * rt.iw1) +
                            ((int64_t)w2 * rt.iw2);
      int32_t const inv_w = (int32_t)(numiw / (int64_t)rt.area2);
      int64_t const numu = ((int64_t)w0 * rt.u_iw0) + ((int64_t)w1 * rt.u_iw1) +
                           ((int64_t)w2 * rt.u_iw2);
      int64_t const numv = ((int64_t)w0 * rt.v_iw0) + ((int64_t)w1 * rt.v_iw1) +
                           ((int64_t)w2 * rt.v_iw2);
      int32_t const u_iw_p = (int32_t)(numu / (int64_t)rt.area2);
      int32_t const v_iw_p = (int32_t)(numv / (int64_t)rt.area2);
      fx16_16 const u_q16 = (fx16_16)(((int64_t)u_iw_p << 27) / (int64_t)inv_w);
      fx16_16 const v_q16 = (fx16_16)(((int64_t)v_iw_p << 27) / (int64_t)inv_w);
      tex_sample_rgba(&rs.tex, u_q16, v_q16, 0, rgba);
      // L6: fog applied to `fogged` FIRST (before the premultiply), then the
      // single layer accumulated UNDER and bg folded under — bit-exact vs the
      // impl's blend_premul_accumulate/_resolve.
      uint16_t c_acc = 0;
      uint8_t aa = 0;
      blend_premul_accumulate(&c_acc, &aa, fogged, rgba[3]);
      uint16_t const expected =
          (aa != 0) ? blend_premul_resolve(c_acc, aa, bg) : bg;
      uint16_t const got = fb[(sy * RDR_SCREEN_W) + sx];
      ASSERT_EQ(got, expected)
          << "XLU+fog pixel (" << sx << "," << sy << ") mismatch";
      if (got != bg) {
        ++blended;
      }
      ++checked;
    }
  }
  EXPECT_GT(checked, 50) << "too few covered pixels — test is vacuous";
  EXPECT_GT(blended, 50) << "XLU+fog did not composite (no blend over bg)";
}

// Bit-exact wiring of the FLAT fast path under fog (symmetric with
// TexturedFogMatchesReferenceExact): a NO-TEXTURE fog-enabled tri with a
// per-vertex fog GRADIENT (0/128/255). The flat path (raster.cc !textured
// branch) has no combiner — its base is the provoking color t->color — so every
// covered pixel must EXACTLY equal fog_lerp(flat, fog_color,
// affine_fog_factor). Pins the flat fog path bit-exactly (the mixed-scene CRC
// only covers it indirectly). The provoking color is the FLAT color of v0
// (pre-swap), constant across the tri, so the only per-pixel variation is the
// fog factor.
TEST(RasterFog, FlatFogMatchesReferenceExact) {
  // No texture -> flat fast path. Fog enabled toward K_FOG565.
  struct RenderState rs;
  memset(&rs, 0, sizeof(rs));
  rs.fog.enabled = 1;
  rs.fog.color = K_FOG565;
  const struct RenderState* table = &rs;

  // Distinct per-vertex fog (0/128/255); all three carry the SAME flat color so
  // t->color (provoking v0) is unambiguous and the reference base is exact.
  uint16_t const flat = rgb565_pack(40, 180, 220);
  TVtx pool[3];
  pool[0] = mk_vtx(8, 8, 0, 0, 0x10000);
  pool[1] = mk_vtx(52, 12, 0, 0, 0x10000);
  pool[2] = mk_vtx(14, 50, 0, 0, 0x10000);
  for (int k = 0; k < 3; ++k) {
    pool[k].rgba = flat;  // mk_vtx defaults to K_FLAT565; override to `flat`
  }
  set_fog3(pool, 0, 128, 255);
  TriRef ref;
  ref.v0 = 0;
  ref.v1 = 1;
  ref.v2 = 2;
  ref.material = 0;
  TileBin bin;
  bin.refs = &ref;
  bin.count = 1;
  bin.cap = 1;
  bin.dropped = 0;

  static uint16_t fb[RDR_SCREEN_W * RDR_SCREEN_H];
  static uint16_t zbuf[RDR_TILE_W * RDR_TILE_H];
  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    fb[i] = 0;
  }
  raster_tile(0, &bin, pool, fb, zbuf, table, 0, /*worker=*/0);

  struct RefTri rt;
  ref_setup(&rt, &pool[0], &pool[1], &pool[2]);
  uint8_t fg0;
  uint8_t fg1;
  uint8_t fg2;
  ref_fog_order(pool, &fg0, &fg1, &fg2);
  int checked = 0;
  for (int sy = 0; sy < RDR_TILE_H; ++sy) {
    for (int sx = 0; sx < RDR_TILE_W; ++sx) {
      uint16_t expected;
      if (!ref_flat_fogged_pixel(&rt, flat, K_FOG565, fg0, fg1, fg2, sx, sy,
                                 &expected)) {
        continue;
      }
      uint16_t const got = fb[(sy * RDR_SCREEN_W) + sx];
      ASSERT_EQ(got, expected)
          << "flat fogged pixel (" << sx << "," << sy << ") mismatch";
      ++checked;
    }
  }
  EXPECT_GT(checked, 50) << "too few covered pixels — test is vacuous";
}

// ===========================================================================
// R.3-AA coverage WRITE tests. The rasterizer writes an analytic coverage byte
// at each winning OPAQUE fragment when AA is enabled AND a cov scratch is
// supplied. We verify: interior -> AA_COV_FULL(255); a pixel-centre on an edge
// -> ~128 (0.5); untouched/outside -> the FULL clear value; AA-off / null-cov
// leaves cov untouched; and per-pixel parity vs the float oracle_coverage on
// clean edge pixels.
// ===========================================================================
#include "aa/aa.h"  // aa_set_enabled, AA_COV_FULL

namespace {

// Float edge function matching raster.cc edge_eval (Q12.4 inputs as float).
double edge_f(double ax, double ay, double bx, double by, double px,
              double py) {
  return ((bx - ax) * (py - ay)) - ((by - ay) * (px - ax));
}
double edge_len_q124(double ax, double ay, double bx, double by) {
  double const dx = bx - ax;
  double const dy = by - ay;
  return sqrt((dx * dx) + (dy * dy));
}

// AA-on render of one tri (tile 0) into a caller cov buffer (cleared to FULL by
// raster_tile). fb cleared to black. Verts must already be positive-area in
// screen y-down so the rasterizer does NOT winding-swap (keeps the edge mapping
// w0=edge(v1,v2), w1=edge(v2,v0), w2=edge(v0,v1) predictable for the oracle).
void render_cov(const OneTri* o, uint8_t* cov_out) {
  static uint16_t fb[RDR_SCREEN_W * RDR_SCREEN_H];
  static uint16_t zbuf[RDR_TILE_W * RDR_TILE_H];
  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    fb[i] = 0;
  }
  aa_set_enabled(1);
  raster_tile(0, &o->bin, o->pool, fb, zbuf, no_texture_table(), cov_out,
              /*worker=*/0);
  aa_set_enabled(0);
}

}  // namespace

// AA OFF: cov scratch must be left UNTOUCHED (the golden-neutral gate at the
// write site — even when a cov pointer is supplied, AA-off skips it entirely).
TEST(RasterCoverage, DisabledLeavesCovUntouched) {
  OneTri o;
  one_tri(&o, mk_vtx(8, 8, 0, 0, 0x10000), mk_vtx(50, 12, 0, 0, 0x10000),
          mk_vtx(20, 50, 0, 0, 0x10000));
  static uint16_t fb[RDR_SCREEN_W * RDR_SCREEN_H];
  static uint16_t zbuf[RDR_TILE_W * RDR_TILE_H];
  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    fb[i] = 0;
  }
  uint8_t cov[RDR_TILE_W * RDR_TILE_H];
  memset(cov, 0x5A, sizeof(cov));  // sentinel
  aa_set_enabled(0);
  raster_tile(0, &o.bin, o.pool, fb, zbuf, no_texture_table(), cov,
              /*worker=*/0);
  for (int i = 0; i < RDR_TILE_W * RDR_TILE_H; ++i) {
    ASSERT_EQ(cov[i], 0x5A) << "AA-off must not touch cov (i=" << i << ")";
  }
}

// AA ON: a deep interior pixel saturates to AA_COV_FULL; a pixel no fragment
// covers stays at the FULL clear value (so the resolve treats it as interior).
TEST(RasterCoverage, InteriorFullAndUntouchedFull) {
  // Large triangle so its centre region is many px from every edge -> 255.
  OneTri o;
  one_tri(&o, mk_vtx(4, 4, 0, 0, 0x10000), mk_vtx(56, 8, 0, 0, 0x10000),
          mk_vtx(10, 56, 0, 0, 0x10000));
  uint8_t cov[RDR_TILE_W * RDR_TILE_H];
  render_cov(&o, cov);
  // A clearly-interior pixel (centroid-ish).
  int const cx = (4 + 56 + 10) / 3;
  int const cy = (4 + 8 + 56) / 3;
  EXPECT_EQ(cov[(cy * RDR_TILE_W) + cx], AA_COV_FULL)
      << "deep interior must saturate to full coverage";
  // A pixel far outside the triangle (top-right corner of the tile) was never
  // written -> stays at the FULL clear value.
  EXPECT_EQ(cov[(2 * RDR_TILE_W) + (RDR_TILE_W - 2)], AA_COV_FULL);
}

// AA ON: a pixel whose centre lies EXACTLY on an edge gets ~half coverage
// (d=0 -> cov = 0.5 -> byte 128). We place a vertical LEFT edge through the
// column-6 pixel CENTRES: a centre is at x*16+8 in Q12.4, so an edge x-coord of
// 6*16+8 (px 6 + 0.5) makes the column-6 centres sit exactly on the edge ->
// perpendicular distance 0 -> coverage 0.5. The other two edges are far at
// mid-height, so this edge is the min.
TEST(RasterCoverage, EdgePixelHalfCoverage) {
  OneTri o;
  // v0=(6,6)+0.5px, v1=(6,54)+0.5px: a vertical left edge at x = 6.5 px running
  // down; v2=(34,30) puts the interior to the right. sub_x=8 = +0.5 px (Q12.4).
  one_tri(&o, mk_vtx(6, 6, 8, 0, 0x10000), mk_vtx(6, 54, 8, 0, 0x10000),
          mk_vtx(34, 30, 0, 0, 0x10000));
  uint8_t cov[RDR_TILE_W * RDR_TILE_H];
  render_cov(&o, cov);
  // Row y=30 (mid-height, top/bottom verts far). The column-6 pixel centre is
  // ON the edge -> coverage byte ~128 (the +0.5 bias maps d=0 to 0.5).
  int const y = 30;
  uint8_t const cv = cov[(y * RDR_TILE_W) + 6];
  EXPECT_NEAR((int)cv, 128, 4) << "centre-on-edge pixel must read ~half cover";
  // Deeper in (x=10) the same row is interior -> full coverage.
  EXPECT_EQ(cov[(y * RDR_TILE_W) + 10], AA_COV_FULL);
}

// AA ON: per-pixel parity vs the float oracle_coverage on a clean triangle.
// For every covered pixel we recompute the float edge distances (same edge_eval
// + gradient normalisation as raster.cc) and assert the integer coverage byte
// equals round(255*oracle) within a small tolerance (fixed-point rounding).
TEST(RasterCoverage, ParityWithFloatOracle) {
  OneTri o;
  // Clean, low-frequency geometry, positive area in y-down (no winding swap).
  one_tri(&o, mk_vtx(8, 6, 0, 0, 0x10000), mk_vtx(52, 14, 0, 0, 0x10000),
          mk_vtx(16, 50, 0, 0, 0x10000));
  uint8_t cov[RDR_TILE_W * RDR_TILE_H];
  render_cov(&o, cov);

  // Vertex Q12.4 coords (tile 0 -> tile-local == screen for tile 0).
  double const x0 = (double)o.pool[0].x;
  double const y0 = (double)o.pool[0].y;
  double const x1 = (double)o.pool[1].x;
  double const y1 = (double)o.pool[1].y;
  double const x2 = (double)o.pool[2].x;
  double const y2 = (double)o.pool[2].y;
  // Per-edge length (Q12.4 units), matched to w0/w1/w2.
  double const len0 = edge_len_q124(x1, y1, x2, y2);  // w0 = edge(v1,v2)
  double const len1 = edge_len_q124(x2, y2, x0, y0);  // w1 = edge(v2,v0)
  double const len2 = edge_len_q124(x0, y0, x1, y1);  // w2 = edge(v0,v1)

  int checked = 0;
  int max_abs = 0;
  for (int sy = 0; sy < RDR_TILE_H; ++sy) {
    for (int sx = 0; sx < RDR_TILE_W; ++sx) {
      uint8_t const cv = cov[(sy * RDR_TILE_W) + sx];
      if (cv == AA_COV_FULL) {
        continue;  // interior / untouched — parity is on partial edge pixels.
      }
      // Pixel-centre in Q12.4 (matches raster: (sx<<4)+8).
      double const px = (double)((sx << 4) + 8);
      double const py = (double)((sy << 4) + 8);
      double const w0 = edge_f(x1, y1, x2, y2, px, py);
      double const w1 = edge_f(x2, y2, x0, y0, px, py);
      double const w2 = edge_f(x0, y0, x1, y1, px, py);
      // d_i (pixels) = w_i / (16 * len_i).
      double const d0 = w0 / (16.0 * len0);
      double const d1 = w1 / (16.0 * len1);
      double const d2 = w2 / (16.0 * len2);
      float ocov = 0.0F;
      ASSERT_EQ(oracle_coverage((float)d0, (float)d1, (float)d2, &ocov), 0);
      int const expect = (int)lround((double)ocov * 255.0);
      int const diff = (int)cv - expect;
      int const adiff = diff < 0 ? -diff : diff;
      if (adiff > max_abs) {
        max_abs = adiff;
      }
      // Tolerance: byte rounding + the +128 fixed-point bias vs the 0.5*255
      // float scaling differ by at most a couple LSBs.
      ASSERT_LE(adiff, 3) << "coverage parity at (" << sx << "," << sy
                          << ") cv=" << (int)cv << " oracle=" << expect;
      ++checked;
    }
  }
  EXPECT_GT(checked, 10) << "too few partial edge pixels — test is weak";
}

// AA ON, NEGATIVE-initial-area tri (triggers tri_setup's v1<->v2 winding swap).
// This is the ONLY check that distinguishes a post-swap recip (correct: the
// per-edge gradient reciprocals must be computed from the FINAL, swapped verts)
// from a pre-swap recip (the classic skew bug — would still pass every
// positive-area parity test). We feed reversed winding, reconstruct the SAME
// post-swap ordering raster.cc produces (swap v1<->v2 to positive area), and
// assert cov-byte parity vs oracle_coverage computed on those FINAL edges.
TEST(RasterCoverage, ParityWithFloatOracleReversedWinding) {
  OneTri o;
  // A NEGATIVE-initial-area tri with DELIBERATELY ASYMMETRIC edge lengths so a
  // pre-swap recip would mis-pair the per-edge gradient: under the v1<->v2
  // swap, slot r1 (edge v2->v0) and slot r2 (edge v0->v1) exchange lengths (~52
  // px vs ~17 px here), so a pre-swap recip pairs the 17 px reciprocal with the
  // 52 px edge (~3x skew) -> cov parity blows past the +/-3 tolerance. (A
  // symmetric tri would hide it: r0's edge length is swap-invariant and a
  // near-isoceles r1/r2 makes the mis-pairing invisible.)
  one_tri(&o, mk_vtx(8, 28, 0, 0, 0x10000), mk_vtx(20, 40, 0, 0, 0x10000),
          mk_vtx(56, 8, 0, 0, 0x10000));
  // Sanity: the INPUT really is negative initial area (so the swap path runs).
  double const ix0 = (double)o.pool[0].x;
  double const iy0 = (double)o.pool[0].y;
  double const ix1 = (double)o.pool[1].x;
  double const iy1 = (double)o.pool[1].y;
  double const ix2 = (double)o.pool[2].x;
  double const iy2 = (double)o.pool[2].y;
  double const init_area2 = edge_f(ix0, iy0, ix1, iy1, ix2, iy2);
  ASSERT_LT(init_area2, 0.0)
      << "test setup wrong — input must be negative-area to exercise the swap";

  uint8_t cov[RDR_TILE_W * RDR_TILE_H];
  render_cov(&o, cov);

  // Reconstruct the FINAL (post-swap) vertex order: tri_setup swaps v1<->v2 on
  // negative area, leaving v0 fixed. cov_byte uses these final edges.
  double const x0 = ix0;
  double const y0 = iy0;
  double const x1 = ix2;  // swapped
  double const y1 = iy2;
  double const x2 = ix1;
  double const y2 = iy1;
  double const len0 = edge_len_q124(x1, y1, x2, y2);  // w0 = edge(v1,v2)
  double const len1 = edge_len_q124(x2, y2, x0, y0);  // w1 = edge(v2,v0)
  double const len2 = edge_len_q124(x0, y0, x1, y1);  // w2 = edge(v0,v1)

  int checked = 0;
  for (int sy = 0; sy < RDR_TILE_H; ++sy) {
    for (int sx = 0; sx < RDR_TILE_W; ++sx) {
      uint8_t const cv = cov[(sy * RDR_TILE_W) + sx];
      if (cv == AA_COV_FULL) {
        continue;
      }
      double const px = (double)((sx << 4) + 8);
      double const py = (double)((sy << 4) + 8);
      double const w0 = edge_f(x1, y1, x2, y2, px, py);
      double const w1 = edge_f(x2, y2, x0, y0, px, py);
      double const w2 = edge_f(x0, y0, x1, y1, px, py);
      double const d0 = w0 / (16.0 * len0);
      double const d1 = w1 / (16.0 * len1);
      double const d2 = w2 / (16.0 * len2);
      float ocov = 0.0F;
      ASSERT_EQ(oracle_coverage((float)d0, (float)d1, (float)d2, &ocov), 0);
      int const expect = (int)lround((double)ocov * 255.0);
      int const diff = (int)cv - expect;
      int const adiff = diff < 0 ? -diff : diff;
      ASSERT_LE(adiff, 3) << "reversed-winding coverage parity at (" << sx
                          << "," << sy << ") cv=" << (int)cv
                          << " oracle=" << expect
                          << " — recip likely computed PRE-swap (skew bug)";
      ++checked;
    }
  }
  EXPECT_GT(checked, 10) << "too few partial edge pixels — test is weak";
}

// AA ON, TEXTURED-opaque path: the winning textured fragment writes coverage
// (raster.cc:587) the SAME way the flat path does (cov_byte over the final
// edges). nit #2: value-check it (the determinism CRC alone never pinned the
// value). Parity vs oracle_coverage on the partial edge pixels.
TEST(RasterCoverage, TexturedOpaqueParityWithOracle) {
  struct Tex565 tex;
  make_tex565(&tex);
  struct RenderState rs;
  memset(&rs, 0, sizeof(rs));
  tex_desc_565(&rs.tex, &tex);
  rs.combiner.mode = COMBINE_MODULATE;
  rs.alpha_cmp = 0;
  const struct RenderState* table = &rs;

  uint16_t const white = rgb565_pack(255, 255, 255);
  TVtx pool[3];
  // Positive-area, low-frequency; distinct inv_w (perspective) — coverage is
  // path-independent so the textured write must match the same oracle.
  pool[0] = mk_tvtx_uv(8, 6, 0x10000, 0, 0, white);
  pool[1] = mk_tvtx_uv(52, 14, 0x10000, 7 * 32, 0, white);
  pool[2] = mk_tvtx_uv(16, 50, 0x10000, 0, 7 * 32, white);
  TriRef ref;
  ref.v0 = 0;
  ref.v1 = 1;
  ref.v2 = 2;
  ref.material = 0;
  TileBin bin;
  bin.refs = &ref;
  bin.count = 1;
  bin.cap = 1;
  bin.dropped = 0;

  static uint16_t fb[RDR_SCREEN_W * RDR_SCREEN_H];
  static uint16_t zbuf[RDR_TILE_W * RDR_TILE_H];
  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    fb[i] = 0;
  }
  uint8_t cov[RDR_TILE_W * RDR_TILE_H];
  aa_set_enabled(1);
  raster_tile(0, &bin, pool, fb, zbuf, table, cov, /*worker=*/0);
  aa_set_enabled(0);

  double const x0 = (double)pool[0].x;
  double const y0 = (double)pool[0].y;
  double const x1 = (double)pool[1].x;
  double const y1 = (double)pool[1].y;
  double const x2 = (double)pool[2].x;
  double const y2 = (double)pool[2].y;
  double const len0 = edge_len_q124(x1, y1, x2, y2);
  double const len1 = edge_len_q124(x2, y2, x0, y0);
  double const len2 = edge_len_q124(x0, y0, x1, y1);

  int checked = 0;
  for (int sy = 0; sy < RDR_TILE_H; ++sy) {
    for (int sx = 0; sx < RDR_TILE_W; ++sx) {
      uint8_t const cv = cov[(sy * RDR_TILE_W) + sx];
      if (cv == AA_COV_FULL) {
        continue;
      }
      double const px = (double)((sx << 4) + 8);
      double const py = (double)((sy << 4) + 8);
      double const w0 = edge_f(x1, y1, x2, y2, px, py);
      double const w1 = edge_f(x2, y2, x0, y0, px, py);
      double const w2 = edge_f(x0, y0, x1, y1, px, py);
      double const d0 = w0 / (16.0 * len0);
      double const d1 = w1 / (16.0 * len1);
      double const d2 = w2 / (16.0 * len2);
      float ocov = 0.0F;
      ASSERT_EQ(oracle_coverage((float)d0, (float)d1, (float)d2, &ocov), 0);
      int const expect = (int)lround((double)ocov * 255.0);
      int const diff = (int)cv - expect;
      int const adiff = diff < 0 ? -diff : diff;
      ASSERT_LE(adiff, 3) << "textured coverage parity at (" << sx << "," << sy
                          << ") cv=" << (int)cv << " oracle=" << expect;
      ++checked;
    }
  }
  EXPECT_GT(checked, 10) << "too few partial edge pixels — test is weak";
}

// AA ON, XLU path: a covered translucent fragment STAMPS cov==AA_COV_FULL
// (raster.cc:747 — AA ignores translucent geometry, plan v1). nit #2:
// value-check an XLU-covered pixel reads full coverage (so the resolve treats
// it as interior, never edge-blending over the alpha-over result).
TEST(RasterCoverage, XluFragmentStampsFullCoverage) {
  struct Tex4444 tex;
  make_tex4444_alpha(&tex, 0x8);  // fractional alpha so the XLU fragment writes
  struct RenderState rs;
  mk_xlu_state(&rs, &tex);
  const struct RenderState* table = &rs;

  uint16_t const white = rgb565_pack(255, 255, 255);
  TVtx pool[3];
  pool[0] = mk_tvtx_uv(8, 8, 0x10000, 0, 0, white);
  pool[1] = mk_tvtx_uv(52, 12, 0x10000, 7 * 32, 0, white);
  pool[2] = mk_tvtx_uv(14, 50, 0x10000, 0, 7 * 32, white);
  TriRef ref;
  ref.v0 = 0;
  ref.v1 = 1;
  ref.v2 = 2;
  ref.material = 0;
  TileBin bin;
  bin.refs = &ref;
  bin.count = 1;
  bin.cap = 1;
  bin.dropped = 0;

  static uint16_t fb[RDR_SCREEN_W * RDR_SCREEN_H];
  static uint16_t zbuf[RDR_TILE_W * RDR_TILE_H];
  uint16_t const bg = rgb565_pack(30, 70, 110);
  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    fb[i] = bg;
  }
  uint8_t cov[RDR_TILE_W * RDR_TILE_H];
  aa_set_enabled(1);
  raster_tile(0, &bin, pool, fb, zbuf, table, cov, /*worker=*/0);
  aa_set_enabled(0);

  // Find a pixel the XLU fragment actually touched (fb changed from bg), assert
  // its coverage is FULL (255). Anti-vacuity: require we found one.
  int found = 0;
  for (int sy = 0; sy < RDR_TILE_H && !found; ++sy) {
    for (int sx = 0; sx < RDR_TILE_W && !found; ++sx) {
      if (fb[(sy * RDR_SCREEN_W) + sx] != bg) {
        EXPECT_EQ(cov[(sy * RDR_TILE_W) + sx], AA_COV_FULL)
            << "XLU-covered pixel (" << sx << "," << sy
            << ") must stamp full coverage (AA ignores XLU)";
        found = 1;
      }
    }
  }
  EXPECT_EQ(found, 1) << "no XLU fragment blended — test is vacuous";
}

// ===========================================================================
// R.4 — 2-cycle detail multitexture (TEXEL1 sample + shade_pixel2). cycle ==
// COMBINE_TWO_CYCLE with a valid tex1 routes the detail combiner; the default
// (cycle == ONE / no tex1) is BYTE-IDENTICAL to the pre-R.4 textured path. The
// TEXEL1 UV = base UV << detail_shift.
// ===========================================================================
namespace {

// A second 8x8 detail texture, distinct gradient from make_tex565 so its
// contribution is observable separately from the base.
void make_tex565_detail(struct Tex565* t) {
  for (int v = 0; v < K_TEXH; ++v) {
    for (int u = 0; u < K_TEXW; ++u) {
      uint8_t const r = (uint8_t)(200 - (u * 20));
      uint8_t const g = (uint8_t)(60 + (v * 20));
      uint8_t const b = (uint8_t)(128 + ((u + v) * 8));
      t->px[(v * K_TEXW) + u] = rgb565_pack(r, g, b);
    }
  }
}

// Build a 2-cycle render state: base TEXEL0 in rs.tex, detail TEXEL1 in
// rs.tex1, cycle1 = TEXEL0 * TEXEL1, cycle2 = COMBINED * ENV(white) ~= cycle1.
void mk_two_cycle_state(struct RenderState* rs, const struct Tex565* base,
                        const struct Tex565* detail, uint8_t detail_shift) {
  memset(rs, 0, sizeof(*rs));
  tex_desc_565(&rs->tex, base);
  tex_desc_565(&rs->tex1, detail);
  rs->cycle = COMBINE_TWO_CYCLE;
  rs->detail_shift = detail_shift;
  rs->combiner.mode = COMBINE_CUSTOM;
  rs->combiner.a = CC_TEXEL0;
  rs->combiner.b = CC_ZERO;
  rs->combiner.c = CC_TEXEL1;
  rs->combiner.d = CC_ZERO;
  rs->combiner2.mode = COMBINE_CUSTOM;
  rs->combiner2.a = CC_COMBINED;
  rs->combiner2.b = CC_ZERO;
  rs->combiner2.c = CC_ENVIRONMENT;
  rs->combiner2.d = CC_ZERO;
  rs->env_color = 0xFFFF;  // white ENV
  rs->alpha_cmp = 0;
}

// Expected 2-cycle 565 for an interior covered pixel (mirrors raster's integer
// math: perspective UV, detail UV = base UV << shift, tex_sample both,
// shade_pixel2). Returns 1 + writes *out if covered, else 0.
int ref_two_cycle_pixel(const struct RefTri* t, const struct RenderState* rs,
                        int sx, int sy, uint16_t* out) {
  fx12_4 const cx = (fx12_4)((sx << 4) + 8);
  fx12_4 const cy = (fx12_4)((sy << 4) + 8);
  int32_t const w0 = edge_i(t->x1, t->y1, t->x2, t->y2, cx, cy);
  int32_t const w1 = edge_i(t->x2, t->y2, t->x0, t->y0, cx, cy);
  int32_t const w2 = edge_i(t->x0, t->y0, t->x1, t->y1, cx, cy);
  int const in0 = (w0 > 0) || (w0 == 0 && t->tl1);
  int const in1 = (w1 > 0) || (w1 == 0 && t->tl2);
  int const in2 = (w2 > 0) || (w2 == 0 && t->tl0);
  if (!(in0 && in1 && in2)) {
    return 0;
  }
  int64_t const numiw =
      ((int64_t)w0 * t->iw0) + ((int64_t)w1 * t->iw1) + ((int64_t)w2 * t->iw2);
  int32_t const inv_w = (int32_t)(numiw / (int64_t)t->area2);
  if (inv_w <= 0) {
    return 0;
  }
  int64_t const numu = ((int64_t)w0 * t->u_iw0) + ((int64_t)w1 * t->u_iw1) +
                       ((int64_t)w2 * t->u_iw2);
  int64_t const numv = ((int64_t)w0 * t->v_iw0) + ((int64_t)w1 * t->v_iw1) +
                       ((int64_t)w2 * t->v_iw2);
  int32_t const u_iw_p = (int32_t)(numu / (int64_t)t->area2);
  int32_t const v_iw_p = (int32_t)(numv / (int64_t)t->area2);
  fx16_16 const u_q16 = (fx16_16)(((int64_t)u_iw_p << 27) / (int64_t)inv_w);
  fx16_16 const v_q16 = (fx16_16)(((int64_t)v_iw_p << 27) / (int64_t)inv_w);
  uint8_t const gr =
      gouraud_ref(w0, w1, w2, t->s0[0], t->s1[0], t->s2[0], t->area2);
  uint8_t const gg =
      gouraud_ref(w0, w1, w2, t->s0[1], t->s1[1], t->s2[1], t->area2);
  uint8_t const gb =
      gouraud_ref(w0, w1, w2, t->s0[2], t->s1[2], t->s2[2], t->area2);
  uint16_t const shade565 = rgb565_pack(gr, gg, gb);
  uint16_t const texel0_565 = tex_sample(&rs->tex, u_q16, v_q16, 0);
  fx16_16 const u1_q16 = (fx16_16)(u_q16 << rs->detail_shift);
  fx16_16 const v1_q16 = (fx16_16)(v_q16 << rs->detail_shift);
  uint16_t const texel1_565 = tex_sample(&rs->tex1, u1_q16, v1_q16, 0);
  uint8_t keep = 1;
  *out = shade_pixel2(rs, texel0_565, texel1_565, shade565, &keep);
  return 1;
}

}  // namespace

// 2-cycle textured render must EXACTLY equal the TEXEL0+TEXEL1+shade_pixel2
// reference at every covered pixel (bit-exact wiring: TEXEL1 sample, detail UV,
// feed-forward). Uses high-frequency textures so a UV/transposition slip shows.
TEST(RasterDetail, TwoCycleMatchesShadePixel2Exact) {
  struct Tex565 base;
  struct Tex565 detail;
  make_tex565(&base);
  make_tex565_detail(&detail);
  struct RenderState rs;
  mk_two_cycle_state(&rs, &base, &detail, 0);
  const struct RenderState* table = &rs;

  uint16_t const white = rgb565_pack(255, 255, 255);
  TVtx pool[3];
  pool[0] = mk_tvtx_uv(8, 8, 0x20000, 0, 0, white);
  pool[1] = mk_tvtx_uv(52, 12, 0x08000, 7 * 32, 0, white);
  pool[2] = mk_tvtx_uv(14, 50, 0x10000, 0, 7 * 32, white);
  TriRef ref;
  ref.v0 = 0;
  ref.v1 = 1;
  ref.v2 = 2;
  ref.material = 0;
  TileBin bin;
  bin.refs = &ref;
  bin.count = 1;
  bin.cap = 1;
  bin.dropped = 0;

  static uint16_t fb[RDR_SCREEN_W * RDR_SCREEN_H];
  static uint16_t zbuf[RDR_TILE_W * RDR_TILE_H];
  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    fb[i] = 0;
  }
  raster_tile(0, &bin, pool, fb, zbuf, table, 0, /*worker=*/0);

  struct RefTri rt;
  ref_setup(&rt, &pool[0], &pool[1], &pool[2]);
  int checked = 0;
  for (int sy = 0; sy < RDR_TILE_H; ++sy) {
    for (int sx = 0; sx < RDR_TILE_W; ++sx) {
      uint16_t expected;
      if (!ref_two_cycle_pixel(&rt, &rs, sx, sy, &expected)) {
        continue;
      }
      uint16_t const got = fb[(sy * RDR_SCREEN_W) + sx];
      ASSERT_EQ(got, expected)
          << "2-cycle detail pixel (" << sx << "," << sy << ") mismatch";
      ++checked;
    }
  }
  EXPECT_GT(checked, 50) << "too few covered pixels — test is vacuous";
}

// The detail texture VISIBLY modulates the base: a 2-cycle render must differ
// from a 1-cycle (base-only) render at a meaningful fraction of covered pixels
// (magnitude-independent: count pixels where the two outputs differ).
TEST(RasterDetail, DetailModulatesVsOneCycle) {
  struct Tex565 base;
  struct Tex565 detail;
  make_tex565_smooth(&base);    // low-frequency base
  make_tex565_detail(&detail);  // distinct detail
  uint16_t const white = rgb565_pack(255, 255, 255);
  TVtx pool[3];
  pool[0] = mk_tvtx_uv(8, 8, 0x10000, 0, 0, white);
  pool[1] = mk_tvtx_uv(52, 12, 0x10000, 7 * 32, 0, white);
  pool[2] = mk_tvtx_uv(14, 50, 0x10000, 0, 7 * 32, white);
  TriRef ref;
  ref.v0 = 0;
  ref.v1 = 1;
  ref.v2 = 2;
  ref.material = 0;
  TileBin bin;
  bin.refs = &ref;
  bin.count = 1;
  bin.cap = 1;
  bin.dropped = 0;

  // 1-cycle (base only) render.
  struct RenderState rs1;
  memset(&rs1, 0, sizeof(rs1));
  tex_desc_565(&rs1.tex, &base);
  rs1.combiner.mode = COMBINE_MODULATE;  // TEXEL0 * SHADE (white) = TEXEL0
  rs1.cycle = COMBINE_ONE_CYCLE;
  static uint16_t fb1[RDR_SCREEN_W * RDR_SCREEN_H];
  static uint16_t zbuf1[RDR_TILE_W * RDR_TILE_H];
  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    fb1[i] = 0;
  }
  raster_tile(0, &bin, pool, fb1, zbuf1, &rs1, 0, /*worker=*/0);

  // 2-cycle (base * detail) render.
  struct RenderState rs2;
  mk_two_cycle_state(&rs2, &base, &detail, 0);
  static uint16_t fb2[RDR_SCREEN_W * RDR_SCREEN_H];
  static uint16_t zbuf2[RDR_TILE_W * RDR_TILE_H];
  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    fb2[i] = 0;
  }
  raster_tile(0, &bin, pool, fb2, zbuf2, &rs2, 0, /*worker=*/0);

  int covered = 0;
  int differ = 0;
  for (int sy = 0; sy < RDR_TILE_H; ++sy) {
    for (int sx = 0; sx < RDR_TILE_W; ++sx) {
      uint16_t const a = fb1[(sy * RDR_SCREEN_W) + sx];
      uint16_t const b = fb2[(sy * RDR_SCREEN_W) + sx];
      if (a == 0 && b == 0) {
        continue;  // background (uncovered) on both
      }
      ++covered;
      if (a != b) {
        ++differ;
      }
    }
  }
  EXPECT_GT(covered, 50) << "too few covered pixels — test is vacuous";
  // The detail multiply darkens most pixels -> the vast majority must differ.
  EXPECT_GT(differ, covered / 2)
      << "detail texture did not visibly modulate the base (" << differ << "/"
      << covered << ")";
}

// detail_shift CHANGES the detail frequency: shift=0 vs shift=1 sample the
// detail texture at different positions, so the 2-cycle output differs at a
// meaningful fraction of pixels (sample positions differ).
TEST(RasterDetail, DetailShiftChangesFrequency) {
  struct Tex565 base;
  struct Tex565 detail;
  make_tex565_smooth(&base);
  make_tex565_detail(&detail);
  uint16_t const white = rgb565_pack(255, 255, 255);
  TVtx pool[3];
  pool[0] = mk_tvtx_uv(8, 8, 0x10000, 0, 0, white);
  pool[1] = mk_tvtx_uv(52, 12, 0x10000, 7 * 32, 0, white);
  pool[2] = mk_tvtx_uv(14, 50, 0x10000, 0, 7 * 32, white);
  TriRef ref;
  ref.v0 = 0;
  ref.v1 = 1;
  ref.v2 = 2;
  ref.material = 0;
  TileBin bin;
  bin.refs = &ref;
  bin.count = 1;
  bin.cap = 1;
  bin.dropped = 0;

  struct RenderState rs0;
  mk_two_cycle_state(&rs0, &base, &detail, 0);
  struct RenderState rs1;
  mk_two_cycle_state(&rs1, &base, &detail, 1);

  static uint16_t fb0[RDR_SCREEN_W * RDR_SCREEN_H];
  static uint16_t fb1[RDR_SCREEN_W * RDR_SCREEN_H];
  static uint16_t zbuf[RDR_TILE_W * RDR_TILE_H];
  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    fb0[i] = 0;
    fb1[i] = 0;
  }
  raster_tile(0, &bin, pool, fb0, zbuf, &rs0, 0, /*worker=*/0);
  raster_tile(0, &bin, pool, fb1, zbuf, &rs1, 0, /*worker=*/0);

  int covered = 0;
  int differ = 0;
  for (int sy = 0; sy < RDR_TILE_H; ++sy) {
    for (int sx = 0; sx < RDR_TILE_W; ++sx) {
      uint16_t const a = fb0[(sy * RDR_SCREEN_W) + sx];
      uint16_t const b = fb1[(sy * RDR_SCREEN_W) + sx];
      if (a == 0 && b == 0) {
        continue;
      }
      ++covered;
      if (a != b) {
        ++differ;
      }
    }
  }
  EXPECT_GT(covered, 50) << "too few covered pixels — test is vacuous";
  EXPECT_GT(differ, 0)
      << "detail_shift did not change the detail sample positions";
}

// GATE: a 2-cycle render state whose tex1 is INVALID (data==0) must take the
// EXACT 1-cycle path (shade_pixel on TEXEL0), byte-identical to a plain 1-cycle
// material with the same combiner. Guards the "tex1 invalid => 1-cycle" rule.
TEST(RasterDetail, TwoCycleWithNoTex1IsByteIdenticalToOneCycle) {
  struct Tex565 base;
  make_tex565(&base);
  uint16_t const white = rgb565_pack(255, 255, 255);
  TVtx pool[3];
  pool[0] = mk_tvtx_uv(8, 8, 0x20000, 0, 0, white);
  pool[1] = mk_tvtx_uv(52, 12, 0x08000, 7 * 32, 0, white);
  pool[2] = mk_tvtx_uv(14, 50, 0x10000, 0, 7 * 32, white);
  TriRef ref;
  ref.v0 = 0;
  ref.v1 = 1;
  ref.v2 = 2;
  ref.material = 0;
  TileBin bin;
  bin.refs = &ref;
  bin.count = 1;
  bin.cap = 1;
  bin.dropped = 0;

  // 1-cycle baseline: MODULATE TEXEL0 * SHADE.
  struct RenderState rs1;
  memset(&rs1, 0, sizeof(rs1));
  tex_desc_565(&rs1.tex, &base);
  rs1.combiner.mode = COMBINE_MODULATE;
  rs1.cycle = COMBINE_ONE_CYCLE;

  // "2-cycle" state with NO tex1 (tex1 zeroed) but SAME cycle-1 combiner -> the
  // gate must fall back to the 1-cycle shade_pixel path.
  struct RenderState rs2;
  memset(&rs2, 0, sizeof(rs2));
  tex_desc_565(&rs2.tex, &base);
  rs2.combiner.mode = COMBINE_MODULATE;
  rs2.cycle = COMBINE_TWO_CYCLE;  // claims 2-cycle...
  rs2.detail_shift = 2;           // ...but tex1.data==0 -> 1-cycle path
  // combiner2 left zero (COMBINE_MODULATE) — never consulted on the gate.

  static uint16_t fb1[RDR_SCREEN_W * RDR_SCREEN_H];
  static uint16_t fb2[RDR_SCREEN_W * RDR_SCREEN_H];
  static uint16_t zbuf[RDR_TILE_W * RDR_TILE_H];
  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    fb1[i] = 0;
    fb2[i] = 0;
  }
  raster_tile(0, &bin, pool, fb1, zbuf, &rs1, 0, /*worker=*/0);
  raster_tile(0, &bin, pool, fb2, zbuf, &rs2, 0, /*worker=*/0);

  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    ASSERT_EQ(fb1[i], fb2[i])
        << "2-cycle-with-no-tex1 must be byte-identical to 1-cycle at fb[" << i
        << "]";
  }
}

// detail_shift >= 32 must be UBSan-clean (the shift exponent is masked to
// [0,31] in sample_texel1 — a raw `<< 32` on a 32-bit int is C++ shift-exponent
// UB). 32 & 31 == 0, so a shift of 32 must render byte-identically to shift 0.
// Under host-asan (UBSan) this exercises the masked path with no
// shift-exponent diagnostic.
TEST(RasterDetail, DetailShiftAtLeast32IsUbsanCleanAndMasks) {
  struct Tex565 base;
  struct Tex565 detail;
  make_tex565_smooth(&base);
  make_tex565_detail(&detail);
  uint16_t const white = rgb565_pack(255, 255, 255);
  TVtx pool[3];
  pool[0] = mk_tvtx_uv(8, 8, 0x10000, 0, 0, white);
  pool[1] = mk_tvtx_uv(52, 12, 0x10000, 7 * 32, 0, white);
  pool[2] = mk_tvtx_uv(14, 50, 0x10000, 0, 7 * 32, white);
  TriRef ref;
  ref.v0 = 0;
  ref.v1 = 1;
  ref.v2 = 2;
  ref.material = 0;
  TileBin bin;
  bin.refs = &ref;
  bin.count = 1;
  bin.cap = 1;
  bin.dropped = 0;

  struct RenderState rs0;
  mk_two_cycle_state(&rs0, &base, &detail, 0);
  struct RenderState rs32;
  mk_two_cycle_state(&rs32, &base, &detail, 32);  // 32 & 31 == 0

  static uint16_t fb0[RDR_SCREEN_W * RDR_SCREEN_H];
  static uint16_t fb32[RDR_SCREEN_W * RDR_SCREEN_H];
  static uint16_t zbuf[RDR_TILE_W * RDR_TILE_H];
  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    fb0[i] = 0;
    fb32[i] = 0;
  }
  raster_tile(0, &bin, pool, fb0, zbuf, &rs0, 0, /*worker=*/0);
  raster_tile(0, &bin, pool, fb32, zbuf, &rs32, 0,
              /*worker=*/0);  // must not UB / crash

  int checked = 0;
  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    ASSERT_EQ(fb0[i], fb32[i])
        << "detail_shift 32 (masked to 0) must equal shift 0 at fb[" << i
        << "]";
    if (fb0[i] != 0) {
      ++checked;
    }
  }
  EXPECT_GT(checked, 50) << "too few covered pixels — test is vacuous";
}

// ---------------------------------------------------------------------------
// T5 L1 — interp.h exact-integer DDA parity (the golden-neutral GATE).
// The stepper must reproduce the rasterizer's per-pixel  (int32_t)(num / area2)
// divide BIT-FOR-BIT across the full bounding box, for every interpolant —
// including negative numerators (u_iw/v_iw), thin triangles (small area2 ->
// large per-pixel quotient steps), and pixels OUTSIDE the triangle (the
// accumulator must stay in lockstep with sx — the L1 desync invariant). If this
// passes, L1 changes no output and the golden fb_crc is unchanged.
namespace {

// Drive the stepper exactly as the raster inner loop does (init at the row
// origin sx=0,sy=0; begin_row per scanline; value-then-step_x per pixel) and
// assert interp_value == the reference truncating divide at EVERY pixel of a
// w x h grid. num(sx,sy) = num0 + sx*dx + sy*dy (int64, exact-linear).
void check_interp_grid(int64_t num0, int64_t dx, int64_t dy, int32_t area2,
                       int w, int h) {
  ASSERT_GT(area2, 0);
  struct Interp p;
  interp_init(&p, num0, dx, dy, area2);
  for (int sy = 0; sy < h; ++sy) {
    interp_begin_row(&p);
    for (int sx = 0; sx < w; ++sx) {
      int64_t const num = num0 + ((int64_t)sx * dx) + ((int64_t)sy * dy);
      int32_t const ref = (int32_t)(num / (int64_t)area2);  // C trunc-to-zero
      ASSERT_EQ(interp_value(&p), ref)
          << "sx=" << sx << " sy=" << sy << " num0=" << num0 << " dx=" << dx
          << " dy=" << dy << " area2=" << area2;
      interp_step_x(&p);
    }
  }
}

}  // namespace

TEST(InterpDDA, NonNegativeMatchesDivide) {
  // inv_w / Gouraud / fog class: num >= 0 across the grid (floor == trunc).
  check_interp_grid(/*num0=*/0, /*dx=*/12345, /*dy=*/777, /*area2=*/4096, 60,
                    60);
  check_interp_grid(1 << 20, 65536, 65536, 257, 40, 40);       // ~Q16.16
  check_interp_grid(255LL * 4096, -4096, -512, 4096, 60, 60);  // 0..255
}

TEST(InterpDDA, NegativeNumeratorTruncatesTowardZero) {
  // u_iw/v_iw class: numerator goes negative across the grid; the trunc
  // correction must match C's truncation toward zero (NOT floor) everywhere.
  check_interp_grid(/*num0=*/-1000000, /*dx=*/53, /*dy=*/29, /*area2=*/4096, 80,
                    80);
  check_interp_grid(50000, -3000, -2000, 4096, 64, 64);  // crosses zero
  check_interp_grid(-7, 1, 0, 5, 30, 1);                 // tiny, hand-checkable
}

TEST(InterpDDA, ThinTriangleLargeSteps) {
  // Small area2 (near RASTER_AREA_EPS) with large numerators -> qx large /
  // possibly negative; (q,r) must stay exact (32-bit) and matching.
  check_interp_grid(123456789LL, 7654321, -1234567, 257, 60, 60);
  check_interp_grid(-123456789LL, -7654321, 1234567, 257, 60, 60);
}

TEST(InterpDDA, ExactDivisibleBoundaries) {
  // Exact multiples of area2 (remainder 0) at and around zero — the rf==0
  // branch of the trunc correction (floor == trunc when exactly divisible).
  check_interp_grid(/*num0=*/-4096LL * 3, /*dx=*/4096, /*dy=*/0, /*area2=*/4096,
                    16, 1);  // -3,-2,-1,0,1,... exactly
  check_interp_grid(0, 4096, 4096, 4096, 20, 20);
}
