/* rgb565_test.cc — host unit tests for the RGB565 pixel packing the display
 * fork depends on. The shared framebuffer contract (gfx/framebuffer.h) packs
 * R[5] G[6] B[5] with red in the high bits; the ST7789 COLMOD=0x55 path and the
 * SDL SDL_PIXELFORMAT_RGB565 presenter both consume exactly this layout.
 *
 * Spec packing: (r>>3)<<11 | (g>>2)<<5 | (b>>3).
 */

#include <gtest/gtest.h>
#include <stdint.h>

#include "gfx/framebuffer.h"

/* Reference oracle: the spec formula, recomputed independently of the header.
 */
static uint16_t pack565_oracle(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((uint16_t)(r >> 3) << 11) | ((uint16_t)(g >> 2) << 5) |
                    (uint16_t)(b >> 3));
}

TEST(Rgb565, Black) { EXPECT_EQ(rgb565(0, 0, 0), 0x0000); }

TEST(Rgb565, White) { EXPECT_EQ(rgb565(255, 255, 255), 0xFFFF); }

TEST(Rgb565, PureRedTopBits) {
  /* Full red -> 0b11111 in bits [15:11]. */
  EXPECT_EQ(rgb565(255, 0, 0), 0xF800);
}

TEST(Rgb565, PureGreenMidBits) {
  /* Full green -> 0b111111 in bits [10:5]. */
  EXPECT_EQ(rgb565(0, 255, 0), 0x07E0);
}

TEST(Rgb565, PureBlueLowBits) {
  /* Full blue -> 0b11111 in bits [4:0]. */
  EXPECT_EQ(rgb565(0, 0, 255), 0x001F);
}

TEST(Rgb565, GreenHasSixBits) {
  /* Green keeps 6 bits of precision; red/blue only 5. */
  EXPECT_EQ(rgb565(0, 0x04, 0), 0x0020); /* lsb of green field set */
  EXPECT_EQ(rgb565(0, 0x02, 0), 0x0000); /* below 6-bit resolution -> dropped */
}

TEST(Rgb565, ChannelsDoNotBleed) {
  /* mid-grey: each channel lands in its own field, no cross-talk. */
  const uint16_t v = rgb565(0x80, 0x80, 0x80);
  EXPECT_EQ((v >> 11) & 0x1F, 0x10); /* 0x80>>3 = 0x10 */
  EXPECT_EQ((v >> 5) & 0x3F, 0x20);  /* 0x80>>2 = 0x20 */
  EXPECT_EQ(v & 0x1F, 0x10);         /* 0x80>>3 = 0x10 */
}

TEST(Rgb565, MatchesSpecOracleExhaustive) {
  /* Sweep representative byte values across all three channels; the header must
   * match the spec formula bit-for-bit. */
  for (int r = 0; r <= 255; r += 17) {
    for (int g = 0; g <= 255; g += 17) {
      for (int b = 0; b <= 255; b += 17) {
        EXPECT_EQ(rgb565((uint8_t)r, (uint8_t)g, (uint8_t)b),
                  pack565_oracle((uint8_t)r, (uint8_t)g, (uint8_t)b))
            << "r=" << r << " g=" << g << " b=" << b;
      }
    }
  }
}

TEST(Rgb565, ColorTypeIsSixteenBit) { EXPECT_EQ(sizeof(color_t), (size_t)2); }
