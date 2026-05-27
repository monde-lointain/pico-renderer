// tex.cc — point-sampling texture fetch. See tex.h.
// Mask-based WRAP/MIRROR/CLAMP follows the N64 RDP convention (angrylion
// tcoord.c): for a power-of-2 texture, the low log2(dim) bits index the texel;
// MIRROR toggles via the bit just above the mask; CLAMP saturates to [0,dim-1].
// Source RGBA565 passes through; RGBA4444 is nibble-replicated to 8-bit and
// repacked to RGB565 (the opaque 565 target drops alpha).
#include "tex/tex.h"

#include "gfx/framebuffer.h"  // rgb565()

// log2 of a power-of-2 dimension (mask = dim-1). dim must be >= 1.
static int log2_pow2(uint16_t dim) {
  int bits = 0;
  while ((1u << bits) < dim) {
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

// Decode one source texel (already index-selected) to packed RGB565.
static uint16_t decode_texel(const struct TexDesc* t, int idx) {
  switch (t->format) {
    case TEXFMT_RGBA4444: {
      const uint16_t* src = (const uint16_t*)t->data;
      uint16_t const p = src[idx];
      uint8_t const r = expand4((uint8_t)((p >> 12) & 0xF));
      uint8_t const g = expand4((uint8_t)((p >> 8) & 0xF));
      uint8_t const b = expand4((uint8_t)((p >> 4) & 0xF));
      return rgb565(r, g, b);
    }
    case TEXFMT_RGBA565:
    default: {
      const uint16_t* src = (const uint16_t*)t->data;
      return src[idx];  // already 565
    }
  }
}

uint16_t tex_sample(const struct TexDesc* t, fx16_16 u, fx16_16 v, int lod) {
  (void)lod;  // v1: single mip level
  if (t == (const struct TexDesc*)0 || t->data == (const void*)0 || t->w == 0 ||
      t->h == 0) {
    return 0;
  }
  // Point filter: integer part of the Q16.16 coordinate (arithmetic floor).
  int const su = wrap_coord((int)(u >> 16), t->w, t->wrap_s);
  int const tv = wrap_coord((int)(v >> 16), t->h, t->wrap_t);
  int const idx = (tv * (int)t->w) + su;
  return decode_texel(t, idx);
}
