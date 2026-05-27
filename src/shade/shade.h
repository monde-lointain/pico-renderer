// shade.h — module interface (Stream A contract). Orthodox C++.
#ifndef RDR_SHADE_H
#define RDR_SHADE_H
#include "rdr/types.h"

uint16_t shade_pixel(const struct RenderState* st, uint16_t texel, uint16_t shade, uint8_t* keep);

#endif  // RDR_SHADE_H
