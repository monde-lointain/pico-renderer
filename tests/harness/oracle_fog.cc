// oracle_fog.cc — float reference for the fog lerp (color = lerp(color,
// fog_color, factor)). Owned by Wave-D stream D.3 (blend/fog). Stub until D.3
// lands the body; returns nonzero ("unsupported") meanwhile.
//
// Harness carve-out: NOT orthodoxy_enforced (see tests/harness/CMakeLists.txt).

#include "oracle.h"

int oracle_fog_lerp(const uint8_t in_rgb[3], const uint8_t fog_rgb[3],
                    float factor, uint8_t out_rgb[3]) {
  (void)in_rgb;
  (void)fog_rgb;
  (void)factor;
  // Stub: write a defined default (the real body lerps in_rgb -> fog_rgb).
  out_rgb[0] = out_rgb[1] = out_rgb[2] = 0;
  return 1;  // ORACLE_TODO(D.3/fog): lerp color toward fog by factor
}
