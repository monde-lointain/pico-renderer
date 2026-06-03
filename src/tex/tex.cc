// tex.cc — fixed-point texture fetch. See tex.h.
//
// Mask-based WRAP/MIRROR/CLAMP follows the N64 RDP convention (angrylion
// tcoord.c): for a power-of-2 texture, the low log2(dim) bits index the texel;
// MIRROR toggles via the bit just above the mask; CLAMP saturates to [0,dim-1].
//
// Per-format decode to RGBA8 (verified against angrylion tmem.c):
//   RGBA565   — 5/6/5 with bit replication, alpha opaque.
//   RGBA4444  — each nibble replicated to 8 bits.
//   RGBA5551  — 5/5/5 top-3-bit replication (i<<3|i>>2); alpha = bit0 -> ff/00.
//   I8/I4     — intensity replicated into r=g=b=a.
//   IA8       — intensity = hi nibble (replicated); alpha = lo nibble.
//   IA4       — intensity = top 3 bits (replicated); alpha = bit0 -> ff/00.
//   CI4/CI8   — index into a 16-bit RGBA5551 TLUT.
//
// THREE_POINT is the N64 3-tap triangular bilerp (NOT a 2x2 box): 5-bit S/T
// fractions select the lower or upper diagonal triangle of the 2x2 texel
// neighbourhood and blend three taps. CI formats are POINT-only (indices do
// not interpolate); tex_validate() rejects THREE_POINT on CI.
//
// Fixed-point discipline: all sampler/coord math is integer. The only multiply
// is a frac weight by a channel diff (-255..255). The weight is sfrac/tfrac
// (0..31) in the lower triangle, or invsf/invtf = 0x20-frac (1..32) in the
// upper triangle: worst case 32 * 255 = 8160, so the product fits comfortably
// in signed 32-bit and a single 32x32->32 MULS suffices (no UMULL; ARMv6-M).
#include "tex/tex.h"

#include "gfx/framebuffer.h"  // rgb565()

// log2 of a power-of-2 dimension (mask = dim-1). dim must be >= 1.
static int log2_pow2(uint16_t dim) {
  int bits = 0;
  while ((1U << bits) < dim) {
    ++bits;
  }
  return bits;
}

// Map a (floored) texel coordinate to an in-range index per wrap mode.
static int wrap_coord(int coord, uint16_t dim, uint8_t mode) {
  int const max = (int)dim - 1;
  switch (mode) {
    case WRAP_CLAMP:
      if (coord < 0) {
        return 0;
      }
      if (coord > max) {
        return max;
      }
      return coord;
    case WRAP_MIRROR: {
      int const bits = log2_pow2(dim);
      int const period = (int)dim;
      // Reduce into [0, 2*dim) accounting for negatives, then reflect the
      // upper half (the bit above the mask selects the mirror, RDP-style).
      int phase = coord & ((period << 1) - 1);
      if (phase < 0) {
        phase += (period << 1);
      }
      int const mirror = (phase >> bits) & 1;
      int idx = phase & (period - 1);
      if (mirror) {
        idx = max - idx;
      }
      return idx;
    }
    case WRAP_REPEAT:
    default:
      return coord & ((int)dim - 1);  // power-of-2 mask wrap
  }
}

// Expand a 4-bit channel to 8 bits by nibble replication.
static uint8_t expand4(uint8_t nibble) {
  return (uint8_t)((nibble << 4) | nibble);
}

// Expand a 5-bit channel to 8 bits (top-3-bit replication; angrylion table).
static uint8_t expand5(uint8_t v5) {
  return (uint8_t)(((v5 & 0x1f) << 3) | ((v5 & 0x1f) >> 2));
}

// Decode a 16-bit RGBA5551 word (also the TLUT-entry decode) to RGBA8.
static void decode_5551(uint16_t p, uint8_t out_rgba[4]) {
  out_rgba[0] = expand5((uint8_t)((p >> 11) & 0x1f));
  out_rgba[1] = expand5((uint8_t)((p >> 6) & 0x1f));
  out_rgba[2] = expand5((uint8_t)((p >> 1) & 0x1f));
  out_rgba[3] = (uint8_t)((p & 0x1) ? 0xff : 0x00);
}

// Decode a 16-bit RGBA565 word to RGBA8 (alpha opaque).
static void decode_565_rgba(uint16_t p, uint8_t out_rgba[4]) {
  uint8_t const r5 = (uint8_t)((p >> 11) & 0x1f);
  uint8_t const g6 = (uint8_t)((p >> 5) & 0x3f);
  uint8_t const b5 = (uint8_t)(p & 0x1f);
  out_rgba[0] = (uint8_t)((r5 << 3) | (r5 >> 2));
  out_rgba[1] = (uint8_t)((g6 << 2) | (g6 >> 4));
  out_rgba[2] = (uint8_t)((b5 << 3) | (b5 >> 2));
  out_rgba[3] = 0xff;
}

// Decode a 16-bit RGBA4444 word to RGBA8 (each nibble replicated).
static void decode_4444(uint16_t p, uint8_t out_rgba[4]) {
  out_rgba[0] = expand4((uint8_t)((p >> 12) & 0xf));
  out_rgba[1] = expand4((uint8_t)((p >> 8) & 0xf));
  out_rgba[2] = expand4((uint8_t)((p >> 4) & 0xf));
  out_rgba[3] = expand4((uint8_t)(p & 0xf));
}

// Decode one texel at the (already wrapped) integer coords (su,sv) to RGBA8.
// su is needed beyond the linear index to select the nibble for 4-bit formats.
static void decode_texel_rgba(const struct TexDesc* t, int su, int sv,
                              uint8_t out_rgba[4]) {
  int const lin = (sv * (int)t->w) + su;
  switch (t->format) {
    case TEXFMT_RGBA4444: {
      const uint16_t* src = (const uint16_t*)t->data;
      decode_4444(src[lin], out_rgba);
      return;
    }
    case TEXFMT_RGBA5551: {
      const uint16_t* src = (const uint16_t*)t->data;
      decode_5551(src[lin], out_rgba);
      return;
    }
    case TEXFMT_I8: {
      const uint8_t* src = (const uint8_t*)t->data;
      uint8_t const c = src[lin];
      out_rgba[0] = out_rgba[1] = out_rgba[2] = out_rgba[3] = c;
      return;
    }
    case TEXFMT_I4: {
      const uint8_t* src = (const uint8_t*)t->data;
      uint8_t const byte = src[lin >> 1];
      uint8_t const nib = (uint8_t)((su & 1) ? (byte & 0xf) : (byte >> 4));
      uint8_t const c = expand4(nib);
      out_rgba[0] = out_rgba[1] = out_rgba[2] = out_rgba[3] = c;
      return;
    }
    case TEXFMT_IA8: {
      const uint8_t* src = (const uint8_t*)t->data;
      uint8_t const p = src[lin];
      uint8_t const i = (uint8_t)((p & 0xf0) | (p >> 4));
      out_rgba[0] = out_rgba[1] = out_rgba[2] = i;
      out_rgba[3] = expand4((uint8_t)(p & 0xf));
      return;
    }
    case TEXFMT_IA4: {
      const uint8_t* src = (const uint8_t*)t->data;
      uint8_t const byte = src[lin >> 1];
      uint8_t const p = (uint8_t)((su & 1) ? (byte & 0xf) : (byte >> 4));
      uint8_t const i3 = (uint8_t)(p & 0xe);
      uint8_t const i = (uint8_t)((i3 << 4) | (i3 << 1) | (i3 >> 2));
      out_rgba[0] = out_rgba[1] = out_rgba[2] = i;
      out_rgba[3] = (uint8_t)((p & 0x1) ? 0xff : 0x00);
      return;
    }
    case TEXFMT_CI8: {
      const uint8_t* src = (const uint8_t*)t->data;
      const uint16_t* pal = (const uint16_t*)t->tlut;
      decode_5551(pal[src[lin]], out_rgba);
      return;
    }
    case TEXFMT_CI4: {
      const uint8_t* src = (const uint8_t*)t->data;
      const uint16_t* pal = (const uint16_t*)t->tlut;
      uint8_t const byte = src[lin >> 1];
      uint8_t const nib = (uint8_t)((su & 1) ? (byte & 0xf) : (byte >> 4));
      decode_5551(pal[nib], out_rgba);
      return;
    }
    case TEXFMT_RGBA565:
    default: {
      const uint16_t* src = (const uint16_t*)t->data;
      decode_565_rgba(src[lin], out_rgba);
      return;
    }
  }
}

// Point-sample: wrap the floored coords, decode one texel to RGBA8.
static void point_rgba(const struct TexDesc* t, int s, int tt,
                       uint8_t out_rgba[4]) {
  int const su = wrap_coord(s, t->w, t->wrap_s);
  int const sv = wrap_coord(tt, t->h, t->wrap_t);
  decode_texel_rgba(t, su, sv, out_rgba);
}

// N64 3-tap triangular bilerp on the 2x2 neighbourhood of (s,t). sfrac/tfrac
// are 5-bit (0..31, where 0x20 == 1.0). The (sfrac+tfrac)&0x20 bit splits the
// texel into a lower triangle (t0,t1,t2) and an upper triangle (t3,t2,t1);
// each blends three taps. Matches angrylion tex.c texture_pipeline_cycle.
static void three_point_rgba(const struct TexDesc* t, int s, int tt, int sfrac,
                             int tfrac, uint8_t out_rgba[4]) {
  uint8_t t0[4];
  uint8_t t1[4];
  uint8_t t2[4];
  uint8_t t3[4];
  point_rgba(t, s, tt, t0);          // (s,   t)
  point_rgba(t, s + 1, tt, t1);      // (s+1, t)
  point_rgba(t, s, tt + 1, t2);      // (s,   t+1)
  point_rgba(t, s + 1, tt + 1, t3);  // (s+1, t+1)

  int const upper = (sfrac + tfrac) & 0x20;
  if (upper) {
    int const invsf = 0x20 - sfrac;
    int const invtf = 0x20 - tfrac;
    for (int k = 0; k < 4; ++k) {
      int const ds = (invsf * ((int)t2[k] - (int)t3[k]));
      int const dt = (invtf * ((int)t1[k] - (int)t3[k]));
      out_rgba[k] = (uint8_t)((int)t3[k] + ((ds + dt + 0x10) >> 5));
    }
  } else {
    for (int k = 0; k < 4; ++k) {
      int const ds = (sfrac * ((int)t1[k] - (int)t0[k]));
      int const dt = (tfrac * ((int)t2[k] - (int)t0[k]));
      out_rgba[k] = (uint8_t)((int)t0[k] + ((ds + dt + 0x10) >> 5));
    }
  }
}

void tex_sample_rgba(const struct TexDesc* t, fx16_16 u, fx16_16 v, int lod,
                     uint8_t out_rgba[4]) {
  (void)lod;  // v1: single mip level
  out_rgba[0] = out_rgba[1] = out_rgba[2] = out_rgba[3] = 0;
  if (t == (const struct TexDesc*)0 || t->data == (const void*)0 || t->w == 0 ||
      t->h == 0) {
    return;
  }
  // Floor (arithmetic shift) the Q16.16 coordinate to the base texel.
  int const s = (int)(u >> 16);
  int const tt = (int)(v >> 16);
  // CI indices never interpolate -> force point regardless of t->filter.
  int const is_ci = (t->format == TEXFMT_CI4 || t->format == TEXFMT_CI8);
  if (t->filter == FILTER_THREE_POINT && !is_ci) {
    // 5-bit fraction from the Q16.16 low word (bits 15..11 = top 5 of frac).
    int const sfrac = (int)((u >> 11) & 0x1f);
    int const tfrac = (int)((v >> 11) & 0x1f);
    three_point_rgba(t, s, tt, sfrac, tfrac, out_rgba);
    return;
  }
  point_rgba(t, s, tt, out_rgba);
}

uint16_t tex_sample(const struct TexDesc* t, fx16_16 u, fx16_16 v, int lod) {
  if (t == (const struct TexDesc*)0 || t->data == (const void*)0 || t->w == 0 ||
      t->h == 0) {
    return 0;
  }
  uint8_t rgba[4];
  tex_sample_rgba(t, u, v, lod, rgba);
  return rgb565(rgba[0], rgba[1], rgba[2]);  // 565 target drops alpha
}

int tex_validate(const struct TexDesc* t) {
  if (t == (const struct TexDesc*)0 || t->data == (const void*)0 || t->w == 0 ||
      t->h == 0) {
    return RDR_OK;  // "no texture" is a legal render state; fetch gates on it.
  }
  switch (t->format) {
    case TEXFMT_RGBA565:
    case TEXFMT_RGBA4444:
    case TEXFMT_RGBA5551:
    case TEXFMT_IA8:
    case TEXFMT_IA4:
    case TEXFMT_I8:
    case TEXFMT_I4:
      return RDR_OK;
    case TEXFMT_CI4:
    case TEXFMT_CI8:
      // Palette indices do not interpolate; bilinear-on-CI is rejected.
      if (t->filter == FILTER_THREE_POINT) {
        return RDR_EINVAL;
      }
      if (t->tlut == (const void*)0) {
        return RDR_EINVAL;  // CI without a palette is unsampleable.
      }
      return RDR_OK;
    default:
      return RDR_EINVAL;  // unknown format
  }
}
