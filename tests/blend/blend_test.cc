// blend_test.cc — golden tests for the framebuffer blend ops + fog lerp.
// Modes: BLEND_OPAQUE (opaque copy), BLEND_ALPHA (source-alpha over), BLEND_ADD
// (clamped additive), and fog_lerp (lerp toward fog_color). src/dst are packed
// RGB565 (frozen contract: framebuffer color_t). The fixed-point blend is
// validated against the float oracle (oracle_blend / oracle_fog_lerp) within a
// 565-quantization tolerance. Reference: design spec section 8b (blend modes),
// N64 manual blender (force-blend / 1*src + 0*dst), "RGB565 has no dst alpha".

#include "blend/blend.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include "gfx/framebuffer.h"
#include "gtest/gtest.h"
#include "oracle.h"

// ---- helpers ---------------------------------------------------------------
// Unpack a 565 result and the oracle's 8-bit output, compare per channel within
// `tol` (8-bit units). The 565 round-trip quantizes the 5/6-bit channels, so a
// few LSBs of slack is expected vs the pure-8-bit float oracle.
static int chan_abs_diff(uint8_t a, uint8_t b) {
  return a > b ? (int)(a - b) : (int)(b - a);
}

static void expect_close_to_oracle(uint16_t got565, const uint8_t* oracle_rgb,
                                   int tol) {
  uint8_t gr;
  uint8_t gg;
  uint8_t gb;
  oracle_unpack565(got565, &gr, &gg, &gb);
  EXPECT_LE(chan_abs_diff(gr, oracle_rgb[0]), tol)
      << "R got=" << (int)gr << " oracle=" << (int)oracle_rgb[0];
  EXPECT_LE(chan_abs_diff(gg, oracle_rgb[1]), tol)
      << "G got=" << (int)gg << " oracle=" << (int)oracle_rgb[1];
  EXPECT_LE(chan_abs_diff(gb, oracle_rgb[2]), tol)
      << "B got=" << (int)gb << " oracle=" << (int)oracle_rgb[2];
}

// ---- BLEND_OPAQUE (regression: unchanged behavior) -------------------------
// BLEND_OPAQUE must reproduce src exactly regardless of dst (1*src + 0*dst).
TEST(Blend, OpaqueCopiesSrcIgnoringDst) {
  EXPECT_EQ(blend_pixel(BLEND_OPAQUE, 0xF800, 0x07E0), 0xF800);
  EXPECT_EQ(blend_pixel(BLEND_OPAQUE, 0x0000, 0xFFFF), 0x0000);
  EXPECT_EQ(blend_pixel(BLEND_OPAQUE, 0xFFFF, 0x0000), 0xFFFF);
  EXPECT_EQ(blend_pixel(BLEND_OPAQUE, 0x1234, 0xABCD), 0x1234);
}

// Opaque copy is independent of dst across the whole 565 range for a fixed src.
TEST(Blend, OpaqueIsDstIndependent) {
  uint16_t const src = 0x55AA;
  for (uint32_t dst = 0; dst <= 0xFFFF; dst += 0x101) {
    EXPECT_EQ(blend_pixel(BLEND_OPAQUE, src, (uint16_t)dst), src);
  }
}

// blend_pixel_alpha with BLEND_OPAQUE ignores alpha and dst (src wins).
TEST(Blend, AlphaEntryOpaqueIgnoresAlphaAndDst) {
  EXPECT_EQ(blend_pixel_alpha(BLEND_OPAQUE, 0xF800, 0x00, 0x07E0), 0xF800);
  EXPECT_EQ(blend_pixel_alpha(BLEND_OPAQUE, 0x1234, 0x7F, 0xABCD), 0x1234);
}

// ---- BLEND_ALPHA endpoints -------------------------------------------------
// alpha=255 -> fully src; alpha=0 -> fully dst (RGB565 round-trip of each).
TEST(Blend, AlphaEndpoints) {
  uint16_t const src = 0xF81F;  // magenta
  uint16_t const dst = 0x07E0;  // green
  // a=255: result == src (round-tripped through unpack/repack, which is exact).
  EXPECT_EQ(blend_pixel_alpha(BLEND_ALPHA, src, 0xFF, dst), src);
  // a=0: result == dst.
  EXPECT_EQ(blend_pixel_alpha(BLEND_ALPHA, src, 0x00, dst), dst);
}

// Half alpha blends roughly halfway between src and dst (per channel).
TEST(Blend, AlphaHalfIsMidpoint) {
  uint16_t const src = 0xFFFF;  // white
  uint16_t const dst = 0x0000;  // black
  uint16_t const out = blend_pixel_alpha(BLEND_ALPHA, src, 0x80, dst);
  uint8_t r;
  uint8_t g;
  uint8_t b;
  oracle_unpack565(out, &r, &g, &b);
  // 0x80/255 ~ 0.502 -> ~128 each. Allow 565 quantization slack.
  EXPECT_NEAR((int)r, 128, 8);
  EXPECT_NEAR((int)g, 128, 8);
  EXPECT_NEAR((int)b, 128, 8);
}

// ---- BLEND_ADD endpoints ---------------------------------------------------
TEST(Blend, AddSaturatesToWhite) {
  // src+dst (a=255) where both are bright -> clamps to white per channel.
  uint16_t const out =
      blend_pixel_alpha(BLEND_ADD, 0xFFFF, 0xFF, 0xFFFF);  // white+white
  EXPECT_EQ(out, 0xFFFF);
}

TEST(Blend, AddZeroAlphaIsDst) {
  // a=0 -> source contributes nothing; result == dst.
  uint16_t const dst = 0x4A69;
  EXPECT_EQ(blend_pixel_alpha(BLEND_ADD, 0xF800, 0x00, dst), dst);
}

TEST(Blend, AddOntoBlackIsScaledSrc) {
  // dst=black, a=255 -> result == src (round-tripped).
  uint16_t const src = 0x39C7;
  EXPECT_EQ(blend_pixel_alpha(BLEND_ADD, src, 0xFF, 0x0000), src);
}

// Readable mid-alpha additive onto a non-black/non-white dst (the sweep covers
// endpoints; this pins the interior arithmetic). src=white, a=0x80 -> source
// contributes round(255*128/255) = 128 per channel, added onto dst.
TEST(Blend, AddMidAlphaOntoGrayDst) {
  uint16_t const src = 0xFFFF;  // white source
  uint16_t const dst = 0x4208;  // mid-gray-ish (R5=8 G6=16 B5=8)
  uint16_t const out = blend_pixel_alpha(BLEND_ADD, src, 0x80, dst);
  uint8_t dr;
  uint8_t dg;
  uint8_t db;
  oracle_unpack565(dst, &dr, &dg, &db);
  uint8_t orr;
  uint8_t og;
  uint8_t ob;
  oracle_unpack565(out, &orr, &og, &ob);
  // Expected per channel = clamp255(128 + dst); compare within 565 slack.
  int const er = (dr + 128 > 255) ? 255 : (dr + 128);
  int const eg = (dg + 128 > 255) ? 255 : (dg + 128);
  int const eb = (db + 128 > 255) ? 255 : (db + 128);
  EXPECT_NEAR((int)orr, er, 8);
  EXPECT_NEAR((int)og, eg, 8);
  EXPECT_NEAR((int)ob, eb, 8);
  // Sanity: additive made it strictly brighter than dst (none of these clamp).
  EXPECT_GT((int)orr, (int)dr);
  EXPECT_GT((int)og, (int)dg);
  EXPECT_GT((int)ob, (int)db);
}

// ---- fog_lerp endpoints ----------------------------------------------------
TEST(Fog, LerpEndpoints) {
  uint16_t const color = 0xF800;                 // red
  uint16_t const fog = 0x001F;                   // blue
  EXPECT_EQ(fog_lerp(color, fog, 0x00), color);  // factor 0 -> unchanged
  EXPECT_EQ(fog_lerp(color, fog, 0xFF), fog);    // factor 255 -> full fog
}

TEST(Fog, LerpHalfIsMidpoint) {
  uint16_t const color = 0x0000;  // black
  uint16_t const fog = 0xFFFF;    // white
  uint16_t const out = fog_lerp(color, fog, 0x80);
  uint8_t r;
  uint8_t g;
  uint8_t b;
  oracle_unpack565(out, &r, &g, &b);
  EXPECT_NEAR((int)r, 128, 8);
  EXPECT_NEAR((int)g, 128, 8);
  EXPECT_NEAR((int)b, 128, 8);
}

// ---- oracle parity sweeps --------------------------------------------------
// Feed identical 8-bit inputs (unpacked from the same 565 src/dst) to the
// oracle and the fixed-point blend; the fixed result (re-unpacked from 565)
// must track the float oracle within the 565-quantization tolerance.
static const uint16_t K_SAMPLES565[] = {0x0000, 0xFFFF, 0xF800, 0x07E0,
                                        0x001F, 0x1234, 0xABCD, 0x55AA,
                                        0x39C7, 0x7BEF, 0xC618, 0x8410};

TEST(Blend, AlphaMatchesOracleSweep) {
  for (size_t si = 0; si < sizeof(K_SAMPLES565) / sizeof(K_SAMPLES565[0]);
       ++si) {
    for (size_t di = 0; di < sizeof(K_SAMPLES565) / sizeof(K_SAMPLES565[0]);
         ++di) {
      uint16_t const src = K_SAMPLES565[si];
      uint16_t const dst = K_SAMPLES565[di];
      uint8_t sr;
      uint8_t sg;
      uint8_t sb;
      uint8_t dr;
      uint8_t dg;
      uint8_t db;
      oracle_unpack565(src, &sr, &sg, &sb);
      oracle_unpack565(dst, &dr, &dg, &db);
      for (int a = 0; a <= 255; a += 17) {  // 0,17,...,255
        uint8_t const src_rgba[4] = {sr, sg, sb, (uint8_t)a};
        uint8_t const dst_rgb[3] = {dr, dg, db};
        uint8_t oracle_rgb[3];
        ASSERT_EQ(
            oracle_blend(BLEND_ALPHA, src_rgba, 1.0F, dst_rgb, oracle_rgb), 0);
        uint16_t const got =
            blend_pixel_alpha(BLEND_ALPHA, src, (uint8_t)a, dst);
        // Tol 8 (one 5-bit 565 step) absorbs 565 quantization + /255 rounding.
        expect_close_to_oracle(got, oracle_rgb, 8);
      }
    }
  }
}

TEST(Blend, AddMatchesOracleSweep) {
  for (size_t si = 0; si < sizeof(K_SAMPLES565) / sizeof(K_SAMPLES565[0]);
       ++si) {
    for (size_t di = 0; di < sizeof(K_SAMPLES565) / sizeof(K_SAMPLES565[0]);
         ++di) {
      uint16_t const src = K_SAMPLES565[si];
      uint16_t const dst = K_SAMPLES565[di];
      uint8_t sr;
      uint8_t sg;
      uint8_t sb;
      uint8_t dr;
      uint8_t dg;
      uint8_t db;
      oracle_unpack565(src, &sr, &sg, &sb);
      oracle_unpack565(dst, &dr, &dg, &db);
      for (int a = 0; a <= 255; a += 17) {
        uint8_t const src_rgba[4] = {sr, sg, sb, (uint8_t)a};
        uint8_t const dst_rgb[3] = {dr, dg, db};
        uint8_t oracle_rgb[3];
        ASSERT_EQ(oracle_blend(BLEND_ADD, src_rgba, 1.0F, dst_rgb, oracle_rgb),
                  0);
        uint16_t const got = blend_pixel_alpha(BLEND_ADD, src, (uint8_t)a, dst);
        expect_close_to_oracle(got, oracle_rgb, 8);
      }
    }
  }
}

TEST(Fog, LerpMatchesOracleSweep) {
  for (size_t si = 0; si < sizeof(K_SAMPLES565) / sizeof(K_SAMPLES565[0]);
       ++si) {
    for (size_t fi = 0; fi < sizeof(K_SAMPLES565) / sizeof(K_SAMPLES565[0]);
         ++fi) {
      uint16_t const color = K_SAMPLES565[si];
      uint16_t const fog = K_SAMPLES565[fi];
      uint8_t cr;
      uint8_t cg;
      uint8_t cb;
      uint8_t fr;
      uint8_t fg;
      uint8_t fb;
      oracle_unpack565(color, &cr, &cg, &cb);
      oracle_unpack565(fog, &fr, &fg, &fb);
      for (int f = 0; f <= 255; f += 17) {
        uint8_t const in_rgb[3] = {cr, cg, cb};
        uint8_t const fog_rgb[3] = {fr, fg, fb};
        uint8_t oracle_rgb[3];
        ASSERT_EQ(
            oracle_fog_lerp(in_rgb, fog_rgb, (float)f / 255.0F, oracle_rgb), 0);
        uint16_t const got = fog_lerp(color, fog, (uint8_t)f);
        expect_close_to_oracle(got, oracle_rgb, 8);
      }
    }
  }
}

// ---- oracle coverage-multiply branch ---------------------------------------
// The oracle's effective source alpha = texel_alpha * coverage. Every sweep
// above passes coverage=1.0, leaving the multiply untested. Here coverage<1
// must scale a fully-opaque texel: oracle_blend(ALPHA, a=255, cov=0.5) must
// equal oracle_blend(ALPHA, a=128, cov=1.0) -- i.e. coverage folds into alpha.
TEST(Oracle, BlendCoverageScalesSourceAlpha) {
  uint8_t const dst_rgb[3] = {30, 90, 200};
  // Opaque texel (a=255) at half coverage.
  uint8_t const src_full[4] = {255, 0, 0, 255};
  uint8_t cov_half[3];
  ASSERT_EQ(oracle_blend(BLEND_ALPHA, src_full, 0.5F, dst_rgb, cov_half), 0);

  // Same source color at ~half alpha (0.5*255 = 127.5 -> 128), full coverage.
  uint8_t const src_half[4] = {255, 0, 0, 128};
  uint8_t full_cov[3];
  ASSERT_EQ(oracle_blend(BLEND_ALPHA, src_half, 1.0F, dst_rgb, full_cov), 0);

  // Both compute src*0.5 + dst*0.5 (within float-rounding of the 127.5 split).
  for (int i = 0; i < 3; ++i) {
    EXPECT_NEAR((int)cov_half[i], (int)full_cov[i], 1)
        << "channel " << i << " cov_half=" << (int)cov_half[i]
        << " full_cov=" << (int)full_cov[i];
  }

  // And both sit roughly midway between src and dst (not a no-op / not opaque).
  // Midpoints precomputed as ints (no division inside EXPECT_NEAR's float ctx).
  int const mid_r = 142;  // (255 + 30) / 2
  int const mid_g = 45;   // (0 + 90) / 2
  int const mid_b = 100;  // (0 + 200) / 2
  EXPECT_NEAR((int)cov_half[0], mid_r, 2);
  EXPECT_NEAR((int)cov_half[1], mid_g, 2);
  EXPECT_NEAR((int)cov_half[2], mid_b, 2);
}

// coverage=0 fully erases the source contribution (effective alpha 0 -> dst).
TEST(Oracle, BlendZeroCoverageIsDst) {
  uint8_t const dst_rgb[3] = {12, 34, 56};
  uint8_t const src[4] = {200, 100, 50, 255};  // opaque texel
  uint8_t out[3];
  ASSERT_EQ(oracle_blend(BLEND_ALPHA, src, 0.0F, dst_rgb, out), 0);
  EXPECT_EQ((int)out[0], 12);
  EXPECT_EQ((int)out[1], 34);
  EXPECT_EQ((int)out[2], 56);
}

// ---- L6 front-to-back premultiplied UNDER ----------------------------------
// Independent guard for blend_premul_accumulate / blend_premul_resolve (the XLU
// overdraw lever). The rasterizer wiring tests in tests/raster drive these SAME
// helpers as their own reference, so they cannot catch a bug in the helper math
// itself. Here the helpers are checked against THREE independent references:
//   (a) a closed-form FLOAT front-to-back-UNDER oracle (C+=c*ea; A+=ea;
//       ea=a*(1-A); out=C+(1-A)*terrain) — within a 565/8-bit requant tol;
//   (b) the OLD, separately-tested back-to-front blend_pixel_alpha over-blend
//       (a DIFFERENT code path), which front-to-back-UNDER provably equals
//       modulo 565 requant: near-over-(far-over-terrain);
//   (c) exact integer invariants (saturation, opaque-hides-terrain, identity).

// Drive the integer helper chain over `n` front-to-back fragments + terrain.
static uint16_t premul_chain(const uint16_t* frag565, const uint8_t* frag_a,
                             int n, uint16_t terrain565) {
  uint16_t c_acc = 0;
  uint8_t a_acc = 0;
  for (int i = 0; i < n; ++i) {
    blend_premul_accumulate(&c_acc, &a_acc, frag565[i], frag_a[i]);
  }
  return (a_acc != 0) ? blend_premul_resolve(c_acc, a_acc, terrain565)
                      : terrain565;
}

// Closed-form FLOAT oracle over the SAME 565-rounded fragment colors (so the
// only divergence from the integer chain is per-step 565 requant + 8-bit alpha
// rounding). Writes the expected 8-bit RGB into out_rgb.
static void premul_chain_oracle(const uint16_t* frag565, const uint8_t* frag_a,
                                int n, uint16_t terrain565, uint8_t* out_rgb) {
  float cr = 0.0F;
  float cg = 0.0F;
  float cb = 0.0F;
  float a_acc = 0.0F;
  for (int i = 0; i < n; ++i) {
    uint8_t fr;
    uint8_t fg;
    uint8_t fb;
    oracle_unpack565(frag565[i], &fr, &fg, &fb);
    float const a = (float)frag_a[i] / 255.0F;
    float const ea = a * (1.0F - a_acc);
    cr += (float)fr * ea;
    cg += (float)fg * ea;
    cb += (float)fb * ea;
    a_acc += ea;
  }
  uint8_t tr;
  uint8_t tg;
  uint8_t tb;
  oracle_unpack565(terrain565, &tr, &tg, &tb);
  float const t = 1.0F - a_acc;
  out_rgb[0] = (uint8_t)lroundf(cr + (t * (float)tr));
  out_rgb[1] = (uint8_t)lroundf(cg + (t * (float)tg));
  out_rgb[2] = (uint8_t)lroundf(cb + (t * (float)tb));
}

// (a) Sweep many 1-, 2-, and 3-layer stacks over varied terrain; the integer
// chain must track the float oracle within the 565-requant tolerance. A wrong
// operator (e.g. dropping the (1-A) attenuation, or over-blending instead of
// accumulating premultiplied) diverges by tens-to-hundreds, far beyond tol.
TEST(BlendPremul, FrontToBackTracksFloatOracle) {
  uint16_t const colors[6] = {0xF800, 0x07E0, 0x001F, 0xFFFF, 0x8410, 0xFD20};
  uint8_t const alphas[5] = {32, 96, 136, 200, 255};
  uint16_t const terrains[3] = {0x0000, 0x4208, 0xFFFF};
  int checked = 0;
  for (int ti = 0; ti < 3; ++ti) {
    for (int n = 1; n <= 3; ++n) {
      for (int c0 = 0; c0 < 6; ++c0) {
        for (int a0 = 0; a0 < 5; ++a0) {
          uint16_t frag565[3];
          uint8_t frag_a[3];
          for (int k = 0; k < n; ++k) {
            frag565[k] = colors[(c0 + k) % 6];
            frag_a[k] = alphas[(a0 + k) % 5];
          }
          uint16_t const got = premul_chain(frag565, frag_a, n, terrains[ti]);
          uint8_t oracle_rgb[3];
          premul_chain_oracle(frag565, frag_a, n, terrains[ti], oracle_rgb);
          // tol scales with layer count: each accumulate requantizes c_acc to
          // 565 (<=1 R/B quantum ~8). 3 layers + resolve -> <=~20 worst case.
          expect_close_to_oracle(got, oracle_rgb, 8 * n);
          ++checked;
        }
      }
    }
  }
  EXPECT_GT(checked, 200) << "premul sweep is vacuous";
}

// (b) Two layers: front-to-back-UNDER must equal the OLD back-to-front
// blend_pixel_alpha chain near-over-(far-over-terrain) (a separate code path),
// within 565 requant. This is what makes the L6 golden rebake a faithful
// reorder, not a behavior change.
TEST(BlendPremul, EqualsBackToFrontOverBlendTwoLayers) {
  uint16_t const near565 = 0xF800;  // red
  uint16_t const far565 = 0x001F;   // blue
  uint16_t const terrain = 0x07E0;  // green
  for (int an = 16; an <= 255; an += 48) {
    for (int af = 16; af <= 255; af += 48) {
      // OLD back-to-front: far over terrain, then near over that.
      uint16_t bf =
          blend_pixel_alpha(BLEND_ALPHA, far565, (uint8_t)af, terrain);
      bf = blend_pixel_alpha(BLEND_ALPHA, near565, (uint8_t)an, bf);
      // NEW front-to-back: accumulate near, then far, then fold terrain under.
      uint16_t const frag565[2] = {near565, far565};
      uint8_t const frag_a[2] = {(uint8_t)an, (uint8_t)af};
      uint16_t const got = premul_chain(frag565, frag_a, 2, terrain);
      uint8_t bf_rgb[3];
      oracle_unpack565(bf, &bf_rgb[0], &bf_rgb[1], &bf_rgb[2]);
      expect_close_to_oracle(got, bf_rgb, 16);
    }
  }
}

// (c1) acc_alpha follows the closed-form 1-prod(1-ai) within 1 LSB (exact-ish
// integer accumulation, independent of any 565 color path).
TEST(BlendPremul, AccumulatedAlphaMatchesFloat) {
  uint8_t const seq[4] = {40, 90, 150, 30};
  uint16_t c_acc = 0;
  uint8_t a_acc = 0;
  float trans = 1.0F;  // product of (1-ai)
  for (int i = 0; i < 4; ++i) {
    blend_premul_accumulate(&c_acc, &a_acc, 0xFFFF, seq[i]);
    trans *= (1.0F - ((float)seq[i] / 255.0F));
    int const expect = (int)lroundf((1.0F - trans) * 255.0F);
    EXPECT_LE(chan_abs_diff(a_acc, (uint8_t)expect), 1)
        << "after layer " << i << " acc_alpha=" << (int)a_acc << " expect~"
        << expect;
  }
}

// (c2) Once saturated (acc_alpha==255), further fragments are a no-op (the
// early-out is mathematically free at RASTER_XLU_SAT=255).
TEST(BlendPremul, SaturationIsNoOp) {
  uint16_t c_acc = 0;
  uint8_t a_acc = 0;
  blend_premul_accumulate(&c_acc, &a_acc, 0xF800, 255);  // opaque red -> sat
  EXPECT_EQ((int)a_acc, 255);
  uint16_t const c_sat = c_acc;
  blend_premul_accumulate(&c_acc, &a_acc, 0x07E0, 200);  // farther green
  EXPECT_EQ((int)a_acc, 255) << "alpha grew past saturation";
  EXPECT_EQ((int)c_acc, (int)c_sat) << "color changed after saturation";
}

// (c3) A single opaque (a=255) fragment fully hides the terrain: resolve == the
// fragment color exactly (no requant — mul255_round(c,255)==c, residual t==0).
TEST(BlendPremul, OpaqueFragmentHidesTerrain) {
  uint16_t const frag = 0xABCD;
  uint16_t c_acc = 0;
  uint8_t a_acc = 0;
  blend_premul_accumulate(&c_acc, &a_acc, frag, 255);
  EXPECT_EQ((int)blend_premul_resolve(c_acc, a_acc, 0x0000), (int)frag);
  EXPECT_EQ((int)blend_premul_resolve(c_acc, a_acc, 0xFFFF), (int)frag);
}

// (c4) Empty accumulation resolves to the terrain exactly (identity): the
// composite pass relies on this for acc_alpha==0 pixels staying byte-identical.
TEST(BlendPremul, EmptyResolveIsTerrain) {
  for (uint32_t t = 0; t <= 0xFFFF; t += 0x111) {
    EXPECT_EQ((int)blend_premul_resolve(0, 0, (uint16_t)t), (int)t);
  }
}
