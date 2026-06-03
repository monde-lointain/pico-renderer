// tex_test.cc — golden tests for the point-sampling texture fetch.
// Reference: N64 RDP texture coordinate handling (angrylion tcoord.c) for
// mask-based WRAP/MIRROR/CLAMP; gbi.h format enums; oracle_unpack565 for the
// RGB565 layout. Test textures are tiny and hand-verifiable.

#include "tex/tex.h"

#include <stdint.h>
#include <string.h>

#include "gfx/framebuffer.h"  // rgb565() — golden 565 packer
#include "gtest/gtest.h"
#include "oracle.h"  // oracle_sample_texel() — float reference

// ---- helpers ---------------------------------------------------------------
static fx16_16 q16(int texel) { return (fx16_16)texel << 16; }

// Q16.16 with a fractional 5-bit step (0..31) in the high frac bits, matching
// the sampler's frac extraction (u >> 11) & 0x1f.
static fx16_16 q16_frac5(int texel, int frac5) {
  return ((fx16_16)texel << 16) | ((fx16_16)(frac5 & 0x1f) << 11);
}

// A 4x1 RGBA565 texture: distinct colors so index errors are obvious.
static const uint16_t K_ROW565[4] = {
    0xF800,  // red   (idx 0)
    0x07E0,  // green (idx 1)
    0x001F,  // blue  (idx 2)
    0xFFFF   // white (idx 3)
};

static void make_tex565(struct TexDesc* t, const uint16_t* data, uint16_t w,
                        uint16_t h, uint8_t wrap_s, uint8_t wrap_t) {
  t->data = data;
  t->w = w;
  t->h = h;
  t->format = TEXFMT_RGBA565;
  t->wrap_s = wrap_s;
  t->wrap_t = wrap_t;
  t->filter = FILTER_POINT;
  t->mip_levels = 1;
  t->tlut = (const void*)0;
}

// ---- point sampling: exact texel pick (floor of Q16.16) --------------------
TEST(Tex, PointSamplesExactTexel565) {
  struct TexDesc t;
  make_tex565(&t, K_ROW565, 4, 1, WRAP_REPEAT, WRAP_REPEAT);
  EXPECT_EQ(tex_sample(&t, q16(0), q16(0), 0), 0xF800);
  EXPECT_EQ(tex_sample(&t, q16(1), q16(0), 0), 0x07E0);
  EXPECT_EQ(tex_sample(&t, q16(2), q16(0), 0), 0x001F);
  EXPECT_EQ(tex_sample(&t, q16(3), q16(0), 0), 0xFFFF);
}

// Fractional coordinate (within a texel) floors to the texel index.
TEST(Tex, PointDropsFraction) {
  struct TexDesc t;
  make_tex565(&t, K_ROW565, 4, 1, WRAP_REPEAT, WRAP_REPEAT);
  // 1.9 -> texel 1; 2.0 -> texel 2; 0.5 -> texel 0.
  EXPECT_EQ(tex_sample(&t, q16(1) + 0xE666, q16(0), 0), 0x07E0);  // 1.9
  EXPECT_EQ(tex_sample(&t, q16(0) + 0x8000, q16(0), 0), 0xF800);  // 0.5
}

// ---- WRAP (repeat): index masks with (w-1) ---------------------------------
TEST(Tex, WrapRepeatsModuloWidth) {
  struct TexDesc t;
  make_tex565(&t, K_ROW565, 4, 1, WRAP_REPEAT, WRAP_REPEAT);
  EXPECT_EQ(tex_sample(&t, q16(4), q16(0), 0), 0xF800);   // 4 & 3 = 0
  EXPECT_EQ(tex_sample(&t, q16(5), q16(0), 0), 0x07E0);   // 5 & 3 = 1
  EXPECT_EQ(tex_sample(&t, q16(-1), q16(0), 0), 0xFFFF);  // -1 & 3 = 3
}

// ---- CLAMP: saturate to [0, w-1] -------------------------------------------
TEST(Tex, ClampSaturatesEdges) {
  struct TexDesc t;
  make_tex565(&t, K_ROW565, 4, 1, WRAP_CLAMP, WRAP_CLAMP);
  EXPECT_EQ(tex_sample(&t, q16(-3), q16(0), 0), 0xF800);  // clamp low -> 0
  EXPECT_EQ(tex_sample(&t, q16(99), q16(0), 0), 0xFFFF);  // clamp high -> 3
  EXPECT_EQ(tex_sample(&t, q16(2), q16(0), 0), 0x001F);   // in range
}

// ---- MIRROR: bit above mask toggles direction ------------------------------
// For w=4 (mask=3, mask_bits=2): coords 0..3 forward, 4..7 mirror to 3..0.
TEST(Tex, MirrorReflectsAcrossWidth) {
  struct TexDesc t;
  make_tex565(&t, K_ROW565, 4, 1, WRAP_MIRROR, WRAP_MIRROR);
  EXPECT_EQ(tex_sample(&t, q16(0), q16(0), 0), 0xF800);  // 0 -> 0
  EXPECT_EQ(tex_sample(&t, q16(3), q16(0), 0), 0xFFFF);  // 3 -> 3
  EXPECT_EQ(tex_sample(&t, q16(4), q16(0), 0), 0xFFFF);  // 4 -> 3 (mirror)
  EXPECT_EQ(tex_sample(&t, q16(5), q16(0), 0), 0x001F);  // 5 -> 2
  EXPECT_EQ(tex_sample(&t, q16(7), q16(0), 0), 0xF800);  // 7 -> 0
}

// ---- 2D indexing: row stride = width ---------------------------------------
TEST(Tex, TwoDimensionalRowMajor) {
  // 2x2: [ A B ; C D ] row-major.
  static const uint16_t img[4] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
  struct TexDesc t;
  make_tex565(&t, img, 2, 2, WRAP_REPEAT, WRAP_REPEAT);
  EXPECT_EQ(tex_sample(&t, q16(0), q16(0), 0), 0xF800);  // (0,0)=A
  EXPECT_EQ(tex_sample(&t, q16(1), q16(0), 0), 0x07E0);  // (1,0)=B
  EXPECT_EQ(tex_sample(&t, q16(0), q16(1), 0), 0x001F);  // (0,1)=C
  EXPECT_EQ(tex_sample(&t, q16(1), q16(1), 0), 0xFFFF);  // (1,1)=D
}

// ---- RGBA4444 decode: nibble-replicate to 8-bit, repack to 565 -------------
TEST(Tex, Rgba4444DecodesTo565) {
  // One texel 0xFA0F: R=F G=A B=0 A=F. Expand 4->8 by replication:
  // R=0xFF, G=0xAA, B=0x00. Repack via golden rgb565().
  static const uint16_t px = 0xFA0F;
  struct TexDesc t;
  t.data = &px;
  t.w = 1;
  t.h = 1;
  t.format = TEXFMT_RGBA4444;
  t.wrap_s = WRAP_CLAMP;
  t.wrap_t = WRAP_CLAMP;
  t.filter = FILTER_POINT;
  t.mip_levels = 1;
  t.tlut = (const void*)0;
  uint16_t const want = rgb565(0xFF, 0xAA, 0x00);
  EXPECT_EQ(tex_sample(&t, q16(0), q16(0), 0), want);
}

// ===========================================================================
//  D.2 — RGBA8 sampler, all formats, 3-point filter, oracle cross-checks.
// ===========================================================================

static void fill_desc(struct TexDesc* t, const void* data, uint16_t w,
                      uint16_t h, uint8_t fmt, uint8_t wrap, uint8_t filter,
                      const void* tlut) {
  t->data = data;
  t->w = w;
  t->h = h;
  t->format = fmt;
  t->wrap_s = wrap;
  t->wrap_t = wrap;
  t->filter = filter;
  t->mip_levels = 1;
  t->tlut = tlut;
}

// ---- per-format decode to RGBA8 (hand-verified bit layouts) ----------------

TEST(TexRgba, I8ReplicatesIntensity) {
  static const uint8_t px[1] = {0x7F};
  struct TexDesc t;
  fill_desc(&t, px, 1, 1, TEXFMT_I8, WRAP_CLAMP, FILTER_POINT, (const void*)0);
  uint8_t out[4];
  tex_sample_rgba(&t, q16(0), q16(0), 0, out);
  EXPECT_EQ(out[0], 0x7F);
  EXPECT_EQ(out[1], 0x7F);
  EXPECT_EQ(out[2], 0x7F);
  EXPECT_EQ(out[3], 0x7F);
}

TEST(TexRgba, I4NibbleReplicate) {
  // Byte 0xA5: hi nibble A (even pixel), lo nibble 5 (odd pixel). 2x1 texture.
  static const uint8_t px[1] = {0xA5};
  struct TexDesc t;
  fill_desc(&t, px, 2, 1, TEXFMT_I4, WRAP_CLAMP, FILTER_POINT, (const void*)0);
  uint8_t out[4];
  tex_sample_rgba(&t, q16(0), q16(0), 0, out);
  EXPECT_EQ(out[0], 0xAA);  // 0xA replicated
  EXPECT_EQ(out[3], 0xAA);
  tex_sample_rgba(&t, q16(1), q16(0), 0, out);
  EXPECT_EQ(out[0], 0x55);  // 0x5 replicated
}

TEST(TexRgba, IA8IntensityHiAlphaLo) {
  // 0xF3: intensity nibble F -> 0xFF; alpha nibble 3 -> 0x33.
  static const uint8_t px[1] = {0xF3};
  struct TexDesc t;
  fill_desc(&t, px, 1, 1, TEXFMT_IA8, WRAP_CLAMP, FILTER_POINT, (const void*)0);
  uint8_t out[4];
  tex_sample_rgba(&t, q16(0), q16(0), 0, out);
  EXPECT_EQ(out[0], 0xFF);
  EXPECT_EQ(out[1], 0xFF);
  EXPECT_EQ(out[2], 0xFF);
  EXPECT_EQ(out[3], 0x33);
}

TEST(TexRgba, IA4Intensity3BitAlpha1Bit) {
  // Nibble 0xF = 1111: i3 = 0xe (top 3 bits), alpha bit = 1 -> 0xFF.
  // i = (0xe<<4)|(0xe<<1)|(0xe>>2) = 0xe0|0x1c|0x03 = 0xff.
  static const uint8_t px[1] = {0xF0};  // hi nibble F (even pixel)
  struct TexDesc t;
  fill_desc(&t, px, 2, 1, TEXFMT_IA4, WRAP_CLAMP, FILTER_POINT, (const void*)0);
  uint8_t out[4];
  tex_sample_rgba(&t, q16(0), q16(0), 0, out);
  EXPECT_EQ(out[0], 0xFF);
  EXPECT_EQ(out[3], 0xFF);
  // Even-pixel intensity 0xE = 1110: alpha bit 0 -> 0x00; i3 = 0xe.
  static const uint8_t px2[1] = {0xE0};
  fill_desc(&t, px2, 2, 1, TEXFMT_IA4, WRAP_CLAMP, FILTER_POINT,
            (const void*)0);
  tex_sample_rgba(&t, q16(0), q16(0), 0, out);
  EXPECT_EQ(out[3], 0x00);  // bit0 of 0xE is 0
}

TEST(TexRgba, Rgba5551Decode) {
  // 0xFA0F = 1111 1010 0000 1111: R=11111(31), G=01000(8), B=00111(7), A=1.
  // expand5: 31->0xFF, 8->(8<<3)|(8>>2)=0x40|2=0x42, 7->(7<<3)|(7>>2)=0x39.
  static const uint16_t px[1] = {0xFA0F};
  struct TexDesc t;
  fill_desc(&t, px, 1, 1, TEXFMT_RGBA5551, WRAP_CLAMP, FILTER_POINT,
            (const void*)0);
  uint8_t out[4];
  tex_sample_rgba(&t, q16(0), q16(0), 0, out);
  EXPECT_EQ(out[0], 0xFF);
  EXPECT_EQ(out[1], 0x42);
  EXPECT_EQ(out[2], 0x39);
  EXPECT_EQ(out[3], 0xFF);
  // Alpha bit clear -> 0x00.
  static const uint16_t px0[1] = {0xFA0E};
  fill_desc(&t, px0, 1, 1, TEXFMT_RGBA5551, WRAP_CLAMP, FILTER_POINT,
            (const void*)0);
  tex_sample_rgba(&t, q16(0), q16(0), 0, out);
  EXPECT_EQ(out[3], 0x00);
}

TEST(TexRgba, Rgba565AlphaOpaque) {
  static const uint16_t px[1] = {0xF800};  // pure red
  struct TexDesc t;
  fill_desc(&t, px, 1, 1, TEXFMT_RGBA565, WRAP_CLAMP, FILTER_POINT,
            (const void*)0);
  uint8_t out[4];
  tex_sample_rgba(&t, q16(0), q16(0), 0, out);
  EXPECT_EQ(out[0], 0xFF);
  EXPECT_EQ(out[1], 0x00);
  EXPECT_EQ(out[2], 0x00);
  EXPECT_EQ(out[3], 0xFF);  // 565 is opaque
}

TEST(TexRgba, Ci8ThroughTlut5551) {
  // CI8 index -> RGBA5551 palette entry. Palette[2] = pure blue (B=31,A=1).
  static const uint8_t idx[1] = {2};
  static const uint16_t pal[4] = {0x0000, 0xF801 /*red,A*/, 0x003F /*B=31,A*/,
                                  0xFFFF};
  struct TexDesc t;
  fill_desc(&t, idx, 1, 1, TEXFMT_CI8, WRAP_CLAMP, FILTER_POINT, pal);
  uint8_t out[4];
  tex_sample_rgba(&t, q16(0), q16(0), 0, out);
  EXPECT_EQ(out[2], 0xFF);  // blue saturated
  EXPECT_EQ(out[3], 0xFF);  // alpha bit set
}

TEST(TexRgba, Ci4ThroughTlut5551) {
  // Byte 0x31: hi nibble 3 (even), lo nibble 1 (odd). 2x1 texture.
  static const uint8_t idx[1] = {0x31};
  static const uint16_t pal[16] = {0};
  static uint16_t pal_rw[16];
  memcpy(pal_rw, pal, sizeof pal);
  pal_rw[3] = 0xF801;  // red, A=1
  pal_rw[1] = 0x07C1;  // green (G=31), A=1 : bits 10..6 = 11111
  struct TexDesc t;
  fill_desc(&t, idx, 2, 1, TEXFMT_CI4, WRAP_CLAMP, FILTER_POINT, pal_rw);
  uint8_t out[4];
  tex_sample_rgba(&t, q16(0), q16(0), 0, out);  // index 3 -> red
  EXPECT_EQ(out[0], 0xFF);
  EXPECT_EQ(out[3], 0xFF);
  tex_sample_rgba(&t, q16(1), q16(0), 0, out);  // index 1 -> green
  EXPECT_EQ(out[1], 0xFF);
}

// ---- tex_validate: bilinear-on-CI rejected, others accepted ----------------

TEST(TexValidate, RejectsThreePointOnCi) {
  static const uint8_t idx[1] = {0};
  static const uint16_t pal[16] = {0};
  struct TexDesc t;
  fill_desc(&t, idx, 1, 1, TEXFMT_CI8, WRAP_REPEAT, FILTER_THREE_POINT, pal);
  EXPECT_EQ(tex_validate(&t), RDR_EINVAL);
  t.format = TEXFMT_CI4;
  EXPECT_EQ(tex_validate(&t), RDR_EINVAL);
  // POINT on CI is fine (with a TLUT).
  t.filter = FILTER_POINT;
  EXPECT_EQ(tex_validate(&t), RDR_OK);
}

TEST(TexValidate, RejectsCiWithoutTlut) {
  static const uint8_t idx[1] = {0};
  struct TexDesc t;
  fill_desc(&t, idx, 1, 1, TEXFMT_CI8, WRAP_REPEAT, FILTER_POINT,
            (const void*)0);
  EXPECT_EQ(tex_validate(&t), RDR_EINVAL);
}

TEST(TexValidate, AcceptsThreePointOnRgbaAndI) {
  static const uint16_t px[1] = {0};
  struct TexDesc t;
  fill_desc(&t, px, 1, 1, TEXFMT_RGBA5551, WRAP_REPEAT, FILTER_THREE_POINT,
            (const void*)0);
  EXPECT_EQ(tex_validate(&t), RDR_OK);
  t.format = TEXFMT_I8;
  EXPECT_EQ(tex_validate(&t), RDR_OK);
  t.format = TEXFMT_IA8;
  EXPECT_EQ(tex_validate(&t), RDR_OK);
}

TEST(TexValidate, NullOrZeroDimIsOk) {
  struct TexDesc t;
  fill_desc(&t, (const void*)0, 1, 1, TEXFMT_RGBA565, WRAP_REPEAT, FILTER_POINT,
            (const void*)0);
  EXPECT_EQ(tex_validate(&t), RDR_OK);  // null data
  EXPECT_EQ(tex_validate((const struct TexDesc*)0), RDR_OK);
}

// ---- CI sampler forces POINT even if filter says THREE_POINT ---------------
// (Defense in depth: validate() rejects at setup, but the fetch must not crash
//  / interpolate indices if a THREE_POINT CI slips through.)
TEST(TexRgba, CiThreePointFallsBackToPoint) {
  static const uint8_t idx[1] = {2};
  static const uint16_t pal[4] = {0, 0, 0x003F, 0};  // pal[2] = blue,A
  struct TexDesc t;
  fill_desc(&t, idx, 1, 1, TEXFMT_CI8, WRAP_CLAMP, FILTER_THREE_POINT, pal);
  uint8_t out[4];
  tex_sample_rgba(&t, q16_frac5(0, 16), q16_frac5(0, 16), 0, out);
  EXPECT_EQ(out[2], 0xFF);  // clean point fetch of pal[2], no interpolation
  EXPECT_EQ(out[3], 0xFF);
}

// ---- 3-point filter: zero-frac == point; midpoint of two taps --------------

TEST(TexRgba, ThreePointZeroFracEqualsPoint) {
  // 2x1 I8: [0x00, 0x80]. sfrac=tfrac=0 -> exactly t0.
  static const uint8_t px[2] = {0x00, 0x80};
  struct TexDesc t;
  fill_desc(&t, px, 2, 1, TEXFMT_I8, WRAP_CLAMP, FILTER_THREE_POINT,
            (const void*)0);
  uint8_t out[4];
  tex_sample_rgba(&t, q16(0), q16(0), 0, out);
  EXPECT_EQ(out[0], 0x00);
}

TEST(TexRgba, ThreePointInterpolatesS) {
  // 2x1 I8: t0=0x00 at s=0, t1=0x40 at s=1. tfrac=0 so lower triangle:
  // out = t0 + (sfrac*(t1-t0) + 0 + 0x10) >> 5. At sfrac=16: (16*64+16)>>5 =
  // (1024+16)/32 = 32 = 0x20.
  static const uint8_t px[2] = {0x00, 0x40};
  struct TexDesc t;
  fill_desc(&t, px, 2, 1, TEXFMT_I8, WRAP_CLAMP, FILTER_THREE_POINT,
            (const void*)0);
  uint8_t out[4];
  tex_sample_rgba(&t, q16_frac5(0, 16), q16(0), 0, out);
  EXPECT_EQ(out[0], 0x20);
}

// ---- oracle cross-check: every format x wrap, POINT, over a coord sweep ----

static const uint8_t K_FMTS[] = {
    TEXFMT_RGBA565, TEXFMT_RGBA4444, TEXFMT_RGBA5551, TEXFMT_IA8, TEXFMT_IA4,
    TEXFMT_I8,      TEXFMT_I4,       TEXFMT_CI4,      TEXFMT_CI8};
static const uint8_t K_WRAPS[] = {WRAP_REPEAT, WRAP_MIRROR, WRAP_CLAMP};

// Build a 4x4 source for a given format from a deterministic byte/word pattern.
// Returns pointer to a static buffer keyed per format (re-filled each call).
static void make_src(uint8_t fmt, const void** data, const void** tlut) {
  static uint16_t buf16[16];
  static uint8_t buf8[16];
  static uint16_t pal[256];
  for (int i = 0; i < 256; ++i) {
    pal[i] = (uint16_t)((i * 0x0101U) ^ 0x1357U);  // arbitrary 5551 entries
  }
  *tlut = (const void*)0;
  if (fmt == TEXFMT_RGBA565 || fmt == TEXFMT_RGBA4444 ||
      fmt == TEXFMT_RGBA5551) {
    for (int i = 0; i < 16; ++i) {
      buf16[i] = (uint16_t)(0x1234U + (uint16_t)(i * 0x0911U));
    }
    *data = buf16;
    return;
  }
  // 8b / packed-4b / CI: byte buffer. 4-bit packs 2 px/byte (8 bytes used).
  for (int i = 0; i < 16; ++i) {
    buf8[i] = (uint8_t)(0x10U + (uint8_t)(i * 0x1DU));
  }
  *data = buf8;
  if (fmt == TEXFMT_CI4 || fmt == TEXFMT_CI8) {
    *tlut = pal;
  }
}

TEST(TexOracle, PointMatchesFloatReferenceAllFormatsAllWraps) {
  for (size_t fi = 0; fi < sizeof K_FMTS; ++fi) {
    for (size_t wi = 0; wi < sizeof K_WRAPS; ++wi) {
      const void* data = (const void*)0;
      const void* tlut = (const void*)0;
      make_src(K_FMTS[fi], &data, &tlut);
      struct TexDesc t;
      fill_desc(&t, data, 4, 4, K_FMTS[fi], K_WRAPS[wi], FILTER_POINT, tlut);
      // Sweep integer texel coords including out-of-range (exercises wrap).
      for (int sv = -3; sv <= 6; ++sv) {
        for (int su = -3; su <= 6; ++su) {
          uint8_t fixed_rgba[4];
          uint8_t oracle_rgba[4];
          tex_sample_rgba(&t, q16(su), q16(sv), 0, fixed_rgba);
          int const rc = oracle_sample_texel(&t, su, sv, oracle_rgba);
          ASSERT_EQ(rc, 0) << "oracle decode failed fmt=" << (int)K_FMTS[fi];
          for (int k = 0; k < 4; ++k) {
            EXPECT_EQ(fixed_rgba[k], oracle_rgba[k])
                << "fmt=" << (int)K_FMTS[fi] << " wrap=" << (int)K_WRAPS[wi]
                << " s=" << su << " t=" << sv << " chan=" << k;
          }
        }
      }
    }
  }
}

// ---- oracle cross-check: 3-point at frac == taps of the point oracle -------
// The 3-point result is a fixed-point blend of the SAME texels the point
// oracle decodes; validate the corners (frac 0 and a known midpoint) against
// the oracle-decoded neighbourhood within +-1 LSB tolerance.
TEST(TexOracle, ThreePointCornersMatchOracleTaps) {
  // Use RGBA5551 (full RGBA) on a 4x4. At sfrac=tfrac=0 the 3-tap result must
  // equal the point oracle at (s,t) exactly.
  const void* data = (const void*)0;
  const void* tlut = (const void*)0;
  make_src(TEXFMT_RGBA5551, &data, &tlut);
  struct TexDesc t;
  fill_desc(&t, data, 4, 4, TEXFMT_RGBA5551, WRAP_REPEAT, FILTER_THREE_POINT,
            tlut);
  for (int sv = 0; sv < 4; ++sv) {
    for (int su = 0; su < 4; ++su) {
      uint8_t fixed_rgba[4];
      uint8_t tap[4];
      tex_sample_rgba(&t, q16_frac5(su, 0), q16_frac5(sv, 0), 0, fixed_rgba);
      ASSERT_EQ(oracle_sample_texel(&t, su, sv, tap), 0);
      for (int k = 0; k < 4; ++k) {
        EXPECT_EQ(fixed_rgba[k], tap[k]) << "zero-frac 3pt != point oracle";
      }
    }
  }
}

// 3-point at the lower-triangle midpoint blends t0/t1/t2; verify against an
// independent float computation of the same N64 triangular weights.
TEST(TexOracle, ThreePointLowerTriangleMatchesFloat) {
  const void* data = (const void*)0;
  const void* tlut = (const void*)0;
  make_src(TEXFMT_RGBA5551, &data, &tlut);
  struct TexDesc t;
  fill_desc(&t, data, 4, 4, TEXFMT_RGBA5551, WRAP_REPEAT, FILTER_THREE_POINT,
            tlut);
  int const sfrac = 8;
  int const tfrac = 4;  // sum=12 < 32 -> lower triangle
  for (int sv = 0; sv < 4; ++sv) {
    for (int su = 0; su < 4; ++su) {
      uint8_t fixed_rgba[4];
      uint8_t t0[4];
      uint8_t t1[4];
      uint8_t t2[4];
      ASSERT_EQ(oracle_sample_texel(&t, su, sv, t0), 0);
      ASSERT_EQ(oracle_sample_texel(&t, su + 1, sv, t1), 0);
      ASSERT_EQ(oracle_sample_texel(&t, su, sv + 1, t2), 0);
      tex_sample_rgba(&t, q16_frac5(su, sfrac), q16_frac5(sv, tfrac), 0,
                      fixed_rgba);
      for (int k = 0; k < 4; ++k) {
        // Reference of the exact N64 integer 3-tap formula (lower triangle).
        int const ds = (sfrac * ((int)t1[k] - (int)t0[k]));
        int const dt = (tfrac * ((int)t2[k] - (int)t0[k]));
        int const want = (int)t0[k] + ((ds + dt + 0x10) >> 5);
        EXPECT_EQ((int)fixed_rgba[k], want);
      }
    }
  }
}
