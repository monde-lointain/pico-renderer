// blit2d_test.cc — host golden tests for the 2D sky background blits.
// References: docs/superpowers/specs design (panorama / sky background),
// N64 GBI GPACK_RGBA5551 (R[15:11] G[10:6] B[5:1] A[0]), gfx/framebuffer.h
// rgb565() packer. Orthodox C++ (the test TU itself is plain C-with-gtest).
//
// Strategy (golden-image-test / float-oracle): each integer/fixed kernel is
// checked against a self-contained float reference computed in the test, plus
// exact spot values. Panorama is verified by re-deriving the source-column
// wrap + RGBA5551->565 decode; clouds by re-deriving the gradient + alpha-over.
#include "blit2d/blit2d.h"

#include <stdint.h>
#include <string.h>

#include "gfx/framebuffer.h"
#include "gtest/gtest.h"

// ---- RGBA5551 helpers (test-side oracle) -----------------------------------

// Pack r,g,b,a (each pre-scaled to its field width) into N64-native RGBA5551.
static uint16_t pack5551(int r5, int g5, int b5, int a1) {
  return (uint16_t)(((r5 & 0x1F) << 11) | ((g5 & 0x1F) << 6) |
                    ((b5 & 0x1F) << 1) | (a1 & 1));
}

// Oracle: convert an RGBA5551 word to RGB565 (5->6 green expand, alpha dropped).
static uint16_t ref_5551_to_565(uint16_t px) {
  int r5 = (px >> 11) & 0x1F;
  int g5 = (px >> 6) & 0x1F;
  int b5 = (px >> 1) & 0x1F;
  int g6 = (g5 << 1) | (g5 >> 4);
  return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

// ============================================================================
// blit2d_decode_ci8 — CI8 index fetch + RGBA5551 TLUT -> RGB565, POINT sample.
// ============================================================================

TEST(Blit2dDecodeCi8, NullSrcOrTlutReturnsZero) {
  uint16_t tlut[256];
  uint8_t ci8[4] = {0, 1, 2, 3};
  memset(tlut, 0xFF, sizeof tlut);
  EXPECT_EQ(blit2d_decode_ci8(nullptr, tlut, 2, 0, 0), 0);
  EXPECT_EQ(blit2d_decode_ci8(ci8, nullptr, 2, 0, 0), 0);
}

TEST(Blit2dDecodeCi8, IndexFetchPointSampledMatchesOracle) {
  // 4x2 index image; palette maps index i -> a distinct 5551 color.
  uint8_t ci8[8] = {0, 1, 2, 3, 4, 5, 6, 7};
  uint16_t tlut[256];
  memset(tlut, 0, sizeof tlut);
  tlut[0] = pack5551(31, 0, 0, 1);   // red
  tlut[1] = pack5551(0, 31, 0, 0);   // green
  tlut[2] = pack5551(0, 0, 31, 1);   // blue
  tlut[3] = pack5551(31, 31, 31, 0); // white
  tlut[4] = pack5551(15, 7, 3, 1);
  tlut[5] = pack5551(1, 2, 4, 0);
  tlut[6] = pack5551(10, 20, 30, 1);
  tlut[7] = pack5551(31, 0, 31, 0);
  int const w = 4;
  for (int y = 0; y < 2; ++y) {
    for (int x = 0; x < 4; ++x) {
      uint8_t idx = ci8[y * w + x];
      uint16_t want = ref_5551_to_565(tlut[idx]);
      EXPECT_EQ(blit2d_decode_ci8(ci8, tlut, w, x, y), want)
          << "x=" << x << " y=" << y;
    }
  }
}

TEST(Blit2dDecodeCi8, AlphaBitDroppedOpaqueTarget) {
  // Same color bits, differing alpha bit -> identical RGB565 (565 is opaque).
  uint8_t ci8[2] = {0, 1};
  uint16_t tlut[256];
  memset(tlut, 0, sizeof tlut);
  tlut[0] = pack5551(17, 9, 25, 0);
  tlut[1] = pack5551(17, 9, 25, 1);
  EXPECT_EQ(blit2d_decode_ci8(ci8, tlut, 2, 0, 0),
            blit2d_decode_ci8(ci8, tlut, 2, 1, 0));
}

// ============================================================================
// blit2d_decode_i8 — raw intensity (used as cloud alpha).
// ============================================================================

TEST(Blit2dDecodeI8, ReturnsRawIntensity) {
  uint8_t i8[6] = {0, 64, 127, 128, 200, 255};
  for (int x = 0; x < 6; ++x) EXPECT_EQ(blit2d_decode_i8(i8, 6, x, 0), i8[x]);
}

TEST(Blit2dDecodeI8, NullSrcReturnsZero) {
  EXPECT_EQ(blit2d_decode_i8(nullptr, 4, 0, 0), 0);
}

TEST(Blit2dDecodeI8, RowMajorIndexing) {
  uint8_t i8[6] = {10, 20, 30, 40, 50, 60};  // 3 wide, 2 tall
  EXPECT_EQ(blit2d_decode_i8(i8, 3, 2, 1), 60);
  EXPECT_EQ(blit2d_decode_i8(i8, 3, 0, 1), 40);
}

// ============================================================================
// blit2d_horizon_row_1to1 — seam-free vertical rescale (4:3 source -> 1:1).
// ============================================================================

TEST(Blit2dHorizon, RescaleMatchesRoundedRatio) {
  // dst row = round(src_horizon_row * dst_h / src_h).
  struct {
    int src_row, src_h, dst_h, want;
  } cases[] = {
      {120, 240, 240, 120},  // identity height -> identity
      {120, 240, 120, 60},   // half-height band
      {90, 240, 240, 90},    // upper horizon stays put at equal height
      {160, 320, 240, 120},  // 320-tall source into 240 panel
      {0, 240, 240, 0},      // top edge
      {239, 240, 240, 239},  // bottom edge
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
    EXPECT_EQ(blit2d_horizon_row_1to1(cases[i].src_row, cases[i].src_h,
                                      cases[i].dst_h),
              cases[i].want)
        << "case " << i;
  }
}

TEST(Blit2dHorizon, DegenerateSrcHeightClampsZero) {
  EXPECT_EQ(blit2d_horizon_row_1to1(100, 0, 240), 0);
}

// ============================================================================
// blit2d_panorama — scrolling cylinder assembly (CI8 + TLUT).
// ============================================================================

// Build a small panorama descriptor over a synthetic CI8 source.
static void make_pano(struct Blit2dRect* r, const uint8_t* src,
                      const uint16_t* tlut, int src_w, int src_h, int scroll_x) {
  memset(r, 0, sizeof *r);
  r->mode = BLIT2D_PANORAMA;
  r->src = src;
  r->tlut = tlut;
  r->src_w = (uint16_t)src_w;
  r->src_h = (uint16_t)src_h;
  r->dst_x = 0;
  r->dst_y = 0;
  r->dst_w = SCREEN_W;
  r->dst_h = SCREEN_H;
  r->scroll_x = (uint16_t)scroll_x;
  r->elevation = 0;
  r->horizon_row = SCREEN_H / 2;
}

TEST(Blit2dPanorama, RejectsBadDescriptor) {
  struct Framebuffer fb;
  struct Blit2dRect r;
  uint8_t src[4] = {0, 0, 0, 0};
  uint16_t tlut[256];
  memset(tlut, 0, sizeof tlut);
  make_pano(&r, src, tlut, 2, 2, 0);
  EXPECT_EQ(blit2d_panorama(&r, nullptr), RDR_EINVAL);
  r.mode = BLIT2D_CLOUDS;  // wrong mode for this entry point
  EXPECT_EQ(blit2d_panorama(&r, fb.px), RDR_EINVAL);
  make_pano(&r, src, tlut, 0, 2, 0);  // zero src_w
  EXPECT_EQ(blit2d_panorama(&r, fb.px), RDR_EINVAL);
}

TEST(Blit2dPanorama, HorizontalWrapMatchesOracle) {
  // 8-wide source (the "cylinder"); the dst is wider than src -> must wrap.
  // Each source column gets a unique palette color so a mis-wrap is visible.
  enum { SRCW = 8, SRCH = 4 };
  uint8_t src[SRCW * SRCH];
  uint16_t tlut[256];
  memset(tlut, 0, sizeof tlut);
  for (int i = 0; i < 256; ++i) tlut[i] = pack5551(i & 31, (i >> 1) & 31, 7, 0);
  for (int y = 0; y < SRCH; ++y)
    for (int x = 0; x < SRCW; ++x) src[y * SRCW + x] = (uint8_t)x;  // col index

  struct Framebuffer fb;
  memset(&fb, 0, sizeof fb);
  struct Blit2dRect r;
  int const scroll = 5;
  make_pano(&r, src, tlut, SRCW, SRCH, scroll);
  ASSERT_EQ(blit2d_panorama(&r, fb.px), RDR_OK);

  // Top dst row maps to source row 0 (elevation 0, horizon at mid). Column c
  // -> source column (scroll + c) % SRCW.
  for (int c = 0; c < SCREEN_W; ++c) {
    int sx = (scroll + c) % SRCW;
    uint16_t want = ref_5551_to_565(tlut[(uint8_t)sx]);
    EXPECT_EQ(fb.px[0 * SCREEN_W + c], want) << "col=" << c;
  }
}

TEST(Blit2dPanorama, VerticalBandSelectsSourceRows) {
  // Constant per source ROW so we can verify the dst row -> source row map.
  enum { SRCW = 4, SRCH = 8 };
  uint8_t src[SRCW * SRCH];
  uint16_t tlut[256];
  memset(tlut, 0, sizeof tlut);
  for (int i = 0; i < 256; ++i) tlut[i] = pack5551(i & 31, 0, 0, 0);
  for (int y = 0; y < SRCH; ++y)
    for (int x = 0; x < SRCW; ++x) src[y * SRCW + x] = (uint8_t)y;  // row index

  struct Framebuffer fb;
  memset(&fb, 0, sizeof fb);
  struct Blit2dRect r;
  make_pano(&r, src, tlut, SRCW, SRCH, 0);
  r.dst_h = SCREEN_H;
  ASSERT_EQ(blit2d_panorama(&r, fb.px), RDR_OK);

  // dst row r -> source row = (r * SRCH) / dst_h (vertical rescale, no scroll
  // wrap vertically — clamp band). Verify against that oracle.
  for (int row = 0; row < SCREEN_H; ++row) {
    int sy = (row * SRCH) / SCREEN_H;
    if (sy >= SRCH) sy = SRCH - 1;
    uint16_t want = ref_5551_to_565(tlut[(uint8_t)sy]);
    EXPECT_EQ(fb.px[row * SCREEN_W + 0], want) << "row=" << row;
  }
}

TEST(Blit2dPanorama, ElevationShiftsBand) {
  // elevation adds a source-row offset (camera pitch). Same source as above.
  enum { SRCW = 4, SRCH = 8 };
  uint8_t src[SRCW * SRCH];
  uint16_t tlut[256];
  memset(tlut, 0, sizeof tlut);
  for (int i = 0; i < 256; ++i) tlut[i] = pack5551(i & 31, 0, 0, 0);
  for (int y = 0; y < SRCH; ++y)
    for (int x = 0; x < SRCW; ++x) src[y * SRCW + x] = (uint8_t)y;

  struct Framebuffer fb0, fb1;
  memset(&fb0, 0, sizeof fb0);
  memset(&fb1, 0, sizeof fb1);
  struct Blit2dRect r0, r1;
  make_pano(&r0, src, tlut, SRCW, SRCH, 0);
  make_pano(&r1, src, tlut, SRCW, SRCH, 0);
  r1.elevation = 2;  // shift the band down by 2 source rows
  ASSERT_EQ(blit2d_panorama(&r0, fb0.px), RDR_OK);
  ASSERT_EQ(blit2d_panorama(&r1, fb1.px), RDR_OK);
  // Row 0 with elevation 2 must equal the source row reached at base map + 2.
  int base_sy = (0 * SRCH) / SCREEN_H;
  int el_sy = base_sy + 2;
  if (el_sy >= SRCH) el_sy = SRCH - 1;
  EXPECT_EQ(fb1.px[0], ref_5551_to_565(tlut[(uint8_t)el_sy]));
}

// ============================================================================
// blit2d_clouds — I8 alpha-over a procedural blue gradient.
// ============================================================================

// Oracle: per-channel alpha-over of cloud over grad with rounding.
static int over8(int a, int cloud, int grad) {
  return (a * cloud + (255 - a) * grad + 127) / 255;
}

// Oracle: vertical gradient row -> RGB565, lerp sky_top..sky_horizon over the
// band [dst_y, horizon_row]; clamped below the horizon to sky_horizon.
static void grad_565(uint16_t top, uint16_t hor, int row, int dst_y,
                     int horizon_row, int* r, int* g, int* b) {
  int tr = (top >> 11) & 0x1F, tg = (top >> 5) & 0x3F, tb = top & 0x1F;
  int hr = (hor >> 11) & 0x1F, hg = (hor >> 5) & 0x3F, hb = hor & 0x1F;
  int span = horizon_row - dst_y;
  int t = row - dst_y;
  if (span <= 0) {
    *r = hr; *g = hg; *b = hb; return;
  }
  if (t < 0) t = 0;
  if (t > span) t = span;
  *r = tr + (hr - tr) * t / span;
  *g = tg + (hg - tg) * t / span;
  *b = tb + (hb - tb) * t / span;
}

TEST(Blit2dClouds, RejectsBadDescriptor) {
  struct Framebuffer fb;
  struct Blit2dRect r;
  uint8_t src[4] = {0, 0, 0, 0};
  memset(&r, 0, sizeof r);
  r.mode = BLIT2D_CLOUDS;
  r.src = src;
  r.src_w = 2;
  r.src_h = 2;
  r.dst_w = SCREEN_W;
  r.dst_h = SCREEN_H;
  r.horizon_row = SCREEN_H / 2;
  EXPECT_EQ(blit2d_clouds(&r, nullptr), RDR_EINVAL);
  r.mode = BLIT2D_PANORAMA;  // wrong mode
  EXPECT_EQ(blit2d_clouds(&r, fb.px), RDR_EINVAL);
}

TEST(Blit2dClouds, ZeroAlphaIsPureGradient) {
  enum { SRCW = 4, SRCH = 4 };
  uint8_t src[SRCW * SRCH];
  memset(src, 0, sizeof src);  // alpha 0 everywhere
  struct Framebuffer fb;
  memset(&fb, 0, sizeof fb);
  struct Blit2dRect r;
  memset(&r, 0, sizeof r);
  r.mode = BLIT2D_CLOUDS;
  r.src = src;
  r.src_w = SRCW;
  r.src_h = SRCH;
  r.dst_x = 0;
  r.dst_y = 0;
  r.dst_w = SCREEN_W;
  r.dst_h = SCREEN_H;
  r.horizon_row = SCREEN_H - 1;
  r.sky_top = rgb565(20, 40, 200);
  r.sky_horizon = rgb565(180, 200, 240);
  r.cloud_color = rgb565(255, 255, 255);
  ASSERT_EQ(blit2d_clouds(&r, fb.px), RDR_OK);
  for (int row = 0; row < SCREEN_H; ++row) {
    int gr, gg, gb;
    grad_565(r.sky_top, r.sky_horizon, row, 0, r.horizon_row, &gr, &gg, &gb);
    uint16_t want = (uint16_t)((gr << 11) | (gg << 5) | gb);
    EXPECT_EQ(fb.px[row * SCREEN_W + 0], want) << "row=" << row;
  }
}

TEST(Blit2dClouds, FullAlphaIsPureCloud) {
  enum { SRCW = 4, SRCH = 4 };
  uint8_t src[SRCW * SRCH];
  memset(src, 255, sizeof src);  // alpha 255 everywhere
  struct Framebuffer fb;
  memset(&fb, 0, sizeof fb);
  struct Blit2dRect r;
  memset(&r, 0, sizeof r);
  r.mode = BLIT2D_CLOUDS;
  r.src = src;
  r.src_w = SRCW;
  r.src_h = SRCH;
  r.dst_w = SCREEN_W;
  r.dst_h = SCREEN_H;
  r.horizon_row = SCREEN_H - 1;
  r.sky_top = rgb565(20, 40, 200);
  r.sky_horizon = rgb565(180, 200, 240);
  r.cloud_color = rgb565(248, 252, 248);  // 565-exact white-ish
  ASSERT_EQ(blit2d_clouds(&r, fb.px), RDR_OK);
  // a=255 -> over8 returns the cloud channel exactly -> whole frame == cloud.
  for (int i = 0; i < SCREEN_PIXELS; ++i) EXPECT_EQ(fb.px[i], r.cloud_color);
}

TEST(Blit2dClouds, MidAlphaMatchesOracle) {
  // A single mid-alpha texel -> full-frame constant (source point-samples to
  // the same texel since src is constant). Check the alpha-over math.
  enum { SRCW = 2, SRCH = 2 };
  uint8_t src[SRCW * SRCH] = {128, 128, 128, 128};
  struct Framebuffer fb;
  memset(&fb, 0, sizeof fb);
  struct Blit2dRect r;
  memset(&r, 0, sizeof r);
  r.mode = BLIT2D_CLOUDS;
  r.src = src;
  r.src_w = SRCW;
  r.src_h = SRCH;
  r.dst_w = SCREEN_W;
  r.dst_h = SCREEN_H;
  r.horizon_row = SCREEN_H - 1;
  r.sky_top = rgb565(0, 0, 255);
  r.sky_horizon = rgb565(255, 255, 255);
  r.cloud_color = rgb565(255, 255, 255);
  ASSERT_EQ(blit2d_clouds(&r, fb.px), RDR_OK);
  int const a = 128;
  int cr = (r.cloud_color >> 11) & 0x1F, cg = (r.cloud_color >> 5) & 0x3F,
      cb = r.cloud_color & 0x1F;
  for (int row = 0; row < SCREEN_H; ++row) {
    int gr, gg, gb;
    grad_565(r.sky_top, r.sky_horizon, row, 0, r.horizon_row, &gr, &gg, &gb);
    int wr = over8(a, cr, gr), wg = over8(a, cg, gg), wb = over8(a, cb, gb);
    uint16_t want = (uint16_t)((wr << 11) | (wg << 5) | wb);
    EXPECT_EQ(fb.px[row * SCREEN_W + 0], want) << "row=" << row;
  }
}

// ============================================================================
// blit2d_render — dispatch.
// ============================================================================

TEST(Blit2dRender, UnknownModeRejected) {
  struct Framebuffer fb;
  struct Blit2dRect r;
  memset(&r, 0, sizeof r);
  r.mode = 200;  // not a Blit2dMode
  EXPECT_EQ(blit2d_render(&r, fb.px), RDR_EINVAL);
}

TEST(Blit2dRender, DispatchesPanorama) {
  enum { SRCW = 4, SRCH = 4 };
  uint8_t src[SRCW * SRCH];
  uint16_t tlut[256];
  memset(tlut, 0, sizeof tlut);
  for (int i = 0; i < 256; ++i) tlut[i] = pack5551(i & 31, 0, 0, 0);
  memset(src, 2, sizeof src);
  struct Framebuffer a, b;
  memset(&a, 0, sizeof a);
  memset(&b, 0, sizeof b);
  struct Blit2dRect r;
  make_pano(&r, src, tlut, SRCW, SRCH, 0);
  ASSERT_EQ(blit2d_panorama(&r, a.px), RDR_OK);
  ASSERT_EQ(blit2d_render(&r, b.px), RDR_OK);
  EXPECT_EQ(memcmp(a.px, b.px, sizeof a.px), 0);
}
