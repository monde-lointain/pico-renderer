// tex.h — module interface (Stream A contract; Wave-D D.2 extension).
// Orthodox C++.
//
// Texture fetch for every N64-native source format:
//   RGBA565, RGBA4444, RGBA5551, IA8, IA4, I8, I4, CI4, CI8 (+ TLUT palette).
// Each format decodes to RGBA8. Two filters:
//   - POINT       (default): floor the Q16.16 coordinate, fetch one texel.
//   - THREE_POINT (N64 3-tap triangular bilerp): blend three of the 2x2 texel
//     neighbourhood by 5-bit S/T fractions (NOT a 2x2 box filter — see tex.cc).
//
// POW2 mask-based WRAP/MIRROR/CLAMP (N64 RDP convention; pow2 is a hard design
// driver — S0: non-pow2 `%` costs +47 cyc/px).
//
// CI/palette formats are POINT-SAMPLED ONLY (indices do not interpolate);
// tex_validate() rejects THREE_POINT on CI4/CI8. TLUT entries are RGBA5551.
#ifndef RDR_TEX_H
#define RDR_TEX_H
#include "rdr/types.h"

// Sample texture `t` at texel coordinates (u,v) given in Q16.16 texel space
// (integer part = texel index; fractional part discarded by the point filter).
// `lod` selects mip level (v1: must be 0; nonzero falls back to level 0).
// Returns the texel as packed RGB565 (frozen color_t layout). Alpha is dropped
// (the 565 target is opaque; the combiner/blend stages own alpha). A null/zero-
// dim texture returns 0 (transparent black) -- callers gate on a valid TexDesc.
// Honors t->filter: POINT or THREE_POINT (RGBA/I/IA only; CI falls back to
// point regardless -- callers should tex_validate() at material setup).
uint16_t tex_sample(const struct TexDesc* t, fx16_16 u, fx16_16 v, int lod);

// Sample texture `t` at (u,v) in Q16.16 texel space, writing the full RGBA8
// result to out_rgba (R,G,B,A). Honors t->filter (POINT / THREE_POINT) and the
// per-axis wrap modes. A null/zero-dim texture writes transparent black
// (0,0,0,0). out_rgba is an OUTPUT (written by the callee). `lod` as above.
void tex_sample_rgba(const struct TexDesc* t, fx16_16 u, fx16_16 v, int lod,
                     uint8_t out_rgba[4]);

// Validate a TexDesc for sampling at material/sample setup. Returns RDR_OK if
// the descriptor is sampleable as configured, else an errno-like code:
//   RDR_EINVAL      — THREE_POINT requested on a CI4/CI8 format (palette
//                     indices do not interpolate), an unknown format, a CI
//                     format with a null TLUT, OR a non-power-of-2 dimension on
//                     an axis whose wrap mode masks (WRAP_REPEAT/WRAP_MIRROR):
//                     the sampler mask-wraps (coord & (dim-1)) / reflects via
//                     log2_pow2(dim), which only behaves for pow2 dims, and a
//                     non-pow2 fallback would cost +47 cyc/px on-device (S0).
//                     A WRAP_CLAMP axis saturates without a mask and is EXEMPT
//                     (any dim is accepted on a CLAMP-only axis); the check is
//                     per-axis (wrap_s vs w, wrap_t vs h).
// A null/zero-dim TexDesc is RDR_OK (callers gate the actual fetch on validity;
// "no texture" is a legal render state).
int tex_validate(const struct TexDesc* t);

#endif  // RDR_TEX_H
