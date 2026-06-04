// shade_test.cc — golden tests for the color combiner (modulate preset).
// out = (A-B)*C + D, per channel, N64 RDP single-cycle arithmetic
// (angrylion combiner.c color_combiner_equation):
//   out8 = clamp( ((a-b)*c + (d<<8) + 0x80) >> 8 )  with 8-bit operands.
// For MODULATE (A=TEXEL0, B=0, C=SHADE, D=0): out = (texel*shade + 0x80) >> 8.
// texel/shade/result are packed RGB565 (frozen color_t). 565->8bit uses the
// same bit-replication as oracle_unpack565 (the golden 565 layout reference).

#include "shade/shade.h"

#include <stdint.h>
#include <string.h>

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

// ===========================================================================
// R.4 — 2-cycle detail multitexture combiner (shade_pixel2).
// Cycle 1 runs `combiner` over {TEXEL0,TEXEL1,SHADE,PRIM,ENV,ZERO,ONE} (with
// CC_COMBINED=0); cycle 2 runs `combiner2`, where CC_COMBINED feeds the cycle-1
// RGB output forward. 565 carries no alpha -> keep is always 1.
// ===========================================================================

// One channel of the N64 RDP combiner, BIT-EXACT to shade.cc combine_chan:
// clamp(((a-b)*c + (d<<8) + 0x80) >> 8). The 2-cycle self-check uses this.
static uint8_t combine_chan_ref(int32_t a, int32_t b, int32_t c, int32_t d) {
  int32_t out = ((a - b) * c) + (d << 8) + 0x80;
  out >>= 8;
  if (out < 0) {
    out = 0;
  }
  if (out > 255) {
    out = 255;
  }
  return (uint8_t)out;
}

// Resolve a combiner mux id to its 8-bit RGB for the reference. `comb` is the
// cycle-1 output RGB used by CC_COMBINED in cycle 2 (pass {0,0,0} for cycle 1).
static void select_ref(uint8_t id, const struct RenderState* st,
                       const uint8_t* t0, const uint8_t* t1, const uint8_t* sh,
                       const uint8_t* comb, uint8_t* r, uint8_t* g,
                       uint8_t* b) {
  switch (id) {
    case CC_COMBINED:
      *r = comb[0];
      *g = comb[1];
      *b = comb[2];
      return;
    case CC_TEXEL0:
      *r = t0[0];
      *g = t0[1];
      *b = t0[2];
      return;
    case CC_TEXEL1:
      *r = t1[0];
      *g = t1[1];
      *b = t1[2];
      return;
    case CC_SHADE:
      *r = sh[0];
      *g = sh[1];
      *b = sh[2];
      return;
    case CC_PRIMITIVE:
      oracle_unpack565(st->prim_color, r, g, b);
      return;
    case CC_ENVIRONMENT:
      oracle_unpack565(st->env_color, r, g, b);
      return;
    case CC_ONE:
      *r = *g = *b = 255;
      return;
    case CC_ZERO:
    default:
      *r = *g = *b = 0;
      return;
  }
}

// Run ONE combiner cycle (integer self-check); writes the RGB result. `comb` is
// the cycle-1 RGB (CC_COMBINED source); pass {0,0,0} in cycle 1.
static void cycle_ref(const struct CombinerState* cs,
                      const struct RenderState* st, const uint8_t* t0,
                      const uint8_t* t1, const uint8_t* sh, const uint8_t* comb,
                      uint8_t out[3]) {
  uint8_t ia = CC_TEXEL0;
  uint8_t ib = CC_ZERO;
  uint8_t ic = CC_SHADE;
  uint8_t idd = CC_ZERO;
  if (cs->mode == COMBINE_CUSTOM) {
    ia = cs->a;
    ib = cs->b;
    ic = cs->c;
    idd = cs->d;
  }
  uint8_t a[3];
  uint8_t b[3];
  uint8_t c[3];
  uint8_t d[3];
  select_ref(ia, st, t0, t1, sh, comb, &a[0], &a[1], &a[2]);
  select_ref(ib, st, t0, t1, sh, comb, &b[0], &b[1], &b[2]);
  select_ref(ic, st, t0, t1, sh, comb, &c[0], &c[1], &c[2]);
  select_ref(idd, st, t0, t1, sh, comb, &d[0], &d[1], &d[2]);
  for (int ch = 0; ch < 3; ++ch) {
    out[ch] = combine_chan_ref(a[ch], b[ch], c[ch], d[ch]);
  }
}

// Full 2-cycle integer reference (bit-exact wiring check for shade_pixel2).
static uint16_t expect_two_cycle(const struct RenderState* st, uint16_t texel0,
                                 uint16_t texel1, uint16_t shade) {
  uint8_t t0[3];
  uint8_t t1[3];
  uint8_t sh[3];
  oracle_unpack565(texel0, &t0[0], &t0[1], &t0[2]);
  oracle_unpack565(texel1, &t1[0], &t1[1], &t1[2]);
  oracle_unpack565(shade, &sh[0], &sh[1], &sh[2]);
  uint8_t const zero[3] = {0, 0, 0};
  uint8_t cyc1[3];
  cycle_ref(&st->combiner, st, t0, t1, sh, zero, cyc1);
  uint8_t cyc2[3];
  cycle_ref(&st->combiner2, st, t0, t1, sh, cyc1, cyc2);
  return rgb565(cyc2[0], cyc2[1], cyc2[2]);
}

// A 2-cycle render state: cycle1 = TEXEL0 * TEXEL1, cycle2 = COMBINED * ENV.
static void state_two_cycle(struct RenderState* st) {
  memset(st, 0, sizeof(*st));
  st->cycle = COMBINE_TWO_CYCLE;
  st->combiner.mode = COMBINE_CUSTOM;
  st->combiner.a = CC_TEXEL0;
  st->combiner.b = CC_ZERO;
  st->combiner.c = CC_TEXEL1;
  st->combiner.d = CC_ZERO;
  st->combiner2.mode = COMBINE_CUSTOM;
  st->combiner2.a = CC_COMBINED;
  st->combiner2.b = CC_ZERO;
  st->combiner2.c = CC_ENVIRONMENT;
  st->combiner2.d = CC_ZERO;
  st->prim_color = 0;
  st->env_color = 0xFFFF;  // white ENV -> cycle2 ~= cycle1 (near identity)
  st->alpha_cmp = 0;
}

// ---- 2-cycle (TEXEL0*TEXEL1)->(COMBINED*ENV) matches the integer self-check
// --
TEST(Shade2, TwoCycleModulateBitExact) {
  struct RenderState st;
  state_two_cycle(&st);
  uint8_t keep = 0;
  uint16_t const t0[4] = {0xF800, 0x07E0, 0xFFFF, 0x8410};
  uint16_t const t1[4] = {0x8000, 0xFFFF, 0x4208, 0x07E0};
  uint16_t const sh[4] = {0xFFFF, 0x8410, 0xF800, 0x001F};
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      uint16_t const got = shade_pixel2(&st, t0[i], t1[j], sh[i], &keep);
      EXPECT_EQ(got, expect_two_cycle(&st, t0[i], t1[j], sh[i]))
          << "t0=" << t0[i] << " t1=" << t1[j];
      EXPECT_EQ(keep, 1);
    }
  }
}

// ---- CC_COMBINED feed-forward: cycle2 sees cycle1's output, not 0 -----------
// cycle1 = TEXEL0*ONE (= TEXEL0), cycle2 = COMBINED*ONE (= cycle1). White-ENV
// not used. So the 2-cycle output must equal a plain TEXEL0 pass-through; if
// the impl fed 0 into CC_COMBINED (single-cycle behavior) cycle2 would be 0.
TEST(Shade2, CombinedFeedForwardNotZero) {
  struct RenderState st;
  memset(&st, 0, sizeof(st));
  st.cycle = COMBINE_TWO_CYCLE;
  st.combiner.mode = COMBINE_CUSTOM;
  st.combiner.a = CC_TEXEL0;
  st.combiner.b = CC_ZERO;
  st.combiner.c = CC_ONE;
  st.combiner.d = CC_ZERO;
  st.combiner2.mode = COMBINE_CUSTOM;
  st.combiner2.a = CC_COMBINED;
  st.combiner2.b = CC_ZERO;
  st.combiner2.c = CC_ONE;
  st.combiner2.d = CC_ZERO;
  uint8_t keep = 0;
  uint16_t const texel = 0xABCD;
  uint16_t const got = shade_pixel2(&st, texel, 0x1234, 0x5678, &keep);
  // (texel*255+0x80)>>8 per channel twice == near-identity; assert nonzero AND
  // equal to the bit-exact reference (CC_COMBINED carried cycle1, not 0).
  EXPECT_NE(got, 0x0000);
  EXPECT_EQ(got, expect_two_cycle(&st, texel, 0x1234, 0x5678));
}

// ---- CC_TEXEL1 is a distinct input from TEXEL0 ------------------------------
// cycle1 = TEXEL1 * ONE (= TEXEL1), cycle2 = COMBINED * ONE. Output tracks
// texel1, NOT texel0 -> proves CC_TEXEL1 selects the 2nd texel.
TEST(Shade2, Texel1IsSelectedDistinctly) {
  struct RenderState st;
  memset(&st, 0, sizeof(st));
  st.cycle = COMBINE_TWO_CYCLE;
  st.combiner.mode = COMBINE_CUSTOM;
  st.combiner.a = CC_TEXEL1;
  st.combiner.b = CC_ZERO;
  st.combiner.c = CC_ONE;
  st.combiner.d = CC_ZERO;
  st.combiner2.mode = COMBINE_CUSTOM;
  st.combiner2.a = CC_COMBINED;
  st.combiner2.b = CC_ZERO;
  st.combiner2.c = CC_ONE;
  st.combiner2.d = CC_ZERO;
  uint8_t keep = 0;
  uint16_t const got_t1lo = shade_pixel2(&st, 0xFFFF, 0x0000, 0xFFFF, &keep);
  uint16_t const got_t1hi = shade_pixel2(&st, 0x0000, 0xFFFF, 0x0000, &keep);
  EXPECT_EQ(got_t1lo, 0x0000) << "texel1=0 -> cycle1=0 -> output 0";
  EXPECT_NE(got_t1hi, 0x0000) << "texel1=white -> output nonzero";
  EXPECT_EQ(got_t1hi, expect_two_cycle(&st, 0x0000, 0xFFFF, 0x0000));
}

// ---- a degenerate 2-cycle (cycle2 = COMBINED*ONE) reduces to cycle1 ---------
// matches the float oracle within tolerance for cycle1 = TEXEL0 * SHADE.
TEST(Shade2, TwoCycleMatchesFloatOracleWithinTolerance) {
  struct RenderState st;
  memset(&st, 0, sizeof(st));
  st.cycle = COMBINE_TWO_CYCLE;
  st.combiner.mode = COMBINE_MODULATE;  // TEXEL0 * SHADE
  st.combiner2.mode = COMBINE_CUSTOM;   // COMBINED * ONE (pass-through)
  st.combiner2.a = CC_COMBINED;
  st.combiner2.b = CC_ZERO;
  st.combiner2.c = CC_ONE;
  st.combiner2.d = CC_ZERO;
  uint8_t keep = 0;
  uint16_t const texels[4] = {0xF800, 0x07E0, 0x001F, 0x8410};
  uint16_t const shades[4] = {0x8000, 0xFFFF, 0x07E0, 0x4208};
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      uint16_t const got =
          shade_pixel2(&st, texels[i], 0x0000, shades[j], &keep);
      // Float oracle: cycle1 = (texel*shade), cycle2 = combined*1 = combined.
      uint8_t t8[3];
      uint8_t s8[3];
      oracle_unpack565(texels[i], &t8[0], &t8[1], &t8[2]);
      oracle_unpack565(shades[j], &s8[0], &s8[1], &s8[2]);
      uint8_t got8[3];
      oracle_unpack565(got, &got8[0], &got8[1], &got8[2]);
      for (int ch = 0; ch < 3; ++ch) {
        uint8_t a4[4] = {t8[ch], t8[ch], t8[ch], t8[ch]};
        uint8_t b4[4] = {0, 0, 0, 0};
        uint8_t c4[4] = {s8[ch], s8[ch], s8[ch], s8[ch]};
        uint8_t d4[4] = {0, 0, 0, 0};
        uint8_t cyc1f[4];
        ASSERT_EQ(oracle_shade_combiner(&st.combiner, a4, b4, c4, d4, cyc1f),
                  0);
        // cycle2 = combined * 1; combined is cyc1f[ch].
        uint8_t a2[4] = {cyc1f[ch], cyc1f[ch], cyc1f[ch], cyc1f[ch]};
        uint8_t c2[4] = {255, 255, 255, 255};
        uint8_t cyc2f[4];
        ASSERT_EQ(oracle_shade_combiner(&st.combiner2, a2, b4, c2, d4, cyc2f),
                  0);
        // 565 quantization of the expected float channel for an
        // apples-to-apples compare (got8 is already 565-quantized then
        // bit-replicated).
        int const ref = (int)cyc2f[ch];
        int const diff = (int)got8[ch] - ref;
        EXPECT_LE(diff < 0 ? -diff : diff, 10)
            << "t=" << texels[i] << " s=" << shades[j] << " ch=" << ch;
      }
    }
  }
}

// ---- a 1-cycle render state still routes shade_pixel (unchanged) ------------
// shade_pixel2 is NOT used by the 1-cycle path; this guards that shade_pixel
// itself is untouched by R.4.
TEST(Shade2, OneCycleShadePixelUnchanged) {
  struct RenderState st;
  state_modulate(&st);
  st.cycle = COMBINE_ONE_CYCLE;
  uint8_t keep = 0;
  uint16_t const texel = 0xABCD;
  EXPECT_EQ(shade_pixel(&st, texel, 0xFFFF, &keep),
            expect_modulate(texel, 0xFFFF));
}
