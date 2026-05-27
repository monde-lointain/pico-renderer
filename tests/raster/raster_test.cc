// raster_test.cc — Stream B.1-gamma raster tests (flat fill + per-tile Z).
// Host-first TDD. Golden reference = tests/harness oracle_fill_tri (top-left
// fill rule + winding normalization). We render one tile with raster_tile and
// compare its color output pixel-for-pixel against the oracle filling the same
// triangle (tile-local coordinates), proving fill-rule + coverage parity.
#include "raster/raster.h"

#include <stdint.h>
#include <string.h>

#include "gtest/gtest.h"
#include "oracle.h"
#include "rdr/config.h"
#include "rdr/types.h"

namespace {

// RGB565 flat color used across tests (a non-trivial value to catch packing
// bugs). 565: R=10110(22), G=011010(26), B=01011(11).
const uint16_t kFlat565 = (uint16_t)((22u << 11) | (26u << 5) | 11u);

// rgb565 packer matching gfx/framebuffer.h rgb565() (avoid pulling that header).
uint16_t rgb565_pack(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)((((uint16_t)(r & 0xF8)) << 8) |
                    (((uint16_t)(g & 0xFC)) << 3) | (uint16_t)(b >> 3));
}

// Build a TVtx at pixel (px,py) with subpixel offsets (in 1/16 px), depth, and
// the flat color. Q12.4: screen value = px*16 + sub.
TVtx mk_vtx(int px, int py, int sub_x, int sub_y, int32_t inv_w_q16) {
  TVtx v;
  memset(&v, 0, sizeof(v));
  v.x = (fx12_4)((px * 16) + sub_x);
  v.y = (fx12_4)((py * 16) + sub_y);
  v.inv_w = (fx_invw)inv_w_q16;
  v.rgba = kFlat565;
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
  oracle_unpack565(kFlat565, r, g, b);
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
  raster_tile(0, bin, pool, fb, zbuf);
  for (int y = 0; y < RDR_TILE_H; ++y) {
    for (int x = 0; x < RDR_TILE_W; ++x) {
      uint16_t const px = fb[(y * RDR_SCREEN_W) + x];
      uint8_t r, g, b;
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
  uint8_t fr, fg, fb;
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
  oracle_fill_tri(&img, o->pool[0].x / 16.0F, o->pool[0].y / 16.0F,
                  o->pool[1].x / 16.0F, o->pool[1].y / 16.0F,
                  o->pool[2].x / 16.0F, o->pool[2].y / 16.0F, fr, fg, fb);

  int diff = 0;
  for (int i = 0; i < RDR_TILE_W * RDR_TILE_H * 3; ++i) {
    if (rast[i] != ref[i]) {
      ++diff;
    }
  }
  return diff;
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
