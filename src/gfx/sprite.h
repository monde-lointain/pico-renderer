#ifndef GFX_SPRITE_H
#define GFX_SPRITE_H

#include <stdint.h>

/* FROZEN CONTRACT (Stream A). A sprite is an RGB565 pixel array plus a packed
 * 1-bit alpha mask. Mask bit layout: row-major, bit index = y*w + x, byte =
 * mask[idx >> 3], bit = 0x80 >> (idx & 7); set bit = opaque, clear = skip. */

struct Sprite {
  const uint16_t *rgb565; /* w*h pixels */
  const uint8_t *mask;    /* ceil(w*h / 8) bytes, MSB-first */
  uint16_t w;
  uint16_t h;
};

static inline bool sprite_opaque_at(const struct Sprite *s, uint16_t x,
                                    uint16_t y) {
  const uint32_t idx = ((uint32_t)y * s->w) + x;
  return (s->mask[idx >> 3] & (uint8_t)(0x80 >> (idx & 7))) != 0;
}

#endif /* GFX_SPRITE_H */
