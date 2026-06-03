// oracle_texel.cc — float reference for texture sampling (per-format decode +
// wrap + POINT sample). Owned by Wave-D stream D.2 (tex). The fixed-point
// tex.cc sampler is validated against THIS within tolerance.
//
// References (verified, not assumed):
//   - angrylion-rdp-plus src/core/n64video/rdp/tmem.c — per-format texel decode
//     (RGBA16=5551 hi/med/low replication; IA8/IA4; I8/I4; CI index → TLUT).
//   - gbi.h GPACK_RGBA5551 — R[15:11] G[10:6] B[5:1] A[0].
//   - gfx/framebuffer.h rgb565 — R[15:11] G[10:5] B[4:0].
// 5-bit channel expansion uses the N64 replicated table value
// (i<<3)|(i>>2) (tmem.c:2063); 4-bit uses nibble replication (i<<4)|i.
//
// Harness carve-out: NOT orthodoxy_enforced (see tests/harness/CMakeLists.txt).
// FLOAT REFERENCE: this TU is allowed floats freely; it is the oracle.

#include "oracle.h"

// 5-bit (0..31) channel -> 8-bit, top-3-bit replication (angrylion table).
static int expand5(int v5) { return ((v5 & 0x1f) << 3) | ((v5 & 0x1f) >> 2); }

// 4-bit (0..15) channel -> 8-bit, nibble replication.
static int expand4(int v4) { return ((v4 & 0xf) << 4) | (v4 & 0xf); }

// Decode one RGBA5551 16-bit word to RGBA8 (also the TLUT entry decode).
static void decode_5551(uint16_t p, uint8_t out_rgba[4]) {
  out_rgba[0] = (uint8_t)expand5((p >> 11) & 0x1f);  // R
  out_rgba[1] = (uint8_t)expand5((p >> 6) & 0x1f);   // G
  out_rgba[2] = (uint8_t)expand5((p >> 1) & 0x1f);   // B
  out_rgba[3] = (uint8_t)((p & 0x1) ? 0xff : 0x00);  // A (1 bit)
}

// Decode one RGBA565 16-bit word to RGBA8 (alpha opaque).
static void decode_565(uint16_t p, uint8_t out_rgba[4]) {
  int const r5 = (p >> 11) & 0x1f;
  int const g6 = (p >> 5) & 0x3f;
  int const b5 = p & 0x1f;
  out_rgba[0] = (uint8_t)((r5 << 3) | (r5 >> 2));
  out_rgba[1] = (uint8_t)((g6 << 2) | (g6 >> 4));
  out_rgba[2] = (uint8_t)((b5 << 3) | (b5 >> 2));
  out_rgba[3] = 0xff;
}

// Decode one RGBA4444 16-bit word to RGBA8 (each nibble replicated).
static void decode_4444(uint16_t p, uint8_t out_rgba[4]) {
  out_rgba[0] = (uint8_t)expand4((p >> 12) & 0xf);  // R
  out_rgba[1] = (uint8_t)expand4((p >> 8) & 0xf);   // G
  out_rgba[2] = (uint8_t)expand4((p >> 4) & 0xf);   // B
  out_rgba[3] = (uint8_t)expand4(p & 0xf);          // A
}

// Map a texel coord to an in-range index per wrap mode (pow2 mask convention,
// mirrors tex.cc). dim must be a power of two for WRAP/MIRROR.
static int wrap_idx(int coord, int dim, int mode) {
  int const max = dim - 1;
  if (mode == WRAP_CLAMP) {
    if (coord < 0) {
      return 0;
    }
    if (coord > max) {
      return max;
    }
    return coord;
  }
  if (mode == WRAP_MIRROR) {
    int bits = 0;
    while ((1 << bits) < dim) {
      ++bits;
    }
    int phase = coord & ((dim << 1) - 1);
    if (phase < 0) {
      phase += (dim << 1);
    }
    int const mirror = (phase >> bits) & 1;
    int idx = phase & (dim - 1);
    if (mirror) {
      idx = max - idx;
    }
    return idx;
  }
  return coord & (dim - 1);  // WRAP_REPEAT
}

// Fetch + decode the texel at the (already wrapped) row-major index (su,sv).
static int decode_at(const struct TexDesc* tex, int su, int sv,
                     uint8_t out_rgba[4]) {
  int const w = (int)tex->w;
  int const lin = (sv * w) + su;
  switch (tex->format) {
    case TEXFMT_RGBA565: {
      const uint16_t* src = (const uint16_t*)tex->data;
      decode_565(src[lin], out_rgba);
      return 0;
    }
    case TEXFMT_RGBA4444: {
      const uint16_t* src = (const uint16_t*)tex->data;
      decode_4444(src[lin], out_rgba);
      return 0;
    }
    case TEXFMT_RGBA5551: {
      const uint16_t* src = (const uint16_t*)tex->data;
      decode_5551(src[lin], out_rgba);
      return 0;
    }
    case TEXFMT_I8: {
      const uint8_t* src = (const uint8_t*)tex->data;
      int const c = src[lin];
      out_rgba[0] = out_rgba[1] = out_rgba[2] = out_rgba[3] = (uint8_t)c;
      return 0;
    }
    case TEXFMT_I4: {
      const uint8_t* src = (const uint8_t*)tex->data;
      uint8_t const byte = src[lin >> 1];
      int const nib = (su & 1) ? (byte & 0xf) : (byte >> 4);
      int const c = expand4(nib);
      out_rgba[0] = out_rgba[1] = out_rgba[2] = out_rgba[3] = (uint8_t)c;
      return 0;
    }
    case TEXFMT_IA8: {
      const uint8_t* src = (const uint8_t*)tex->data;
      uint8_t const p = src[lin];
      int const i = (p & 0xf0) | (p >> 4);
      int const a = expand4(p & 0xf);
      out_rgba[0] = out_rgba[1] = out_rgba[2] = (uint8_t)i;
      out_rgba[3] = (uint8_t)a;
      return 0;
    }
    case TEXFMT_IA4: {
      const uint8_t* src = (const uint8_t*)tex->data;
      uint8_t const byte = src[lin >> 1];
      int const p = (su & 1) ? (byte & 0xf) : (byte >> 4);
      // Intensity = top 3 bits replicated; alpha = low bit (angrylion IA4).
      int const i3 = p & 0xe;
      int const i = (i3 << 4) | (i3 << 1) | (i3 >> 2);
      out_rgba[0] = out_rgba[1] = out_rgba[2] = (uint8_t)i;
      out_rgba[3] = (uint8_t)((p & 0x1) ? 0xff : 0x00);
      return 0;
    }
    case TEXFMT_CI8: {
      if (tex->tlut == (const void*)0) {
        return 1;
      }
      const uint8_t* src = (const uint8_t*)tex->data;
      const uint16_t* pal = (const uint16_t*)tex->tlut;
      decode_5551(pal[src[lin]], out_rgba);
      return 0;
    }
    case TEXFMT_CI4: {
      if (tex->tlut == (const void*)0) {
        return 1;
      }
      const uint8_t* src = (const uint8_t*)tex->data;
      const uint16_t* pal = (const uint16_t*)tex->tlut;
      uint8_t const byte = src[lin >> 1];
      int const nib = (su & 1) ? (byte & 0xf) : (byte >> 4);
      decode_5551(pal[nib], out_rgba);
      return 0;
    }
    default:
      return 1;  // unknown format
  }
}

int oracle_sample_texel(const struct TexDesc* tex, int s, int t,
                        uint8_t out_rgba[4]) {
  out_rgba[0] = out_rgba[1] = out_rgba[2] = out_rgba[3] = 0;
  if (tex == (const struct TexDesc*)0 || tex->data == (const void*)0 ||
      tex->w == 0 || tex->h == 0) {
    return 1;
  }
  int const su = wrap_idx(s, (int)tex->w, tex->wrap_s);
  int const sv = wrap_idx(t, (int)tex->h, tex->wrap_t);
  return decode_at(tex, su, sv, out_rgba);
}
