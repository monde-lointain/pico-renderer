// raster.h — module interface (Stream A contract). Orthodox C++.
#ifndef RDR_RASTER_H
#define RDR_RASTER_H
#include "rdr/types.h"

void raster_tile(int tile, const struct TileBin* bin, uint16_t* fb, uint16_t* zbuf);

#endif  // RDR_RASTER_H
