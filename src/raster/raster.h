// raster.h — module interface (Stream B.1-gamma: raster — flat fill + per-tile
// Z). Orthodox C++ (POD + free functions, C headers, no STL/auto/exceptions).
//
// Half-space edge-function rasterizer clipped to tile bounds. Per-material
// (R.1): a flat-color fast path (provoking-vertex rgba, untextured materials)
// or a textured + Gouraud + combiner path (textured materials) with a per-tile
// 1/w (w-buffer) depth test + write. The flat fast path is BIT-IDENTICAL to the
// pre-R.1 rasterizer (the hard regression gate). R.1 = OPAQUE pass only.
//
// Conventions (source of truth: docs/superpowers/specs design + tests/harness
// oracle_fill_tri):
//   - Screen coords are Q12.4 (fx12_4): 16 subpixel steps per pixel. Pixel
//     centers are sampled (the +0.5px = +8 subpixel offset is folded into the
//     edge constants at setup).
//   - Screen Y is down (row 0 at top), matching the framebuffer/scanout.
//   - Top-left fill rule tie-break: a pixel center exactly on a shared edge
//     belongs to exactly one triangle (no double-cover, no gap). Winding is
//     normalized to positive screen-space area, then top/left edges are
//     inclusive (matching the oracle convention).
//   - Depth is 1/w (w-buffer): a LARGER inv_w is CLOSER. A fragment passes the
//     Z test iff its interpolated inv_w >= the stored value; on pass it writes
//     both color and inv_w (opaque pass semantics).
//   - Degenerate triangles (|2*area| <= epsilon, in Q12.4 area units) are
//     rejected at setup — no div-by-zero in the 1/area reciprocal.
//
// Buffer ownership / layout:
//   - `fb`   : full-screen RGB565 framebuffer, RDR_SCREEN_W*RDR_SCREEN_H,
//              row-major, native-endian uint16 (gfx/framebuffer.h color_t).
//   - `zbuf` : per-tile depth scratch, RDR_TILE_W*RDR_TILE_H uint16, row-major
//              within the tile. raster_tile CLEARS it on entry (to "farthest",
//              i.e. 0 == inv_w of 0 == infinitely far) per the spec's per-tile
//              clear step.
//   - `pool` : the transformed-vertex pool; TriRef.v0/v1/v2 index into it.
#ifndef RDR_RASTER_H
#define RDR_RASTER_H
#include "rdr/types.h"

// Z scratch sentinel for "nothing drawn yet / farthest" (inv_w == 0 -> w =
// inf).
#define RASTER_Z_CLEAR ((uint16_t)0)

// Rasterize every triangle binned to `tile` into `fb`, depth-tested + written
// against the per-tile `zbuf`. `tile` is the linear tile index (row-major over
// the RDR_SCREEN_W/RDR_TILE_W by RDR_SCREEN_H/RDR_TILE_H tile grid). `zbuf` is
// cleared to RASTER_Z_CLEAR on entry. Triangles whose refs fall outside the
// tile's pixel rect contribute nothing (edge functions clip to tile bounds).
//
// `rstate_table` is the per-frame interned render-state table (Frame.
// rstate_table); each triangle's TriRef.material indexes it. The selected
// RenderState routes the per-pixel path (see R.1/R.2 in raster.cc):
//   - NO valid texture (tex.data==0 || tex.w==0 || tex.h==0) -> flat-fill fast
//     path (provoking-vertex color), BIT-IDENTICAL to the pre-R.1 rasterizer.
//   - valid texture -> textured + Gouraud + (A-B)*C+D combiner path
//     (perspective-correct UV, affine SHADE, optional alpha-cutout discard).
//
// R.2 TWO-PASS (per tile, internal to raster_tile_noclear): the refs are swept
// TWICE over the SAME bin:
//   - SWEEP 1 (OPAQUE): every ref whose material zmode != ZMODE_XLU
//     (ZMODE_OPAQUE and ZMODE_DECAL — DECAL treated as opaque for now)
//     rasterized exactly as R.1 (z-test + z-write). BIT-IDENTICAL to the pre-
//     R.2 single loop for any opaque-only bin (the hard regression gate).
//   - SWEEP 2 (TRANSLUCENT): the ZMODE_XLU refs gathered, sorted BACK-TO-FRONT
//     (ascending inv_w = farthest first; deterministic bin-order tie-break),
//     then composited z-TEST-only (NO z-write) with texel-alpha alpha-over
//     (blend_pixel_alpha(BLEND_ALPHA, combiner_out, texel.a, fb)). P3-3: XLU
//     blends on TEXEL0 alpha, not coverage.
// The sort is deterministic + each tile is drawn by exactly one worker, so the
// dual-core sweep stays bit-identical to serial. `rstate_table` is read-only
// here (immutable during raster -> the dual-core bit-identical invariant
// holds).
void raster_tile(int tile, const struct TileBin* bin, const struct TVtx* pool,
                 uint16_t* fb, uint16_t* zbuf,
                 const struct RenderState* rstate_table);

// Same as raster_tile but DOES NOT clear `zbuf` on entry — it rasterizes
// directly against whatever depths are already present. raster_tile is exactly
// {clear zbuf; raster_tile_noclear}. Intended for the dual-core depth-hazard
// demonstration (tests/sched) which drives the real rasterizer over a shared,
// un-recleared scratch to prove per-worker zbuf isolation is load-bearing.
// Production callers want raster_tile (the per-tile clear is part of the spec).
void raster_tile_noclear(int tile, const struct TileBin* bin,
                         const struct TVtx* pool, uint16_t* fb, uint16_t* zbuf,
                         const struct RenderState* rstate_table);

// R.2 DEBUG telemetry: total XLU triangles dropped because a single tile's
// translucent gather exceeded the fixed per-tile XLU sort cap (drop-with-count,
// mirroring TileBin.dropped — never corrupt, surface the count). NEVER feeds
// the framebuffer path; a nonzero value is a re-surface signal that the per-
// tile XLU cap needs raising. Not atomic across raster workers (debug only).
uint32_t raster_xlu_dropped(void);

#endif  // RDR_RASTER_H
