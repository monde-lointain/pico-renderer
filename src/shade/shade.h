// shade.h — module interface (Stream A contract). Orthodox C++.
// Fixed-function pixel combiner: out = (A-B)*C + D evaluated per channel on the
// selected inputs (N64 RDP color combiner). v1 ships the MODULATE preset
// (TEXEL0 * SHADE) and the general single-cycle mux over the inputs we support;
// fog lerp and CI/IA paths are W-later. texel/shade/result are packed RGB565
// (the opaque 565 target). PRIM/ENV come from RenderState (also 565).
#ifndef RDR_SHADE_H
#define RDR_SHADE_H
#include "rdr/types.h"

// Combiner input mux ids (subset of N64 G_CCMUX_*, gbi.h:444). The frozen
// CombinerState.{a,b,c,d} carry these ids; named here, not a cross-module type.
enum CombineInput {
  CC_COMBINED = 0,  // previous combiner output (single-cycle: 0)
  CC_TEXEL0 = 1,    // sampled texel
  CC_PRIMITIVE = 3,
  CC_SHADE = 4,        // interpolated vertex (Gouraud) color
  CC_ENVIRONMENT = 5,  // ENV register
  CC_ONE = 6,          // constant 1.0 (255)
  CC_ZERO = 31         // constant 0.0
};

// Combiner presets (CombinerState.mode). v1: MODULATE only.
enum CombineMode {
  COMBINE_MODULATE = 0,  // TEXEL0 * SHADE  (A=TEXEL0,B=0,C=SHADE,D=0)
  COMBINE_CUSTOM = 1     // use a/b/c/d ids verbatim
};

// Run the combiner for one pixel. `texel` and `shade` are packed RGB565; PRIM/
// ENV are read from `st`. Writes the alpha-compare result to *keep (nonzero =
// keep the pixel). v1 has no alpha channel in 565, so *keep is always 1 unless
// alpha-compare logic later rejects. Returns the packed RGB565 result.
uint16_t shade_pixel(const struct RenderState* st, uint16_t texel,
                     uint16_t shade, uint8_t* keep);

#endif  // RDR_SHADE_H
