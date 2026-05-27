// raster.cc — half-space edge-function rasterizer (flat fill + per-tile Z).
// Stream B.1-gamma. Orthodox C++: POD + free functions, C headers, C-style
// casts, no STL/auto/lambda/templates/exceptions. See raster.h for contract.
#include "raster/raster.h"

#include <stdint.h>
#include <string.h>

#include "rdr/config.h"

// ---- subpixel / depth constants --------------------------------------------
// Q12.4 screen coords: 16 (=2^4) subpixel steps per pixel. Pixel-center sample
// is integer-pixel + 0.5px = +8 subpixel units.
enum { RASTER_SUBPX = 4, RASTER_HALF = 8 };

// Degenerate reject: |2*area| in Q12.4 area units. 2*area is the cross product
// of two Q12.4 edge vectors -> Q24.8; one full subpixel-cell of area is 16*16 =
// 256. Reject anything not strictly larger than that (a sliver < 1 px area
// carries no reliable winding / would blow up the 1/area reciprocal).
enum { RASTER_AREA_EPS = 256 };

// Edge function in Q12.4 integer space: signed area of (a->b) x (a->p). > 0
// means p is to the left of directed edge a->b (screen y-down). Coords are
// bounded (guard-band clipped) so the int32 product does not overflow.
static int32_t edge_eval(fx12_4 ax, fx12_4 ay, fx12_4 bx, fx12_4 by, fx12_4 px,
                         fx12_4 py) {
  return ((int32_t)(bx - ax) * (int32_t)(py - ay)) -
         ((int32_t)(by - ay) * (int32_t)(px - ax));
}

// Top-left edge test (RDP / oracle convention, winding normalized to area>0):
// an edge a->b is a "top" edge if horizontal pointing in -x, or a "left" edge
// if it goes upward (dy < 0 in screen y-down). Samples exactly on a top-left
// edge are inclusive; on other edges, exclusive.
static int edge_is_top_left(fx12_4 ax, fx12_4 ay, fx12_4 bx, fx12_4 by) {
  int32_t const ex = (int32_t)bx - (int32_t)ax;
  int32_t const ey = (int32_t)by - (int32_t)ay;
  int const top = (ey == 0 && ex < 0);
  int const left = (ey < 0);
  return top || left;
}

// Map interpolated 1/w (Q16.16) to the uint16 per-tile depth scratch value.
// Monotonic in closeness (larger inv_w -> larger stored -> closer). inv_w for a
// valid (in-front) fragment is >= 0; negatives clamp to 0 (= farthest = clear).
static uint16_t depth_pack(int32_t inv_w_q16) {
  if (inv_w_q16 <= 0) {
    return RASTER_Z_CLEAR;
  }
  int32_t const d = inv_w_q16 >> 4;  // Q16.16 -> Q12.12-ish, range-fit
  if (d > 0xFFFF) {
    return (uint16_t)0xFFFF;
  }
  return (uint16_t)d;
}

// One transformed triangle, normalized to positive screen-space area, with the
// per-edge top-left bias precomputed. Holds copies (not pool indices) so the
// inner loop is self-contained.
struct TriSetup {
  fx12_4 x0, y0, x1, y1, x2, y2;
  int32_t iw0, iw1, iw2;  // 1/w (Q16.16) per vertex, post winding-normalize
  int32_t area2;          // 2*signed area (Q24.8), > 0 after normalize
  int tl0, tl1, tl2;      // top-left flags for edges v0->v1, v1->v2, v2->v0
  uint16_t color;         // flat fill (provoking vertex v0)
};

// Build TriSetup from the three transformed verts. Returns 0 on success, or
// RDR_EDEGENERATE if the triangle area is <= epsilon (rejected at setup).
static int tri_setup(struct TriSetup* t, const struct TVtx* a,
                     const struct TVtx* b, const struct TVtx* c) {
  fx12_4 const x0 = a->x;
  fx12_4 const y0 = a->y;
  fx12_4 x1 = b->x;
  fx12_4 y1 = b->y;
  fx12_4 x2 = c->x;
  fx12_4 y2 = c->y;
  int32_t const iw0 = (int32_t)a->inv_w;
  int32_t iw1 = (int32_t)b->inv_w;
  int32_t iw2 = (int32_t)c->inv_w;

  int32_t area2 = edge_eval(x0, y0, x1, y1, x2, y2);
  int32_t const mag = (area2 < 0) ? -area2 : area2;
  if (mag <= RASTER_AREA_EPS) {
    return RDR_EDEGENERATE;
  }
  if (area2 < 0) {
    // Swap v1<->v2 (and their attrs) to normalize to positive area.
    fx12_4 const tx = x1;
    fx12_4 const ty = y1;
    int32_t const tiw = iw1;
    x1 = x2;
    y1 = y2;
    iw1 = iw2;
    x2 = tx;
    y2 = ty;
    iw2 = tiw;
    area2 = -area2;
  }

  t->x0 = x0;
  t->y0 = y0;
  t->x1 = x1;
  t->y1 = y1;
  t->x2 = x2;
  t->y2 = y2;
  t->iw0 = iw0;
  t->iw1 = iw1;
  t->iw2 = iw2;
  t->area2 = area2;
  t->tl0 = edge_is_top_left(x0, y0, x1, y1);
  t->tl1 = edge_is_top_left(x1, y1, x2, y2);
  t->tl2 = edge_is_top_left(x2, y2, x0, y0);
  t->color = a->rgba;
  return RDR_OK;
}

// Clamp helper for the per-tile bounding box (tile-local pixel coords).
static int clampi(int v, int lo, int hi) {
  if (v < lo) {
    return lo;
  }
  if (v > hi) {
    return hi;
  }
  return v;
}

// Rasterize one already-setup triangle into the framebuffer + tile Z scratch.
// tile_px_x0/y0 = the tile's top-left pixel in screen space.
static void raster_one(const struct TriSetup* t, int tile_px_x0, int tile_px_y0,
                       uint16_t* fb, uint16_t* zbuf) {
  // Triangle screen bounding box (pixels), then intersect with the tile rect.
  fx12_4 minx_q = t->x0;
  fx12_4 maxx_q = t->x0;
  fx12_4 miny_q = t->y0;
  fx12_4 maxy_q = t->y0;
  if (t->x1 < minx_q) {
    minx_q = t->x1;
  }
  if (t->x1 > maxx_q) {
    maxx_q = t->x1;
  }
  if (t->x2 < minx_q) {
    minx_q = t->x2;
  }
  if (t->x2 > maxx_q) {
    maxx_q = t->x2;
  }
  if (t->y1 < miny_q) {
    miny_q = t->y1;
  }
  if (t->y1 > maxy_q) {
    maxy_q = t->y1;
  }
  if (t->y2 < miny_q) {
    miny_q = t->y2;
  }
  if (t->y2 > maxy_q) {
    maxy_q = t->y2;
  }

  // Q12.4 -> integer pixel: floor(min/16), ceil(max/16) via arithmetic shift +
  // round-up. Negative coords floor toward -inf via shift (arithmetic).
  int bb_minx = (int)(minx_q >> RASTER_SUBPX);
  int bb_miny = (int)(miny_q >> RASTER_SUBPX);
  int bb_maxx = (int)((maxx_q + 15) >> RASTER_SUBPX);
  int bb_maxy = (int)((maxy_q + 15) >> RASTER_SUBPX);

  // Intersect with this tile's screen pixel rect.
  int const tile_minx = tile_px_x0;
  int const tile_miny = tile_px_y0;
  int const tile_maxx = tile_px_x0 + RDR_TILE_W;  // exclusive
  int const tile_maxy = tile_px_y0 + RDR_TILE_H;  // exclusive
  bb_minx = clampi(bb_minx, tile_minx, tile_maxx);
  bb_miny = clampi(bb_miny, tile_miny, tile_maxy);
  bb_maxx = clampi(bb_maxx, tile_minx, tile_maxx);
  bb_maxy = clampi(bb_maxy, tile_miny, tile_maxy);

  for (int sy = bb_miny; sy < bb_maxy; ++sy) {
    fx12_4 const cy = (fx12_4)((sy << RASTER_SUBPX) + RASTER_HALF);
    for (int sx = bb_minx; sx < bb_maxx; ++sx) {
      fx12_4 const cx = (fx12_4)((sx << RASTER_SUBPX) + RASTER_HALF);
      // Barycentric edge functions: w_i opposite vertex i.
      int32_t const w0 = edge_eval(t->x1, t->y1, t->x2, t->y2, cx, cy);
      int32_t const w1 = edge_eval(t->x2, t->y2, t->x0, t->y0, cx, cy);
      int32_t const w2 = edge_eval(t->x0, t->y0, t->x1, t->y1, cx, cy);
      // Inside iff each edge is strictly positive, or zero on a top-left edge.
      // w0 lies on edge v1->v2 (tl1); w1 on v2->v0 (tl2); w2 on v0->v1 (tl0).
      int const in0 = (w0 > 0) || (w0 == 0 && t->tl1);
      int const in1 = (w1 > 0) || (w1 == 0 && t->tl2);
      int const in2 = (w2 > 0) || (w2 == 0 && t->tl0);
      if (!(in0 && in1 && in2)) {
        continue;
      }
      // Interpolate 1/w: perspective-correct in 1/w is linear in screen space.
      // inv_w = (w0*iw0 + w1*iw1 + w2*iw2) / area2. Numerator: w_i (Q24.8) *
      // iw (Q16.16) -> use 64-bit to avoid overflow, divide by area2 (Q24.8) to
      // recover Q16.16.
      int64_t const num = ((int64_t)w0 * (int64_t)t->iw0) +
                          ((int64_t)w1 * (int64_t)t->iw1) +
                          ((int64_t)w2 * (int64_t)t->iw2);
      int32_t const inv_w = (int32_t)(num / (int64_t)t->area2);

      int const tile_x = sx - tile_px_x0;
      int const tile_y = sy - tile_px_y0;
      int const zi = (tile_y * RDR_TILE_W) + tile_x;
      uint16_t const znew = depth_pack(inv_w);
      // w-buffer: larger inv_w is closer. Pass if at least as close as stored.
      if (znew < zbuf[zi]) {
        continue;
      }
      zbuf[zi] = znew;
      fb[(sy * RDR_SCREEN_W) + sx] = t->color;
    }
  }
}

void raster_tile(int tile, const struct TileBin* bin, const struct TVtx* pool,
                 uint16_t* fb, uint16_t* zbuf) {
  if (bin == 0 || pool == 0 || fb == 0 || zbuf == 0) {
    return;
  }
  // Per-tile Z clear (spec: clear Z scratch at the start of each tile).
  for (int i = 0; i < RDR_TILE_W * RDR_TILE_H; ++i) {
    zbuf[i] = RASTER_Z_CLEAR;
  }

  // Tile grid is row-major; compute this tile's top-left screen pixel.
  int const tiles_per_row = RDR_SCREEN_W / RDR_TILE_W;
  int const tile_px_x0 = (tile % tiles_per_row) * RDR_TILE_W;
  int const tile_px_y0 = (tile / tiles_per_row) * RDR_TILE_H;

  for (uint32_t i = 0; i < bin->count; ++i) {
    const struct TriRef* ref = &bin->refs[i];
    struct TriSetup t;
    int const rc =
        tri_setup(&t, &pool[ref->v0], &pool[ref->v1], &pool[ref->v2]);
    if (rc != RDR_OK) {
      continue;  // degenerate -> rejected, no fill
    }
    raster_one(&t, tile_px_x0, tile_px_y0, fb, zbuf);
  }
}
