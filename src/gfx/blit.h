#ifndef GFX_BLIT_H
#define GFX_BLIT_H

#include <stdint.h>

#include "gfx/framebuffer.h"
#include "gfx/sprite.h"

/* Flip flags for blit_copy / blit_mask. */
enum BlitFlags {
  BLIT_NONE = 0,
  BLIT_HFLIP = 1, /* mirror horizontally */
  BLIT_VFLIP = 2  /* mirror vertically */
};

/* Draw a horizontal line from (x, y) for len pixels. Clips to 240x240. */
void hline(struct Framebuffer *fb, int x, int y, int len, color_t color);

/* Draw a vertical line from (x, y) for len pixels. Clips to 240x240. */
void vline(struct Framebuffer *fb, int x, int y, int len, color_t color);

/* Draw a hollow rectangle outline. Clips to 240x240. */
void rect(struct Framebuffer *fb, int x, int y, int w, int h, color_t color);

/* Draw a filled rectangle. Clips to 240x240. */
void frect(struct Framebuffer *fb, int x, int y, int w, int h, color_t color);

/* Blit all pixels of sprite to (x, y), ignoring the mask (opaque copy).
 * flags is a combination of BlitFlags. Clips to 240x240. */
void blit_copy(struct Framebuffer *fb, const struct Sprite *s, int x, int y,
               int flags);

/* Blit sprite to (x, y), skipping pixels where mask bit is clear.
 * flags is a combination of BlitFlags. Clips to 240x240. */
void blit_mask(struct Framebuffer *fb, const struct Sprite *s, int x, int y,
               int flags);

#endif /* GFX_BLIT_H */
