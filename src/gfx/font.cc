#include "gfx/font.h"

#include <stddef.h>
#include <stdint.h>

#include "gfx/assets_gen.h"
#include "gfx/blit.h"
#include "gfx/sprite.h"

/* Charmap: 39 entries laid out row-major, 13/row.
 * Index = row * FONT_COLS + col.
 * Entry 0 = space (blank cell). */
static const char CHARMAP[FONT_COLS * FONT_ROWS] = {
    /* row 0 */
    ' ', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L',
    /* row 1 */
    'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y',
    /* row 2 */
    'Z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', ':', 'x'};

void font_cell_for(char c, int *col, int *row) {
  int i;
  for (i = 0; i < FONT_COLS * FONT_ROWS; ++i) {
    if (CHARMAP[i] == c) {
      *col = i % FONT_COLS;
      *row = i / FONT_COLS;
      return;
    }
  }
  /* Unknown → space (row 0, col 0) */
  *col = 0;
  *row = 0;
}

/* Blit a single glyph cell from the font atlas at dst (dx, dy) using mask. */
static void blit_glyph(struct Framebuffer *fb, const struct Sprite *atlas,
                       int cell_col, int cell_row, int dx, int dy, color_t fg) {
  /* We need to blit a sub-region of the atlas.
   * The atlas Sprite covers 208x66 pixels.
   * Glyph source rect: (cell_col*16, cell_row*22, 16, 22).
   * Strategy: iterate over the glyph cell and mask-blit each pixel
   * individually. This avoids needing a sub-sprite blit function while staying
   * Orthodox. */
  const int src_x0 = cell_col * FONT_CELL_W;
  const int src_y0 = cell_row * FONT_CELL_H;
  int sy;
  int sx;
  for (sy = 0; sy < FONT_CELL_H; ++sy) {
    const int dy_abs = dy + sy;
    if (dy_abs < 0 || dy_abs >= SCREEN_H) {
      continue;
    }
    for (sx = 0; sx < FONT_CELL_W; ++sx) {
      const int dx_abs = dx + sx;
      if (dx_abs < 0 || dx_abs >= SCREEN_W) {
        continue;
      }

      const int sidx = ((src_y0 + sy) * (int)atlas->w) + (src_x0 + sx);
      if (atlas->mask[sidx >> 3] & (uint8_t)(0x80 >> (sidx & 7))) {
        fb->px[(dy_abs * SCREEN_W) + dx_abs] = fg;
      }
    }
  }
}

void text(struct Framebuffer *fb, const char *s, int x, int y, color_t fg) {
  const struct Sprite *atlas = sprite_get(SPRITE_FONT);
  if (!atlas) {
    return;
  }

  int cx = x;
  const char *p;
  for (p = s; *p != '\0'; ++p) {
    int col;
    int row;
    font_cell_for(*p, &col, &row);
    blit_glyph(fb, atlas, col, row, cx, y, fg);
    cx += FONT_CELL_W;
  }
}
