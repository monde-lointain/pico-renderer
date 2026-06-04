// aa.h — RDP-adapted coverage anti-aliasing (Stream R.3-AA). Orthodox C++ (POD
// + free module_verb functions, C headers, no STL/auto/exceptions).
//
// AA is a TWO-STAGE pipeline split across modules:
//   1) WRITE (in raster.cc): the rasterizer computes an analytic per-pixel
//      coverage byte [0,255] (255 = full/interior) at each winning opaque
//      fragment and stores it into a per-worker, tile-sized coverage scratch
//      (Frame.cov[worker], born in rdr/frame.h). XLU pixels store 255 (AA
//      ignores translucent geometry, plan v1).
//   2) RESOLVE (here): aa_resolve_tile blends each EDGE pixel (coverage < full)
//      with its in-tile neighbours, leaving INTERIOR (full-coverage) pixels
//      untouched. Cost scales with edge length, not tile area.
//
// Both stages are gated by a runtime enable (default OFF). When AA is disabled
// (or the caller passes a null coverage scratch) the rasterizer skips the
// coverage path entirely and is BYTE-IDENTICAL to the pre-AA renderer, and
// aa_resolve_tile is a no-op — so AA off costs nothing and changes no output.
//
// Determinism: the resolve reads ONLY pixels inside the tile rect (a neighbour
// off the tile edge falls back to the centre), so it never reads another
// worker's framebuffer region — the dual-core sweep stays bit-identical to
// serial (the same per-worker independence as zbuf). The per-pixel + resolve
// math is integer-only (host<->device bit-identical); floats live only in the
// float oracle (tests/harness/oracle_coverage.cc).
#ifndef RDR_AA_H
#define RDR_AA_H
#include "rdr/types.h"

// Coverage byte for a fully-covered (interior) fragment. The WRITE clears the
// tile coverage scratch to this on entry (like raster clears zbuf), so a pixel
// no fragment touched reads as full -> the resolve leaves it alone.
#define AA_COV_FULL 255

// Runtime enable (file-scope static behind accessors in aa.cc — Orthodox
// encapsulation, default OFF). Both the raster coverage WRITE and aa_resolve
// consult aa_enabled(); flip it ON only where AA is wanted (the demo/golden
// path keeps it OFF — Lead enables it at T4 integration).
void aa_set_enabled(int on);
int aa_enabled(void);

// Resolve one tile in place: for each EDGE pixel (cov[i] < AA_COV_FULL) blend
// the framebuffer colour toward the min/max of a small in-tile neighbour set by
// the RDP offset form, leaving interior (cov == AA_COV_FULL) pixels unchanged.
//   fb           : full-screen RGB565 framebuffer (row-major).
//   cov          : this tile's RDR_TILE_W*RDR_TILE_H coverage scratch
//                  (Frame.cov[worker]); row-major within the tile.
//   tile_px_x0/y0: the tile's top-left screen pixel.
// A no-op when !aa_enabled() or cov == 0. Neighbours outside the tile rect are
// NOT read (border-clamp) so no cross-tile / cross-core framebuffer race.
void aa_resolve_tile(uint16_t* fb, const uint8_t* cov, int tile_px_x0,
                     int tile_px_y0);

#endif  // RDR_AA_H
