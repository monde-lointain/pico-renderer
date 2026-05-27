// blend.cc — framebuffer blend ops over packed RGB565. See blend.h.
// v1: opaque copy only. The N64 blender computes p*a + m*b; the opaque
// rendermode forces a=1, b=0 (src wins). Here both operands are already packed
// RGB565 (the combiner/pack step ran upstream), so the opaque path is a copy of
// src; dst is unused. Alpha-over/add and ordered dither are W-later.
#include "blend/blend.h"

uint16_t blend_pixel(uint8_t mode, uint16_t src, uint16_t dst) {
  switch (mode) {
    case BLEND_OPAQUE:
    default:
      // v1: opaque copy (src wins), dst unused. Unsupported modes fall back to
      // opaque (never silently corrupt the FB). BLEND_ALPHA/BLEND_ADD + ordered
      // dither land W-later, when each gets its own non-clone branch.
      (void)dst;
      return src;
  }
}
