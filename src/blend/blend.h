// blend.h — module interface (Stream A contract). Orthodox C++.
// Framebuffer blend ops over packed RGB565 colors (frozen color_t layout). v1
// ships the opaque-copy mode only; alpha-over/add and ordered RGB dither are
// W-later. `mode` takes a BlendMode value (named, but the frozen ABI is the
// uint8_t in the signature -- not a cross-module type, so it lives here).
#ifndef RDR_BLEND_H
#define RDR_BLEND_H
#include "rdr/types.h"

// Blend modes (valid values for blend_pixel's `mode`). v1: opaque only.
enum BlendMode {
  BLEND_OPAQUE = 0,  // src overwrites dst (1*src + 0*dst)
  BLEND_ALPHA = 1,   // alpha-over (W-later)
  BLEND_ADD = 2      // additive (W-later)
};

// Blend src over dst per `mode`; both are packed RGB565. Returns the RGB565
// result to store in the framebuffer. v1 implements BLEND_OPAQUE only.
uint16_t blend_pixel(uint8_t mode, uint16_t src, uint16_t dst);

#endif  // RDR_BLEND_H
