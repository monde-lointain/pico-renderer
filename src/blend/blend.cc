// blend.cc — framebuffer blend ops over packed RGB565. See blend.h.
//
// The N64 blender computes p*a + m*b; the opaque rendermode forces a=1, b=0
// (src wins). RGB565 has no destination alpha (design spec section 8b), so only
// a SOURCE-alpha "over" (BLEND_ALPHA) and a clamped additive blend (BLEND_ADD)
// are expressible; the caller supplies the source alpha (8-bit).
//
// XLU-ALPHA NOTE (for R.2/R.3 sequencing): the translucent (XLU) tree pass
// blends on TEXEL alpha, NOT coverage. From the N64 source the tree combiner
// alpha = TEXEL0, so the alpha passed to blend_pixel_alpha is the
// combiner/texel alpha; coverage AA (R.3) is a separate, downstream concern.
// R.2 (two-pass translucent) therefore does NOT depend on R.3 (coverage AA).
// This module just takes a source-alpha argument and stays agnostic to where it
// came from.
//
// FIXED-POINT PARITY: channels are unpacked to 8-bit, blended with integer
// math, then repacked to 565. The alpha-over and additive blends divide an
// 8-bit product sum by 255; both operands are non-negative so the divide
// truncates toward zero == floor -- same floor discipline as raster's num/area2
// (raster.cc), and bit-identical host<->device. NOTE: the +127 is an
// INDEPENDENT round-to-nearest bias applied BEFORE the /255 -- this is NOT what
// num/area2 does (that divide is pure truncation, no bias). Do not "reconcile"
// the two: the +127 is deliberate here and the whole expression is validated
// bit-exact against the float oracle (oracle_blend) across all inputs.
#include "blend/blend.h"

#include "gfx/framebuffer.h"
#include "rdr/sram.h"  // __not_in_flash_func (SRAM placement; no-op on host)

// Unpack RGB565 -> 8-bit channels with bit replication (matches the oracle's
// oracle_unpack565 and the inverse of gfx/framebuffer.h rgb565()).
static void blend_unpack565(uint16_t c, uint8_t* r, uint8_t* g, uint8_t* b) {
  uint8_t const r5 = (uint8_t)((c >> 11) & 0x1F);
  uint8_t const g6 = (uint8_t)((c >> 5) & 0x3F);
  uint8_t const b5 = (uint8_t)(c & 0x1F);
  *r = (uint8_t)((r5 << 3) | (r5 >> 2));
  *g = (uint8_t)((g6 << 2) | (g6 >> 4));
  *b = (uint8_t)((b5 << 3) | (b5 >> 2));
}

// out = round(s*a + d*(255-a) over 255). s,d,a in [0,255]; result in [0,255].
static uint8_t blend_over_channel(uint8_t s, uint8_t d, uint8_t a) {
  uint32_t const num = ((uint32_t)s * a) + ((uint32_t)d * (255U - a)) + 127U;
  return (uint8_t)(num / 255U);  // non-negative -> truncates toward zero
}

// out = clamp255(round(s*a over 255) + d). Additive with source pre-scaled by
// a.
static uint8_t blend_add_channel(uint8_t s, uint8_t d, uint8_t a) {
  uint32_t const scaled =
      (((uint32_t)s * a) + 127U) / 255U;  // round-to-nearest
  uint32_t const sum = scaled + d;
  return (uint8_t)(sum > 255U ? 255U : sum);
}

uint16_t blend_pixel(uint8_t mode, uint16_t src, uint16_t dst) {
  switch (mode) {
    case BLEND_OPAQUE:
    default:
      // Opaque copy (src wins), dst unused. The source-alpha modes need an
      // explicit alpha, so they live in blend_pixel_alpha; any mode reaching
      // here also falls back to opaque (never silently corrupt the FB).
      (void)dst;
      return src;
  }
}

uint16_t __not_in_flash_func(blend_pixel_alpha)(uint8_t mode, uint16_t src,
                                                uint8_t src_alpha,
                                                uint16_t dst) {
  if (mode == BLEND_OPAQUE) {
    (void)dst;
    (void)src_alpha;
    return src;  // src wins, alpha ignored
  }

  uint8_t sr;
  uint8_t sg;
  uint8_t sb;
  uint8_t dr;
  uint8_t dg;
  uint8_t db;
  blend_unpack565(src, &sr, &sg, &sb);
  blend_unpack565(dst, &dr, &dg, &db);

  uint8_t orr;
  uint8_t og;
  uint8_t ob;
  switch (mode) {
    case BLEND_ALPHA:
      orr = blend_over_channel(sr, dr, src_alpha);
      og = blend_over_channel(sg, dg, src_alpha);
      ob = blend_over_channel(sb, db, src_alpha);
      break;
    case BLEND_ADD:
      orr = blend_add_channel(sr, dr, src_alpha);
      og = blend_add_channel(sg, dg, src_alpha);
      ob = blend_add_channel(sb, db, src_alpha);
      break;
    default:
      return src;  // unknown -> opaque fallback
  }
  return rgb565(orr, og, ob);
}

uint16_t __not_in_flash_func(fog_lerp)(uint16_t color, uint16_t fog_color,
                                       uint8_t factor) {
  uint8_t cr;
  uint8_t cg;
  uint8_t cb;
  uint8_t fr;
  uint8_t fg;
  uint8_t fb;
  blend_unpack565(color, &cr, &cg, &cb);
  blend_unpack565(fog_color, &fr, &fg, &fb);
  // lerp toward fog: out = color*(255-factor) + fog*factor, scaled by /255.
  // factor reuses the alpha-over channel op with (s=fog, d=color, a=factor):
  // fog*factor + color*(255-factor) over 255.
  uint8_t const orr = blend_over_channel(fr, cr, factor);
  uint8_t const og = blend_over_channel(fg, cg, factor);
  uint8_t const ob = blend_over_channel(fb, cb, factor);
  return rgb565(orr, og, ob);
}

// L6 PREMULTIPLIED-UNDER helpers ---------------------------------------------
// round(x * y / 255) for x,y in [0,255]: same +127-then-/255 round-to-nearest
// as blend_over_channel's terms (host<->device bit-identical, non-negative ->
// truncation == floor). Result is in [0,255] (x*y max 65025 -> 255 after the
// divide). Used by both accumulate (premultiply by the effective alpha) and
// resolve (scale terrain by the residual transmittance).
static uint8_t mul255_round(uint8_t x, uint8_t y) {
  uint32_t const num = ((uint32_t)x * (uint32_t)y) + 127U;
  return (uint8_t)(num / 255U);  // non-negative -> truncates toward zero
}

// out = clamp255(acc_chan + add_chan). Premultiplied accumulation never
// overshoots in exact arithmetic (the front-to-back UNDER weights sum to <=1),
// but the per-fragment 565 requantization can nudge a channel +1, so clamp.
static uint8_t add_clamp255(uint8_t acc, uint8_t add) {
  uint32_t const sum = (uint32_t)acc + (uint32_t)add;
  return (uint8_t)(sum > 255U ? 255U : sum);
}

void __not_in_flash_func(blend_premul_accumulate)(uint16_t* c_acc,
                                                  uint8_t* acc_alpha,
                                                  uint16_t frag565,
                                                  uint8_t frag_alpha) {
  uint8_t const t = (uint8_t)(255U - (uint32_t)*acc_alpha);  // transmittance
  uint8_t const ea = mul255_round(frag_alpha, t);  // effective contribution
  if (ea == 0U) {
    return;  // no light reaches this pixel through the accumulated alpha
  }
  uint8_t fr;
  uint8_t fg;
  uint8_t fb;
  blend_unpack565(frag565, &fr, &fg, &fb);
  uint8_t ar;
  uint8_t ag;
  uint8_t ab;
  blend_unpack565(*c_acc, &ar, &ag, &ab);
  // Premultiply the straight fragment color by ea, add to the accumulator.
  uint8_t const orr = add_clamp255(ar, mul255_round(fr, ea));
  uint8_t const og = add_clamp255(ag, mul255_round(fg, ea));
  uint8_t const ob = add_clamp255(ab, mul255_round(fb, ea));
  *c_acc = rgb565(orr, og, ob);
  *acc_alpha = add_clamp255(*acc_alpha, ea);
}

uint16_t __not_in_flash_func(blend_premul_resolve)(uint16_t c_acc,
                                                   uint8_t acc_alpha,
                                                   uint16_t terrain565) {
  uint8_t const t = (uint8_t)(255U - (uint32_t)acc_alpha);  // residual transmit
  uint8_t ar;
  uint8_t ag;
  uint8_t ab;
  blend_unpack565(c_acc, &ar, &ag, &ab);
  uint8_t tr;
  uint8_t tg;
  uint8_t tb;
  blend_unpack565(terrain565, &tr, &tg, &tb);
  // out = premultiplied accumulation + terrain scaled by residual
  // transmittance.
  uint8_t const orr = add_clamp255(ar, mul255_round(tr, t));
  uint8_t const og = add_clamp255(ag, mul255_round(tg, t));
  uint8_t const ob = add_clamp255(ab, mul255_round(tb, t));
  return rgb565(orr, og, ob);
}
