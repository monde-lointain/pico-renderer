// oracle_shade.cc — float reference for the color combiner ((A-B)*C+D over
// 8-bit inputs) + Gouraud interpolation. Owned by Wave-D R.4 (shade). Stub
// until the body lands; returns nonzero ("unsupported") meanwhile.
//
// Harness carve-out: NOT orthodoxy_enforced (see tests/harness/CMakeLists.txt).

#include "oracle.h"

int oracle_shade_combiner(const struct CombinerState* cs, const uint8_t* a,
                          const uint8_t* b, const uint8_t* c, const uint8_t* d,
                          const uint8_t out[4]) {
  (void)cs;
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  (void)out;
  return 1;  // ORACLE_TODO(R.4/shade): Gouraud interp + (A-B)*C+D
}
