#include "shade/shade.h"

uint16_t shade_pixel(const struct RenderState* st, uint16_t texel, uint16_t shade, uint8_t* keep) { (void)st; (void)shade; if (keep) *keep=1; return texel; }
