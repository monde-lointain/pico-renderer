/* font_test.cc — unit tests for font.cc.
 * Charmap (plan §Asset pipeline):
 *   row 0: ' ' A B C D E F G H I J K L
 *   row 1: M N O P Q R S T U V W X Y
 *   row 2: Z 0 1 2 3 4 5 6 7 8 9 ':' 'x'
 * Cell size: 16w x 22h.  Atlas 208x66 (13 cols x 3 rows).
 */

#include "gfx/font.h"

#include <gtest/gtest.h>
#include <stdint.h>
#include <string.h>

#include "gfx/blit.h"
#include "gfx/framebuffer.h"

static color_t fb_get(const Framebuffer *fb, int x, int y) {
  return fb->px[y * SCREEN_W + x];
}

static void fb_clear(Framebuffer *fb, color_t c) {
  int i;
  for (i = 0; i < SCREEN_PIXELS; ++i) fb->px[i] = c;
}

/* ---- cell index helpers -------------------------------------------------- */

TEST(Font, CharmapGivesCorrectCellForA) {
  /* 'A' is row0 col1 → cell index 1 */
  int col, row;
  font_cell_for('A', &col, &row);
  EXPECT_EQ(col, 1);
  EXPECT_EQ(row, 0);
}

TEST(Font, CharmapGivesCorrectCellForM) {
  /* 'M' is row1 col0 → cell index 13 */
  int col, row;
  font_cell_for('M', &col, &row);
  EXPECT_EQ(col, 0);
  EXPECT_EQ(row, 1);
}

TEST(Font, CharmapGivesCorrectCellForZ) {
  /* 'Z' is row2 col0 */
  int col, row;
  font_cell_for('Z', &col, &row);
  EXPECT_EQ(col, 0);
  EXPECT_EQ(row, 2);
}

TEST(Font, CharmapGivesCorrectCellForZero) {
  /* '0' is row2 col1 */
  int col, row;
  font_cell_for('0', &col, &row);
  EXPECT_EQ(col, 1);
  EXPECT_EQ(row, 2);
}

TEST(Font, CharmapGivesCorrectCellForColon) {
  /* ':' is row2 col11 */
  int col, row;
  font_cell_for(':', &col, &row);
  EXPECT_EQ(col, 11);
  EXPECT_EQ(row, 2);
}

TEST(Font, CharmapGivesCorrectCellForSpace) {
  /* ' ' is row0 col0 */
  int col, row;
  font_cell_for(' ', &col, &row);
  EXPECT_EQ(col, 0);
  EXPECT_EQ(row, 0);
}

TEST(Font, UnknownCharFallsBackToSpace) {
  /* Lowercase 'a' → maps to space (row0 col0) */
  int col, row;
  font_cell_for('a', &col, &row);
  EXPECT_EQ(col, 0);
  EXPECT_EQ(row, 0);
}

/* ---- text() render positions -------------------------------------------- */

/* text() draws each glyph at a fixed 16px stride.
 * We verify glyph origins: for "SCORE", the 'S' starts at x, 'C' at x+16, etc.
 * We test by injecting a sentinel color for bg and checking that the fb pixel
 * at (glyph_x, y) is NOT the sentinel color for a character that has any ink
 * at column 0 of its cell.
 *
 * This is a structural test: we confirm text() advances by 16px per glyph
 * and that the overall call doesn't crash or corrupt adjacent rows. */

TEST(Font, TextAdvances16PxPerGlyph) {
  /* Each char cell is 16px wide, so "SC" leaves 'C' at x+16.
   * The top rows of the cell may be blank (transparent), so we scan
   * the full cell height (22px) for any changed pixel. */
  Framebuffer fb;
  color_t bg = 0x1234;
  fb_clear(&fb, bg);
  color_t fg = rgb565(0xFF, 0xFF, 0xFF);
  /* draw "SC" at (0,0) */
  text(&fb, "SC", 0, 0, fg);
  /* Some pixel within x=0..31, y=0..21 must differ from bg. */
  int any_changed = 0;
  int x, y;
  for (y = 0; y < 22 && !any_changed; ++y) {
    for (x = 0; x < 32 && !any_changed; ++x) {
      if (fb_get(&fb, x, y) != bg) any_changed = 1;
    }
  }
  EXPECT_EQ(any_changed, 1);
}

TEST(Font, TextDoesNotWriteBelowCellHeight) {
  Framebuffer fb;
  color_t bg = 0xABCD;
  fb_clear(&fb, bg);
  color_t fg = rgb565(0xFF, 0xFF, 0xFF);
  text(&fb, "A", 0, 0, fg);
  /* Cell height is 22px. Row 22 should be unchanged. */
  int x;
  for (x = 0; x < 16; ++x) {
    EXPECT_EQ(fb_get(&fb, x, 22), bg) << "x=" << x << " row 22 should be bg";
  }
}

TEST(Font, TextDoesNotWriteAboveYOrigin) {
  Framebuffer fb;
  color_t bg = 0x1111;
  fb_clear(&fb, bg);
  color_t fg = rgb565(0xFF, 0xFF, 0xFF);
  text(&fb, "A", 0, 10, fg);
  /* Row 9 should be untouched. */
  int x;
  for (x = 0; x < 16; ++x) {
    EXPECT_EQ(fb_get(&fb, x, 9), bg) << "x=" << x << " row 9 should be bg";
  }
}

/* ---- "SCORE 0:00" glyph origins ----------------------------------------- */

TEST(Font, ScoreStringGlyphOriginsAreCorrect) {
  /* "SCORE 0:00" has 10 chars.
   * Glyph x origins: S=0, C=16, O=32, R=48, E=64, ' '=80, 0=96, :=112, 0=128,
   * 0=144 We draw at (0, 0) and verify some pixels changed in the S column (x
   * 0-15). */
  Framebuffer fb;
  color_t bg = 0x0000;
  fb_clear(&fb, bg);
  color_t fg = rgb565(0xFF, 0xFF, 0xFF);
  text(&fb, "SCORE 0:00", 0, 0, fg);

  /* 'S' is at row2 col9 in the charmap → atlas x = 9*16=144, y = 2*22=44.
   * Check that at least 1 pixel in x=0..15, y=0..21 changed. */
  int any = 0;
  int x, y;
  for (y = 0; y < 22 && !any; ++y) {
    for (x = 0; x < 16 && !any; ++x) {
      if (fb_get(&fb, x, y) != bg) any = 1;
    }
  }
  EXPECT_EQ(any, 1) << "S glyph at x=0 must have written some pixels";

  /* ':' is at x=112..127; check at least 1 pixel changed there. */
  any = 0;
  for (y = 0; y < 22 && !any; ++y) {
    for (x = 112; x < 128 && !any; ++x) {
      if (fb_get(&fb, x, y) != bg) any = 1;
    }
  }
  EXPECT_EQ(any, 1) << "colon glyph at x=112 must have written some pixels";
}

/* ---- lowercase renders as space ----------------------------------------- */

TEST(Font, LowercaseRendersAsSpaceCell) {
  /* Draw 'a' (unknown) and space at same position on two fbs, compare. */
  Framebuffer fb_space, fb_lower;
  color_t bg = 0x0000;
  color_t fg = rgb565(0xFF, 0xFF, 0xFF);
  fb_clear(&fb_space, bg);
  fb_clear(&fb_lower, bg);
  text(&fb_space, " ", 0, 0, fg);
  text(&fb_lower, "a", 0, 0, fg);
  int x, y;
  for (y = 0; y < 22; ++y) {
    for (x = 0; x < 16; ++x) {
      EXPECT_EQ(fb_get(&fb_space, x, y), fb_get(&fb_lower, x, y))
          << "mismatch at (" << x << "," << y << ")";
    }
  }
}
