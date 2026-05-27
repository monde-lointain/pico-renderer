// tex.h — module interface (Stream A contract). Orthodox C++.
// Point-sampling texture fetch. v1 supports power-of-2 RGBA565/RGBA4444 source
// textures with per-axis WRAP/MIRROR/CLAMP (mask-based, N64 RDP convention).
// IA/I/CI formats and the 3-point filter are W-later.
#ifndef RDR_TEX_H
#define RDR_TEX_H
#include "rdr/types.h"

// Sample texture `t` at texel coordinates (u,v) given in Q16.16 texel space
// (integer part = texel index; fractional part discarded by the point filter).
// `lod` selects mip level (v1: must be 0; nonzero falls back to level 0).
// Returns the texel as packed RGB565 (frozen color_t layout). Alpha from 4444
// sources is dropped (the 565 target is opaque); the combiner/blend stages own
// alpha. A null/zero-dim texture returns 0 (transparent black) -- callers gate
// on a valid TexDesc.
uint16_t tex_sample(const struct TexDesc* t, fx16_16 u, fx16_16 v, int lod);

#endif  // RDR_TEX_H
