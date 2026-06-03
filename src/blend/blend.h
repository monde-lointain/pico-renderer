// blend.h — module interface (Stream A contract). Orthodox C++.
// Framebuffer blend ops over packed RGB565 colors (frozen color_t layout) plus
// the fog lerp. `mode` takes a BlendMode value (named, but the frozen ABI is
// the uint8_t in the signature -- not a cross-module type, so it lives here).
//
// RGB565 has NO destination alpha (design spec section 8b / "RGB565 has no
// destination alpha"): only a SOURCE-alpha "over" and a clamped additive blend
// are expressible. Alpha-over therefore takes a SOURCE alpha argument supplied
// by the caller (the combiner alpha for XLU = TEXEL0 alpha, NOT coverage); see
// the XLU-alpha note in blend.cc.
#ifndef RDR_BLEND_H
#define RDR_BLEND_H
#include "rdr/types.h"

// Blend modes (valid values for blend_pixel / blend_pixel_alpha `mode`).
enum BlendMode {
  BLEND_OPAQUE = 0,  // src overwrites dst (1*src + 0*dst)
  BLEND_ALPHA = 1,   // alpha-over: src*a + dst*(1-a), a = source alpha
  BLEND_ADD = 2      // additive: clamp(src*a + dst), a = source alpha
};

// Blend src over dst per `mode`; both are packed RGB565. Returns the RGB565
// result to store in the framebuffer. This entry point carries no source alpha,
// so only BLEND_OPAQUE is meaningful here (src wins); any other mode also falls
// back to opaque (never silently corrupts the FB). Use blend_pixel_alpha for
// the source-alpha blends.
uint16_t blend_pixel(uint8_t mode, uint16_t src, uint16_t dst);

// Blend src over dst per `mode`, with an explicit 8-bit SOURCE alpha in [0,255]
// (255 = fully opaque). src/dst are packed RGB565; returns the RGB565 result.
//   BLEND_OPAQUE: src wins (alpha ignored).
//   BLEND_ALPHA : out = round(src*a + dst*(255-a) over 255), per channel.
//   BLEND_ADD   : out = clamp255(round(src*a / 255) + dst), per channel.
// Channel math is unpacked to 8-bit, blended, then repacked to 565 (so the 565
// quantization is the same as a fresh pack). Rounding is an independent +127
// bias applied before each /255; the divide itself is over non-negative
// operands -> truncation == floor (host<->device bit-identical). See blend.cc
// for why this round-to-nearest is deliberate and NOT the same as a pure
// truncating divide.
uint16_t blend_pixel_alpha(uint8_t mode, uint16_t src, uint8_t src_alpha,
                           uint16_t dst);

// Lerp a packed RGB565 color toward fog_color by an 8-bit factor in [0,255],
// where factor maps the spec's fog_factor in [0,1] as factor/255 (0 = no fog,
// 255 = full fog_color). Returns the RGB565 result:
//   out = round(color*(255-factor) + fog_color*factor over 255), per channel.
// The depth->factor derivation (D1/Q9 z-space) lives in the caller; this takes
// the factor directly.
uint16_t fog_lerp(uint16_t color, uint16_t fog_color, uint8_t factor);

#endif  // RDR_BLEND_H
