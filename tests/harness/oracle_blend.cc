// oracle_blend.cc — float reference for framebuffer blend (coverage/alpha blend
// of src over dst + z compare). Owned by Wave-D stream D.3 (blend). Stub until
// D.3 lands the body; returns nonzero ("unsupported") meanwhile.
//
// Harness carve-out: NOT orthodoxy_enforced (see tests/harness/CMakeLists.txt).

#include "oracle.h"

int oracle_blend(uint8_t zmode, const uint8_t* src_rgba, float coverage,
                 const uint8_t* dst_rgb) {
  (void)zmode;
  (void)src_rgba;
  (void)coverage;
  (void)dst_rgb;
  return 1;  // ORACLE_TODO(D.3/blend): coverage/alpha blend + z compare
}
