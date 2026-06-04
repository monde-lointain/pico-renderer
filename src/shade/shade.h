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
  CC_COMBINED = 0,  // previous combiner output. Single-cycle: 0. Cycle 2
                    // (shade_pixel2): the cycle-1 RGB output (feed-forward).
  CC_TEXEL0 = 1,    // sampled base texel (TEXEL0)
  CC_TEXEL1 = 2,    // R.4: 2nd/detail texel (TEXEL1; tex1 sampled in raster)
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

// R.4: number of combiner cycles (RenderState.cycle stores this — referenced by
// the rdr/types.h Δ2 comment). ONE_CYCLE = the pre-R.4 single-combiner path
// (shade_pixel). TWO_CYCLE = run `combiner` then `combiner2` (shade_pixel2,
// CC_COMBINED feed-forward), iff a valid tex1 is present in raster.
enum CombineCycle { COMBINE_ONE_CYCLE = 0, COMBINE_TWO_CYCLE = 1 };

// Run the combiner for one pixel. `texel` and `shade` are packed RGB565; PRIM/
// ENV are read from `st`. Writes the alpha-compare result to *keep (nonzero =
// keep the pixel). v1 has no alpha channel in 565, so *keep is always 1 unless
// alpha-compare logic later rejects. Returns the packed RGB565 result.
// CC_TEXEL1 selects 0 here (single-cycle has no 2nd texel); use shade_pixel2
// for the 2-cycle detail path.
uint16_t shade_pixel(const struct RenderState* st, uint16_t texel,
                     uint16_t shade, uint8_t* keep);

// R.4: 2-cycle (detail multitexture) combiner for one pixel. Cycle 1 runs
// `st->combiner` over {TEXEL0, TEXEL1, SHADE, PRIM, ENV, ZERO, ONE}
// (CC_COMBINED = 0 here, as in single-cycle); cycle 2 runs `st->combiner2`
// where CC_COMBINED selects the cycle-1 RGB output (feed-forward).
// `texel0`/`texel1`/`shade` are packed RGB565; the result is the packed RGB565
// of cycle 2. Same per-channel N64 RDP (A-B)*C+D integer math as shade_pixel
// (clamp(((a-b)*c+(d<<8)+0x80)>>8))
// -> host<->device bit-identical. 565 carries no alpha, so *keep is always 1
// (consistent with shade_pixel). When mode==COMBINE_MODULATE each cycle uses
// the modulate preset (A=a-arm,B=0,C=c-arm,D=0); COMBINE_CUSTOM uses a/b/c/d
// verbatim. T4b FOOTGUN: if combiner2 is left at the zero/default
// (mode==COMBINE_MODULATE), cycle 2 computes TEXEL0*SHADE and IGNORES
// CC_COMBINED — cycle 1's output is silently discarded. The terrain wiring MUST
// set combiner2 explicitly (e.g. combiner2.mode=CUSTOM, a=CC_COMBINED) to carry
// cycle 1 forward.
uint16_t shade_pixel2(const struct RenderState* st, uint16_t texel0,
                      uint16_t texel1, uint16_t shade, uint8_t* keep);

#endif  // RDR_SHADE_H
