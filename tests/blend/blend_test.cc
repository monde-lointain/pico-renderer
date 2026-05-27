// blend_test.cc — golden tests for the framebuffer blend ops.
// v1 ships BLEND_OPAQUE only: an opaque copy (src overwrites dst). src/dst are
// packed RGB565 (frozen contract: framebuffer color_t). Reference: design spec
// section 8b (opaque copy), N64 manual blender (force-blend / 1*src + 0*dst).

#include "blend/blend.h"

#include <stdint.h>

#include "gtest/gtest.h"

// BLEND_OPAQUE must reproduce src exactly regardless of dst (1*src + 0*dst).
TEST(Blend, OpaqueCopiesSrcIgnoringDst) {
  EXPECT_EQ(blend_pixel(BLEND_OPAQUE, 0xF800, 0x07E0), 0xF800);
  EXPECT_EQ(blend_pixel(BLEND_OPAQUE, 0x0000, 0xFFFF), 0x0000);
  EXPECT_EQ(blend_pixel(BLEND_OPAQUE, 0xFFFF, 0x0000), 0xFFFF);
  EXPECT_EQ(blend_pixel(BLEND_OPAQUE, 0x1234, 0xABCD), 0x1234);
}

// Opaque copy is independent of dst across the whole 565 range for a fixed src.
TEST(Blend, OpaqueIsDstIndependent) {
  uint16_t const src = 0x55AA;
  for (uint32_t dst = 0; dst <= 0xFFFF; dst += 0x101) {
    EXPECT_EQ(blend_pixel(BLEND_OPAQUE, src, (uint16_t)dst), src);
  }
}
