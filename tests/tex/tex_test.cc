// tex_test.cc — golden tests for the point-sampling texture fetch.
// Reference: N64 RDP texture coordinate handling (angrylion tcoord.c) for
// mask-based WRAP/MIRROR/CLAMP; gbi.h format enums; oracle_unpack565 for the
// RGB565 layout. Test textures are tiny and hand-verifiable.

#include "tex/tex.h"

#include <stdint.h>

#include "gfx/framebuffer.h"  // rgb565() — golden 565 packer
#include "gtest/gtest.h"

// ---- helpers ---------------------------------------------------------------
static fx16_16 q16(int texel) { return (fx16_16)texel << 16; }

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
