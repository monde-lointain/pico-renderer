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

#include "golden.h"
#include "gtest/gtest.h"
#include "oracle.h"
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
  raster_tile(0, &bin, pool, fb, zbuf, table);

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
  raster_tile(0, &bin, pool, fb, zbuf, &rs);

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
  raster_tile(0, &bin, pool, fb, zbuf, table);

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
    raster_tile(tile, &bin, pool, fb_serial, z0, table);
  }
  for (int tile = 0; tile < num_tiles; ++tile) {
    uint16_t* const z = (tile & 1) ? z1 : z0;
    raster_tile(tile, &bin, pool, fb_par, z, table);
  }
  EXPECT_EQ(memcmp(fb_serial, fb_par,
                   (size_t)(RDR_SCREEN_W * RDR_SCREEN_H) * sizeof(uint16_t)),
            0)
      << "textured dual-worker sweep diverged from serial — raster has shared "
         "mutable state";
}
