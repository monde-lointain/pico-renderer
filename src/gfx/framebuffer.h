#ifndef GFX_FRAMEBUFFER_H
#define GFX_FRAMEBUFFER_H

#include <stdint.h>

/* FROZEN CONTRACT (Stream A). Shared 240x240 opaque RGB565 framebuffer.
 * color_t is native-endian uint16_t. The SDL path uploads it as
 * SDL_PIXELFORMAT_RGB565; the console PIO shifts bits MSB-first in panel order
 * (no separate CPU byteswap). */

enum { SCREEN_W = 240, SCREEN_H = 240, SCREEN_PIXELS = SCREEN_W * SCREEN_H };

typedef uint16_t color_t;

struct Framebuffer {
  color_t px[SCREEN_PIXELS];
};

/* Pack 8-bit RGB into RGB565 (R[5] G[6] B[5], red in high bits). */
static inline color_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (color_t)(((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) |
                   (uint16_t)(b >> 3));
}

#endif /* GFX_FRAMEBUFFER_H */
