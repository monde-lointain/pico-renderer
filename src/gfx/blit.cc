#include "gfx/blit.h"

#include <stddef.h>

/* ---- internal clip helpers ---------------------------------------------- */

/* Clip a 1D range [pos, pos+len) to [0, limit). Returns 0 if fully outside.
 * Sets *pos and *len to the clipped values. */
static int clip1d(int *pos, int *len, int limit) {
  if (*pos < 0) {
    *len += *pos;
    *pos = 0;
  }
  if (*len <= 0) {
    return 0;
  }
  if (*pos >= limit) {
    return 0;
  }
  if (*pos + *len > limit) {
    *len = limit - *pos;
  }
  return 1;
}

/* ---- primitives ---------------------------------------------------------- */

void hline(struct Framebuffer *fb, int x, int y, int len, color_t color) {
  if (y < 0 || y >= SCREEN_H) {
    return;
  }
  if (!clip1d(&x, &len, SCREEN_W)) {
    return;
  }
  color_t *p = fb->px + ((ptrdiff_t)y * SCREEN_W) + x;
  int i;
  for (i = 0; i < len; ++i) {
    p[i] = color;
  }
}

void vline(struct Framebuffer *fb, int x, int y, int len, color_t color) {
  if (x < 0 || x >= SCREEN_W) {
    return;
  }
  if (!clip1d(&y, &len, SCREEN_H)) {
    return;
  }
  color_t *p = fb->px + ((ptrdiff_t)y * SCREEN_W) + x;
  int i;
  for (i = 0; i < len; ++i) {
    p[(ptrdiff_t)i * SCREEN_W] = color;
  }
}

void rect(struct Framebuffer *fb, int x, int y, int w, int h, color_t color) {
  hline(fb, x, y, w, color);         /* top */
  hline(fb, x, y + h - 1, w, color); /* bottom */
  vline(fb, x, y, h, color);         /* left */
  vline(fb, x + w - 1, y, h, color); /* right */
}

void frect(struct Framebuffer *fb, int x, int y, int w, int h, color_t color) {
  /* clip the rectangle */
  if (!clip1d(&x, &w, SCREEN_W)) {
    return;
  }
  if (!clip1d(&y, &h, SCREEN_H)) {
    return;
  }
  color_t *row = fb->px + ((ptrdiff_t)y * SCREEN_W) + x;
  int j;
  int i;
  for (j = 0; j < h; ++j) {
    for (i = 0; i < w; ++i) {
      row[i] = color;
    }
    row += SCREEN_W;
  }
}

/* ---- sprite blit --------------------------------------------------------- */

/* Compute source pixel index given destination offset (dc, dr) within sprite,
 * accounting for H/V flip. */
static int src_idx(const struct Sprite *s, int dc, int dr, int flags) {
  const int sc = (flags & BLIT_HFLIP) ? (s->w - 1 - dc) : dc;
  const int sr = (flags & BLIT_VFLIP) ? (s->h - 1 - dr) : dr;
  return (sr * s->w) + sc;
}

void blit_copy(struct Framebuffer *fb, const struct Sprite *s, int x, int y,
               int flags) {
  /* Determine the intersection of the sprite rect with the screen. */
  int dx_start = 0;
  int dy_start = 0;
  int draw_w = (int)s->w;
  int draw_h = (int)s->h;

  /* Clip X */
  if (x < 0) {
    dx_start = -x;
    draw_w += x;
    x = 0;
  }
  if (draw_w <= 0) {
    return;
  }
  if (x >= SCREEN_W) {
    return;
  }
  if (x + draw_w > SCREEN_W) {
    draw_w = SCREEN_W - x;
  }

  /* Clip Y */
  if (y < 0) {
    dy_start = -y;
    draw_h += y;
    y = 0;
  }
  if (draw_h <= 0) {
    return;
  }
  if (y >= SCREEN_H) {
    return;
  }
  if (y + draw_h > SCREEN_H) {
    draw_h = SCREEN_H - y;
  }

  int dr;
  int dc;
  for (dr = 0; dr < draw_h; ++dr) {
    color_t *dst = fb->px + ((ptrdiff_t)(y + dr) * SCREEN_W) + x;
    for (dc = 0; dc < draw_w; ++dc) {
      const int sidx = src_idx(s, dx_start + dc, dy_start + dr, flags);
      dst[dc] = s->rgb565[sidx];
    }
  }
}

void blit_mask(struct Framebuffer *fb, const struct Sprite *s, int x, int y,
               int flags) {
  int dx_start = 0;
  int dy_start = 0;
  int draw_w = (int)s->w;
  int draw_h = (int)s->h;

  /* Clip X */
  if (x < 0) {
    dx_start = -x;
    draw_w += x;
    x = 0;
  }
  if (draw_w <= 0) {
    return;
  }
  if (x >= SCREEN_W) {
    return;
  }
  if (x + draw_w > SCREEN_W) {
    draw_w = SCREEN_W - x;
  }

  /* Clip Y */
  if (y < 0) {
    dy_start = -y;
    draw_h += y;
    y = 0;
  }
  if (draw_h <= 0) {
    return;
  }
  if (y >= SCREEN_H) {
    return;
  }
  if (y + draw_h > SCREEN_H) {
    draw_h = SCREEN_H - y;
  }

  int dr;
  int dc;
  for (dr = 0; dr < draw_h; ++dr) {
    color_t *dst = fb->px + ((ptrdiff_t)(y + dr) * SCREEN_W) + x;
    for (dc = 0; dc < draw_w; ++dc) {
      const int sidx = src_idx(s, dx_start + dc, dy_start + dr, flags);
      /* check mask bit */
      if (s->mask[sidx >> 3] & (uint8_t)(0x80 >> (sidx & 7))) {
        dst[dc] = s->rgb565[sidx];
      }
    }
  }
}
