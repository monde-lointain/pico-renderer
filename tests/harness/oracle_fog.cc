// oracle_fog.cc — float reference for the fog lerp (color = lerp(color,
// fog_color, factor)). Owned by Wave-D stream D.3 (blend/fog).
//
// out = in_rgb*(1-factor) + fog_rgb*factor, per 8-bit channel, with `factor` in
// [0,1] (0 = no fog, 1 = full fog_color). The depth->factor derivation
// (D1/Q9 z-space) lives in the caller that derives `factor` from depth; this
// reference takes the factor directly. Result -> out_rgb[3] (8-bit).
//
// Harness carve-out: NOT orthodoxy_enforced (see tests/harness/CMakeLists.txt).

#include <math.h>

#include "oracle.h"

int oracle_fog_lerp(const uint8_t in_rgb[3], const uint8_t fog_rgb[3],
                    float factor, uint8_t out_rgb[3]) {
  if (in_rgb == 0 || fog_rgb == 0) {
    return 1;  // bad argument
  }
  float f = factor;
  if (f < 0.0F) {
    f = 0.0F;
  }
  if (f > 1.0F) {
    f = 1.0F;
  }
  for (int i = 0; i < 3; ++i) {
    float const out = ((float)in_rgb[i] * (1.0F - f)) + ((float)fog_rgb[i] * f);
    long r = lrintf(out);
    if (r < 0) {
      r = 0;
    }
    if (r > 255) {
      r = 255;
    }
    out_rgb[i] = (uint8_t)r;
  }
  return 0;
}
