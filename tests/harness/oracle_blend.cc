// oracle_blend.cc — float reference for framebuffer blend (coverage/alpha blend
// of src over dst + z compare). Owned by Wave-D stream D.3 (blend). Stub until
// D.3 lands the body; returns nonzero ("unsupported") meanwhile.
//
// Harness carve-out: NOT orthodoxy_enforced (see tests/harness/CMakeLists.txt).

#include "oracle.h"

int oracle_blend(uint8_t zmode, const uint8_t* src_rgba, float coverage,
                 const uint8_t* dst_rgb, uint8_t out_rgb[3]) {
  (void)zmode;
  (void)src_rgba;
  (void)coverage;
  (void)dst_rgb;
  // Stub: write a defined default (real body = coverage/alpha blend over dst).
  out_rgb[0] = out_rgb[1] = out_rgb[2] = 0;
  return 1;  // ORACLE_TODO(D.3/blend): coverage/alpha blend + z compare
}
