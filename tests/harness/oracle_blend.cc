// oracle_blend.cc — float reference for framebuffer blend (source-alpha blend
// of src over dst). Owned by Wave-D stream D.3 (blend).
//
// RGB565 has no destination alpha (design spec): only a SOURCE-alpha "over"
// (BLEND_ALPHA) and a clamped additive blend (BLEND_ADD) are expressible. The
// leading uint8_t selects the blend equation -- it carries a BlendMode value
// (the frozen oracle.h signature names it `zmode`, but for blend it is the mode
// id):
//   BLEND_OPAQUE (0): src wins; alpha ignored.
//   BLEND_ALPHA  (1): out = src*a + dst*(1-a),     per channel.
//   BLEND_ADD    (2): out = clamp01(src*a + dst),  per channel.
// `src_rgba` is 8-bit RGBA -- the SOURCE alpha is src_rgba[3] in [0,255]. The
// `coverage` arg is the analytic coverage in [0,1] and multiplies the source
// alpha (so XLU blends on TEXEL alpha * coverage; pass coverage=1 for a pure
// texel-alpha blend). `dst_rgb` is 8-bit RGB. Result -> out_rgb[3] (8-bit).
//
// Harness carve-out: NOT orthodoxy_enforced (see tests/harness/CMakeLists.txt).

#include <math.h>

#include "oracle.h"

static uint8_t oclamp_u8(float v) {
  // Round to nearest, clamp to [0,255]. Matches the device's round-then-clamp.
  long r = lrintf(v);
  if (r < 0) {
    r = 0;
  }
  if (r > 255) {
    r = 255;
  }
  return (uint8_t)r;
}

int oracle_blend(uint8_t zmode, const uint8_t* src_rgba, float coverage,
                 const uint8_t* dst_rgb, uint8_t out_rgb[3]) {
  if (src_rgba == 0 || dst_rgb == 0) {
    return 1;  // bad argument
  }
  // Effective source alpha = texel alpha * coverage (both in [0,1]).
  float a = ((float)src_rgba[3] / 255.0F) * coverage;
  if (a < 0.0F) {
    a = 0.0F;
  }
  if (a > 1.0F) {
    a = 1.0F;
  }

  for (int i = 0; i < 3; ++i) {
    float const s = (float)src_rgba[i];
    float const d = (float)dst_rgb[i];
    float out;
    switch (zmode) {
      case 1:  // BLEND_ALPHA: alpha-over
        out = (s * a) + (d * (1.0F - a));
        break;
      case 2:  // BLEND_ADD: additive (source pre-scaled by alpha), clamped
        out = (s * a) + d;
        break;
      case 0:   // BLEND_OPAQUE
      default:  // unknown mode -> opaque (never corrupt the FB)
        out = s;
        break;
    }
    out_rgb[i] = oclamp_u8(out);
  }
  return 0;
}
