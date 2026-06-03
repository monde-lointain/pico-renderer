// oracle_texel.cc — float reference for texture sampling (per-format decode +
// wrap + point/3-point filter). Owned by Wave-D stream D.2 (tex). Stub until
// D.2 lands the body; returns nonzero ("unsupported") meanwhile.
//
// Harness carve-out: NOT orthodoxy_enforced (see tests/harness/CMakeLists.txt).

#include "oracle.h"

int oracle_sample_texel(const struct TexDesc* tex, int s, int t,
                        uint8_t out_rgba[4]) {
  (void)tex;
  (void)s;
  (void)t;
  // Stub: write a defined default (real body = per-format decode + filter).
  out_rgba[0] = out_rgba[1] = out_rgba[2] = out_rgba[3] = 0;
  return 1;  // ORACLE_TODO(D.2/tex): per-format decode + wrap + filter
}
