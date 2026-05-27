/* blit_test.cc — unit tests for blit.cc primitives.
 * TDD: tests written first; all tests start RED until blit.cc exists. */

#include "gfx/blit.h"

#include <gtest/gtest.h>
#include <stdint.h>
#include <string.h>

#include "gfx/framebuffer.h"
#include "gfx/sprite.h"

/* ---- helpers ------------------------------------------------------------- */

static void fb_clear(Framebuffer *fb, color_t fill) {
  int i;
  for (i = 0; i < SCREEN_PIXELS; ++i) {
    fb->px[i] = fill;
  }
}

static color_t fb_get(const Framebuffer *fb, int x, int y) {
  return fb->px[(y * SCREEN_W) + x];
}

/* ---- rgb565 pack --------------------------------------------------------- */

TEST(Blit, Rgb565PacksCorrectly) {
  /* red max = 0xF800, green max = 0x07E0, blue max = 0x001F */
  EXPECT_EQ(rgb565(0xFF, 0x00, 0x00), (color_t)0xF800);
  EXPECT_EQ(rgb565(0x00, 0xFF, 0x00), (color_t)0x07E0);
  EXPECT_EQ(rgb565(0x00, 0x00, 0xFF), (color_t)0x001F);
  EXPECT_EQ(rgb565(0xFF, 0xFF, 0xFF), (color_t)0xFFFF);
  EXPECT_EQ(rgb565(0x00, 0x00, 0x00), (color_t)0x0000);
}

/* ---- hline --------------------------------------------------------------- */

TEST(Blit, HlineWritesCorrectPixels) {
  Framebuffer fb;
  fb_clear(&fb, 0x0000);
  color_t const red = rgb565(0xFF, 0, 0);
  hline(&fb, 5, 10, 4, red); /* x=5..8, y=10 */
  int x;
  for (x = 0; x < SCREEN_W; ++x) {
    color_t const pix = fb_get(&fb, x, 10);
    if (x >= 5 && x < 9) {
      EXPECT_EQ(pix, red) << "x=" << x;
    } else {
      EXPECT_EQ(pix, (color_t)0x0000) << "x=" << x << " should be bg";
    }
  }
}

TEST(Blit, HlineClipsLeft) {
  Framebuffer fb;
  fb_clear(&fb, 0x0000);
  color_t const c = rgb565(0, 0xFF, 0);
  hline(&fb, -3, 5, 7, c); /* starts off-screen, x=0..3 drawn */
  EXPECT_EQ(fb_get(&fb, 0, 5), c);
  EXPECT_EQ(fb_get(&fb, 3, 5), c);
  EXPECT_EQ(fb_get(&fb, 4, 5), (color_t)0x0000);
}

TEST(Blit, HlineClipsRight) {
  Framebuffer fb;
  fb_clear(&fb, 0x0000);
  color_t const c = rgb565(0, 0, 0xFF);
  hline(&fb, 238, 0, 5, c); /* x=238..239 drawn, 240..242 clipped */
  EXPECT_EQ(fb_get(&fb, 238, 0), c);
  EXPECT_EQ(fb_get(&fb, 239, 0), c);
}

TEST(Blit, HlineFullyOutsideDoesNothing) {
  Framebuffer fb;
  fb_clear(&fb, 0xBEEF);
  hline(&fb, 0, -1, 10, rgb565(0xFF, 0, 0)); /* y out of range */
  EXPECT_EQ(fb_get(&fb, 0, 0), (color_t)0xBEEF);
}

/* ---- vline --------------------------------------------------------------- */

TEST(Blit, VlineWritesCorrectPixels) {
  Framebuffer fb;
  fb_clear(&fb, 0x0000);
  color_t const c = rgb565(0xFF, 0xFF, 0);
  vline(&fb, 20, 10, 5, c); /* x=20, y=10..14 */
  int y;
  for (y = 0; y < SCREEN_H; ++y) {
    color_t const pix = fb_get(&fb, 20, y);
    if (y >= 10 && y < 15) {
      EXPECT_EQ(pix, c) << "y=" << y;
    } else {
      EXPECT_EQ(pix, (color_t)0x0000) << "y=" << y;
    }
  }
}

TEST(Blit, VlineClipsTop) {
  Framebuffer fb;
  fb_clear(&fb, 0x0000);
  color_t const c = rgb565(0xFF, 0, 0xFF);
  vline(&fb, 10, -3, 7, c); /* starts off-screen, y=0..3 drawn */
  EXPECT_EQ(fb_get(&fb, 10, 0), c);
  EXPECT_EQ(fb_get(&fb, 10, 3), c);
  EXPECT_EQ(fb_get(&fb, 10, 4), (color_t)0x0000);
}

TEST(Blit, VlineClipsBottom) {
  Framebuffer fb;
  fb_clear(&fb, 0x0000);
  color_t const c = rgb565(0, 0xFF, 0xFF);
  vline(&fb, 5, 238, 5, c);
  EXPECT_EQ(fb_get(&fb, 5, 238), c);
  EXPECT_EQ(fb_get(&fb, 5, 239), c);
}

/* ---- rect ---------------------------------------------------------------- */

TEST(Blit, RectDrawsHollowBorder) {
  Framebuffer fb;
  fb_clear(&fb, 0x0000);
  color_t const c = rgb565(0xFF, 0xFF, 0xFF);
  rect(&fb, 10, 10, 4, 4, c); /* 4x4 border: x=10..13, y=10..13 */

  /* corners */
  EXPECT_EQ(fb_get(&fb, 10, 10), c);
  EXPECT_EQ(fb_get(&fb, 13, 10), c);
  EXPECT_EQ(fb_get(&fb, 10, 13), c);
  EXPECT_EQ(fb_get(&fb, 13, 13), c);

  /* interior must be untouched */
  EXPECT_EQ(fb_get(&fb, 11, 11), (color_t)0x0000);
  EXPECT_EQ(fb_get(&fb, 12, 12), (color_t)0x0000);
}

/* ---- frect --------------------------------------------------------------- */

TEST(Blit, FrectFillsAllPixels) {
  Framebuffer fb;
  fb_clear(&fb, 0x0000);
  color_t const c = rgb565(0x80, 0x80, 0x80);
  frect(&fb, 5, 5, 3, 3, c); /* 3x3 fill */
  int x;
  int y;
  for (y = 5; y < 8; ++y) {
    for (x = 5; x < 8; ++x) {
      EXPECT_EQ(fb_get(&fb, x, y), c) << "(" << x << "," << y << ")";
    }
  }
  EXPECT_EQ(fb_get(&fb, 4, 5), (color_t)0x0000);
  EXPECT_EQ(fb_get(&fb, 5, 4), (color_t)0x0000);
}

TEST(Blit, FrectClipsToScreen) {
  Framebuffer fb;
  fb_clear(&fb, 0x0000);
  color_t const c = rgb565(0xFF, 0, 0);
  frect(&fb, -2, -2, 5, 5, c); /* starts off-screen, clips to 3x3 at origin */
  EXPECT_EQ(fb_get(&fb, 0, 0), c);
  EXPECT_EQ(fb_get(&fb, 2, 2), c);
  EXPECT_EQ(fb_get(&fb, 3, 0), (color_t)0x0000); /* just outside */
}

/* ---- blit_copy ----------------------------------------------------------- */

/* Build a small test sprite: 3x2, fully opaque. */
static const uint16_t K_COPY_PIXELS[] = {
    0xF800, 0x07E0, 0x001F, /* row 0: red, green, blue */
    0xFFFF, 0x0000, 0xF81F  /* row 1: white, black, magenta */
};
/* mask: 6 opaque pixels = bits 0b11111100 in first byte, then 0b11000000 */
static const uint8_t K_COPY_MASK[] = {0xFC, 0xC0};
static const Sprite K_COPY_SPRITE = {K_COPY_PIXELS, K_COPY_MASK, 3, 2};

TEST(Blit, BlitCopyWritesAllPixels) {
  Framebuffer fb;
  fb_clear(&fb, 0x0000);
  blit_copy(&fb, &K_COPY_SPRITE, 10, 20, 0);       /* no flip */
  EXPECT_EQ(fb_get(&fb, 10, 20), (color_t)0xF800); /* red */
  EXPECT_EQ(fb_get(&fb, 11, 20), (color_t)0x07E0); /* green */
  EXPECT_EQ(fb_get(&fb, 12, 20), (color_t)0x001F); /* blue */
  EXPECT_EQ(fb_get(&fb, 10, 21), (color_t)0xFFFF); /* white */
  EXPECT_EQ(fb_get(&fb, 11, 21), (color_t)0x0000); /* black */
  EXPECT_EQ(fb_get(&fb, 12, 21), (color_t)0xF81F); /* magenta */
}

TEST(Blit, BlitCopyHFlipReversesRows) {
  Framebuffer fb;
  fb_clear(&fb, 0x0000);
  blit_copy(&fb, &K_COPY_SPRITE, 10, 20, BLIT_HFLIP);
  /* H-flip: cols reversed, row 0 becomes [blue, green, red] */
  EXPECT_EQ(fb_get(&fb, 10, 20), (color_t)0x001F); /* blue (was col 2) */
  EXPECT_EQ(fb_get(&fb, 11, 20), (color_t)0x07E0); /* green */
  EXPECT_EQ(fb_get(&fb, 12, 20), (color_t)0xF800); /* red (was col 0) */
}

TEST(Blit, BlitCopyVFlipReversesColumns) {
  Framebuffer fb;
  fb_clear(&fb, 0x0000);
  blit_copy(&fb, &K_COPY_SPRITE, 10, 20, BLIT_VFLIP);
  /* V-flip: rows reversed, row 0 at dst becomes row 1 of src */
  EXPECT_EQ(fb_get(&fb, 10, 20), (color_t)0xFFFF); /* white (was row1,col0) */
  EXPECT_EQ(fb_get(&fb, 11, 20), (color_t)0x0000); /* black */
  EXPECT_EQ(fb_get(&fb, 12, 20), (color_t)0xF81F); /* magenta */
  EXPECT_EQ(fb_get(&fb, 10, 21), (color_t)0xF800); /* red (was row0,col0) */
}

TEST(Blit, BlitCopyClipsRight) {
  Framebuffer fb;
  fb_clear(&fb, 0x1234);
  /* place sprite so it runs off the right edge: x=239, sprite w=3 */
  blit_copy(&fb, &K_COPY_SPRITE, 239, 20, 0);
  EXPECT_EQ(fb_get(&fb, 239, 20), (color_t)0xF800); /* col 0 drawn */
  /* col 1 (x=240) and col 2 (x=241) are off screen — sentinel check */
  /* The test is: no crash, and x=238 is unchanged. */
  EXPECT_EQ(fb_get(&fb, 238, 20), (color_t)0x1234);
}

TEST(Blit, BlitCopyClipsBottom) {
  Framebuffer fb;
  fb_clear(&fb, 0x5678);
  blit_copy(&fb, &K_COPY_SPRITE, 10, 239,
            0); /* row 1 goes to y=240, off screen */
  EXPECT_EQ(fb_get(&fb, 10, 239), (color_t)0xF800); /* row 0 drawn */
  /* row 1 at y=240 is clipped — no crash is the assertion */
}

TEST(Blit, BlitCopyClipsLeft) {
  Framebuffer fb;
  fb_clear(&fb, 0xAAAA);
  blit_copy(&fb, &K_COPY_SPRITE, -1, 20, 0); /* col 0 off-screen */
  /* col 1 → x=0, col 2 → x=1 */
  EXPECT_EQ(fb_get(&fb, 0, 20), (color_t)0x07E0); /* green (src col 1) */
  EXPECT_EQ(fb_get(&fb, 1, 20), (color_t)0x001F); /* blue (src col 2) */
  EXPECT_EQ(fb_get(&fb, 2, 20), (color_t)0xAAAA); /* untouched */
}

TEST(Blit, BlitCopyClipsTop) {
  Framebuffer fb;
  fb_clear(&fb, 0xBBBB);
  blit_copy(&fb, &K_COPY_SPRITE, 10, -1, 0); /* row 0 off-screen */
  /* row 1 → y=0 */
  EXPECT_EQ(fb_get(&fb, 10, 0), (color_t)0xFFFF); /* white (src row 1, col 0) */
  EXPECT_EQ(fb_get(&fb, 10, 1), (color_t)0xBBBB); /* untouched */
}

/* ---- blit_mask ----------------------------------------------------------- */

/* Build a 3x2 sprite with some transparent pixels.
 * Pixel layout:
 *   row 0: [opaque-red] [transparent] [opaque-blue]
 *   row 1: [transparent] [opaque-green] [transparent]
 * mask bits (MSB-first, idx = y*3+x):
 *   idx 0 (row0,col0) = opaque → bit 7 of byte 0 = 1
 *   idx 1 (row0,col1) = transparent → bit 6 of byte 0 = 0
 *   idx 2 (row0,col2) = opaque → bit 5 of byte 0 = 1
 *   idx 3 (row1,col0) = transparent → bit 4 of byte 0 = 0
 *   idx 4 (row1,col1) = opaque → bit 3 of byte 0 = 1
 *   idx 5 (row1,col2) = transparent → bit 2 of byte 0 = 0
 * byte 0 = 1 0 1 0 1 0 0 0 = 0xA8
 */
static const uint16_t K_MASK_PIXELS[] = {
    0xF800, 0x0000, 0x001F, /* red, black(ignored), blue */
    0x0000, 0x07E0, 0x0000  /* black(ignored), green, black(ignored) */
};
static const uint8_t K_MASK_BYTES[] = {0xA8};
static const Sprite K_MASK_SPRITE = {K_MASK_PIXELS, K_MASK_BYTES, 3, 2};

TEST(Blit, BlitMaskOnlyDrawsOpaquePels) {
  Framebuffer fb;
  color_t const bg = rgb565(0x80, 0x40, 0x20);
  fb_clear(&fb, bg);
  blit_mask(&fb, &K_MASK_SPRITE, 10, 20, 0);

  /* opaque pixels drawn */
  EXPECT_EQ(fb_get(&fb, 10, 20), (color_t)0xF800); /* red */
  EXPECT_EQ(fb_get(&fb, 12, 20), (color_t)0x001F); /* blue */
  EXPECT_EQ(fb_get(&fb, 11, 21), (color_t)0x07E0); /* green */

  /* transparent pixels preserve background */
  EXPECT_EQ(fb_get(&fb, 11, 20), bg); /* transparent */
  EXPECT_EQ(fb_get(&fb, 10, 21), bg); /* transparent */
  EXPECT_EQ(fb_get(&fb, 12, 21), bg); /* transparent */
}

TEST(Blit, BlitMaskHFlipPreservesTransparency) {
  Framebuffer fb;
  color_t const bg = 0x1111;
  fb_clear(&fb, bg);
  blit_mask(&fb, &K_MASK_SPRITE, 10, 20, BLIT_HFLIP);
  /* H-flip: col order reversed.
   * src row0: [red, transparent, blue] → flipped [blue, transparent, red]
   * dst row0 x=10: blue, x=11: transparent(bg), x=12: red */
  EXPECT_EQ(fb_get(&fb, 10, 20), (color_t)0x001F); /* blue */
  EXPECT_EQ(fb_get(&fb, 11, 20), bg);              /* transparent */
  EXPECT_EQ(fb_get(&fb, 12, 20), (color_t)0xF800); /* red */
}

TEST(Blit, BlitMaskClipsCorrectly) {
  Framebuffer fb;
  color_t const bg = 0xCCCC;
  fb_clear(&fb, bg);
  /* place 1 col off left edge: col0 (transparent) clips, col1 → x=0 */
  blit_mask(&fb, &K_MASK_SPRITE, -1, 20, 0);
  /* col 1 (transparent) maps to x=0, should remain bg */
  EXPECT_EQ(fb_get(&fb, 0, 20), bg);
  /* col 2 (opaque blue) maps to x=1 */
  EXPECT_EQ(fb_get(&fb, 1, 20), (color_t)0x001F);
}
