// oracle_coverage.cc — float reference for analytic per-pixel coverage from the
// three signed, gradient-normalized edge distances. Owned by Wave-D R.3-AA
// (coverage). Stub until the body lands; returns nonzero ("unsupported")
// meanwhile.
//
// Harness carve-out: NOT orthodoxy_enforced (see tests/harness/CMakeLists.txt).

#include "oracle.h"

int oracle_coverage(float d0, float d1, float d2, float* out_cov) {
  (void)d0;
  (void)d1;
  (void)d2;
  // Stub: write a defined default (the real body = clamp(0.5+min(d0,d1,d2))).
  *out_cov = 0.0F;
  return 1;  // ORACLE_TODO(R.3-AA/cov): clamp(0.5 + min(d0,d1,d2)) in [0,1]
}
