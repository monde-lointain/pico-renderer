// oracle_shade.cc — float reference for the color combiner ((A-B)*C+D over
// 8-bit inputs). Owned by Wave-D R.4 (shade). The fixed-point shade.cc combiner
// (shade_pixel 1-cycle AND shade_pixel2 2-cycle) is validated against THIS
// within tolerance + the integer self-check.
//
// REFERENCE (verified, not assumed): N64 RDP color combiner is, per channel,
//   out = (A - B) * C + D  with the operands normalized to [0,1] (angrylion
//   src/core/n64video/rdp/rdp.c color_combiner_equation; gbi.h:444 G_CCMUX_*).
// Each of A,B,C,D is one of the mux inputs (TEXEL0/TEXEL1/SHADE/PRIM/ENV/
// COMBINED/ONE/ZERO). The FLOAT reference here computes (A-B)*C+D per channel
// on the ALREADY-RESOLVED 8-bit operands; the test harness owns input-mux
// resolution and the cycle-1->cycle-2 feed-forward (CC_COMBINED), so this
// single signature serves both the 1-cycle and 2-cycle paths. The fixed-point
// shade.cc rounds /256 (clamp(((a-b)*c + (d<<8) + 0x80) >> 8)); this float
// version stays exact and the test tolerance absorbs the integer rounding.
//
// Harness carve-out: NOT orthodoxy_enforced (see tests/harness/CMakeLists.txt).
// FLOAT REFERENCE: this TU is allowed floats freely; it is the oracle.

#include <math.h>

#include "oracle.h"

// One channel of (A-B)*C+D in float, operands in [0,1], result clamped [0,1]
// then back to 8-bit (round-to-nearest, via lroundf — bugprone-incorrect-
// roundings forbids the (int)(x+0.5) idiom).
static uint8_t combine_chan_f(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  float const fa = (float)a / 255.0F;
  float const fb = (float)b / 255.0F;
  float const fc = (float)c / 255.0F;
  float const fd = (float)d / 255.0F;
  float out = ((fa - fb) * fc) + fd;
  if (out < 0.0F) {
    out = 0.0F;
  }
  if (out > 1.0F) {
    out = 1.0F;
  }
  return (uint8_t)lroundf(out * 255.0F);
}

int oracle_shade_combiner(const struct CombinerState* cs, const uint8_t* a,
                          const uint8_t* b, const uint8_t* c, const uint8_t* d,
                          uint8_t out[4]) {
  if (a == 0 || b == 0 || c == 0 || d == 0 || out == 0) {
    return 1;
  }
  (void)cs;  // mux resolution is the caller's job; this evaluates (A-B)*C+D on
             // the already-selected RGBA operands (R,G,B,A channels).
  for (int ch = 0; ch < 4; ++ch) {
    out[ch] = combine_chan_f(a[ch], b[ch], c[ch], d[ch]);
  }
  return 0;
}
