// shade_test.cc — golden tests for the color combiner (modulate preset).
// out = (A-B)*C + D, per channel, N64 RDP single-cycle arithmetic
// (angrylion combiner.c color_combiner_equation):
//   out8 = clamp( ((a-b)*c + (d<<8) + 0x80) >> 8 )  with 8-bit operands.
// For MODULATE (A=TEXEL0, B=0, C=SHADE, D=0): out = (texel*shade + 0x80) >> 8.
// texel/shade/result are packed RGB565 (frozen color_t). 565->8bit uses the
// same bit-replication as oracle_unpack565 (the golden 565 layout reference).

#include "shade/shade.h"

#include <stdint.h>

#include "gfx/framebuffer.h"  // rgb565()
#include "gtest/gtest.h"
#include "oracle.h"  // oracle_unpack565 (golden 565 layout)

// ---- independent re-derivation of the expected combiner output -------------
static uint8_t modulate8(uint8_t a, uint8_t c) {
  int32_t out = (((int32_t)a * (int32_t)c) + 0x80) >> 8;  // B=0, D=0
  if (out < 0) {
    out = 0;
  }
  if (out > 255) {
    out = 255;
  }
  return (uint8_t)out;
}

static uint16_t expect_modulate(uint16_t texel, uint16_t shade) {
  uint8_t tr;
  uint8_t tg;
  uint8_t tb;
  uint8_t sr;
  uint8_t sg;
  uint8_t sb;
  oracle_unpack565(texel, &tr, &tg, &tb);
  oracle_unpack565(shade, &sr, &sg, &sb);
  return rgb565(modulate8(tr, sr), modulate8(tg, sg), modulate8(tb, sb));
}

static void state_modulate(struct RenderState* st) {
  st->combiner.mode = COMBINE_MODULATE;
  st->combiner.a = CC_TEXEL0;
  st->combiner.b = CC_ZERO;
  st->combiner.c = CC_SHADE;
  st->combiner.d = CC_ZERO;
  st->prim_color = 0;
  st->env_color = 0;
  st->alpha_cmp = 0;
}

// ---- modulate by white shade: ~identity (255 ~= 0.996, not exact) ----------
TEST(Shade, ModulateByWhiteIsNearIdentity) {
  struct RenderState st;
  state_modulate(&st);
  uint8_t keep = 0;
  uint16_t const texel = 0xABCD;
  EXPECT_EQ(shade_pixel(&st, texel, 0xFFFF, &keep),
            expect_modulate(texel, 0xFFFF));
  EXPECT_EQ(keep, 1);
}

// ---- modulate by black shade: zero -----------------------------------------
TEST(Shade, ModulateByBlackIsZero) {
  struct RenderState st;
  state_modulate(&st);
  uint8_t keep = 0;
  EXPECT_EQ(shade_pixel(&st, 0xFFFF, 0x0000, &keep), 0x0000);
  EXPECT_EQ(keep, 1);
}

// ---- modulate matches the per-channel oracle on several colors -------------
TEST(Shade, ModulateMatchesOracle) {
  struct RenderState st;
  state_modulate(&st);
  uint8_t keep = 0;
  uint16_t const texels[4] = {0xF800, 0x07E0, 0x001F, 0x8410};
  uint16_t const shades[4] = {0x8000, 0xFFFF, 0x07E0, 0x4208};
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      EXPECT_EQ(shade_pixel(&st, texels[i], shades[j], &keep),
                expect_modulate(texels[i], shades[j]))
          << "texel=" << texels[i] << " shade=" << shades[j];
    }
  }
}

// ---- half-intensity shade halves the texel (per-channel) -------------------
TEST(Shade, ModulateHalfIntensity) {
  struct RenderState st;
  state_modulate(&st);
  uint8_t keep = 0;
  // shade gray ~0x8410 -> r5=0x10(->0x84), modulate red texel.
  uint16_t const got = shade_pixel(&st, 0xF800, 0x8410, &keep);
  EXPECT_EQ(got, expect_modulate(0xF800, 0x8410));
}

// ---- null keep pointer must not crash --------------------------------------
TEST(Shade, NullKeepIsSafe) {
  struct RenderState st;
  state_modulate(&st);
  EXPECT_EQ(shade_pixel(&st, 0xF800, 0xFFFF, (uint8_t*)0),
            expect_modulate(0xF800, 0xFFFF));
}
