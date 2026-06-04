// oracle_coverage.cc — float reference for analytic per-pixel coverage from the
// three signed, gradient-normalized edge distances. Owned by Wave-D R.3-AA
// (coverage).
//
// The RDP-adapted analytic coverage of a pixel by a triangle is, per edge i,
// the SIGNED perpendicular distance d_i (in pixel units) of the pixel center to
// that edge — positive inside, negative outside. The fragment's coverage is the
// distance to the NEAREST edge, biased by +0.5 (a center exactly on an edge is
// half-covered) and clamped to [0,1]:
//
//   coverage = clamp(0.5 + min(d0, d1, d2), 0, 1).
//
// An interior pixel (all d_i large positive) saturates to 1 (fully covered); a
// center on an edge -> ~0.5; just outside -> < 0.5 toward 0. The fixed-point
// rasterizer reproduces this with integer math (raster.cc cov_byte); THIS float
// body is the parity oracle the raster coverage tests validate against.
//
// Harness carve-out: NOT orthodoxy_enforced (see tests/harness/CMakeLists.txt).

#include "oracle.h"

int oracle_coverage(float d0, float d1, float d2, float* out_cov) {
  float dmin = d0;
  if (d1 < dmin) {
    dmin = d1;
  }
  if (d2 < dmin) {
    dmin = d2;
  }
  float cov = 0.5F + dmin;
  if (cov < 0.0F) {
    cov = 0.0F;
  }
  if (cov > 1.0F) {
    cov = 1.0F;
  }
  *out_cov = cov;
  return 0;
}
