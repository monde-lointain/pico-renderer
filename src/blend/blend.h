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

// L6 FRONT-TO-BACK PREMULTIPLIED UNDER (the XLU overdraw lever) ---------------
// The translucent sweep no longer over-blends each fragment into the
// framebuffer back-to-front. Instead it sorts FRONT-TO-BACK and ACCUMULATES a
// premultiplied "over from front" result into a per-tile accumulator
// (premultiplied color `c_acc` packed 565 + 8-bit accumulated alpha
// `acc_alpha`), then folds the opaque terrain under the accumulation ONCE at
// the end. Premultiplied UNDER lets a saturated pixel STOP compositing (the
// caller's saturation early-out), killing the measured 6.12x XLU overdraw.
//
// All channel math reuses the SAME +127-then-/255 round-to-nearest form as
// blend_over_channel (host<->device bit-identical, validated against the float
// oracle); see blend.cc.

// Accumulate one front-to-back fragment INTO the premultiplied accumulator.
//   t  = 255 - *acc_alpha                 (remaining transmittance)
//   ea = round(frag_alpha * t / 255)      (effective contribution alpha)
//   *c_acc_chan += round(frag_chan * ea / 255)   (clamp 255), per channel
//   *acc_alpha  += ea                            (clamp 255)
// `frag565` is the straight (un-premultiplied) fragment color (the FOGGED
// combiner output); `frag_alpha` is its 8-bit alpha (TEXEL0 alpha post
// alpha-compare). `c_acc`/`acc_alpha` are read-modify-written in place. A
// fragment with ea==0 (acc already saturated, or zero alpha) is a no-op — but
// the caller's early-out should skip those before calling.
void blend_premul_accumulate(uint16_t* c_acc, uint8_t* acc_alpha,
                             uint16_t frag565, uint8_t frag_alpha);

// Fold the opaque terrain UNDER the finished premultiplied accumulation:
//   out_chan = c_acc_chan + round((255 - acc_alpha) * terrain_chan / 255)
// per channel (clamp 255), returning the composited RGB565 to store. This is
// the correct front-to-back UNDER operator: terrain is scaled by the residual
// transmittance (255-acc_alpha) and added to the already-premultiplied
// accumulation (NOT an alpha-over of straight colors). Caller skips pixels with
// acc_alpha==0 (leaves the framebuffer's opaque terrain byte-identical).
uint16_t blend_premul_resolve(uint16_t c_acc, uint8_t acc_alpha,
                              uint16_t terrain565);

#endif  // RDR_BLEND_H
