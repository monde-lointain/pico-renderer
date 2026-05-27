#ifndef GFX_FONT_H
#define GFX_FONT_H

#include "gfx/framebuffer.h"

/* pico8_font atlas: 16px-wide x 22px-tall cells, 13 cols x 3 rows = 39 glyphs.
 * Charmap (row-major, 13/row):
 *   row 0: ' ' A B C D E F G H I J K L
 *   row 1: M N O P Q R S T U V W X Y
 *   row 2: Z 0 1 2 3 4 5 6 7 8 9 ':' 'x'
 * Uppercase only; unknown chars render as space. */

enum { FONT_CELL_W = 16, FONT_CELL_H = 22, FONT_COLS = 13, FONT_ROWS = 3 };

/* Look up the atlas cell (col, row) for character c.
 * Unknown characters map to col=0, row=0 (space). */
void font_cell_for(char c, int *col, int *row);

/* Mask-blit the string s at pixel (x, y) in the given foreground color.
 * Each glyph advances 16px horizontally. No word-wrap. */
void text(struct Framebuffer *fb, const char *s, int x, int y, color_t fg);

#endif /* GFX_FONT_H */
