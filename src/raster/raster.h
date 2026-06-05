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

// L6 XLU SATURATION EARLY-OUT THRESHOLD (the overdraw lever's knob). The
// front-to-back translucent sweep accumulates premultiplied UNDER into a
// per-tile accumulator; once a pixel's accumulated alpha reaches RASTER_XLU_SAT
// the pixel is "opaque enough" and every FARTHER fragment is skipped (it would
// contribute ~0 through the residual transmittance 255-acc_alpha) — eliding all
// of that fragment's shade/texture/combiner/fog work. This skip is the win that
// recovers the measured 6.12x XLU overdraw.
//
// DEFAULT 255, chosen from the L6 A/B fidelity+win sweep (the scripted 480-
// frame loop), NOT a blind maximize:
//   - 255 is FREE: skipping a pixel whose accumulated alpha == 255 is a
//     mathematical no-op (the fragment's effective contribution alpha
//     ea = round(a * (255-255)/255) = 0), so the image is BYTE-IDENTICAL to
//     pure reorder (256) — verified: full-loop fb_crc 0xcad13160 at BOTH 255
//     and 256 — yet it still skips ~22.5% of z-passing XLU fragments.
//   - going LOWER barely grows the skip (252 -> 22.8%, 248 -> 23.0%, 240 ->
//     23.2%: the soft foliage's deep core saturates to exactly 255 and stops),
//     while it DOES degrade the soft-tree edges (248: PSNR ~70 dB / ~0.1% px vs
//     reorder; 240: PSNR ~60 dB / ~0.3% px). The diminishing ~0.7% extra skip
//     is not worth the visible tail loss.
// So 255 is the Pareto point: the entire achievable overdraw win with ZERO
// fidelity cost. Tunable; not a frozen contract value. Overridable at compile
// time (-DRASTER_XLU_SAT=N). Compared with >= against a uint8 accumulated
// alpha, so the literal 256 (> any uint8) is the "never trigger" sentinel
// (pure reorder, for re-running the A/B).
#ifndef RASTER_XLU_SAT
#define RASTER_XLU_SAT 255
#endif

// Build-wide raster-worker fan-out — sizes the per-worker L6 XLU accumulator
// slices in raster.cc and bounds the `worker` id threaded into raster_tile.
// MUST equal frame.h RDR_NUM_RASTER_WORKERS (which sizes the per-worker
// zbuf/cov the same `worker` indexes); a static_assert in src/sched/drain.h
// (the one TU that sees both headers) pins them. Defined here, not in
// raster.cc, so the assert can reference it without raster.cc pulling in the
// heavy frame.h aggregate (a layering inversion). Must stay a power of two (the
// slice index is masked with RASTER_NUM_WORKERS-1 for the static analyzer).
#define RASTER_NUM_WORKERS 2

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
// TWO-PASS (per tile, internal to raster_tile_noclear): the refs are swept
// TWICE over the SAME bin:
//   - SWEEP 1 (OPAQUE): every ref whose material zmode != ZMODE_XLU
//     (ZMODE_OPAQUE and ZMODE_DECAL — DECAL treated as opaque for now)
//     rasterized exactly as R.1 (z-test + z-write). BIT-IDENTICAL to the pre-
//     R.2 single loop for any opaque-only bin (the hard regression gate).
//   - SWEEP 2 (TRANSLUCENT, L6 FRONT-TO-BACK premultiplied UNDER): the
//   ZMODE_XLU
//     refs are gathered, then STABLE-sorted FRONT-TO-BACK (DESCENDING inv_w =
//     NEAREST first; deterministic bin-order tie-break). Each fragment z-TESTS
//     against the sweep-1 opaque depth (NO z-write), and — unless this pixel's
//     accumulated alpha already reached RASTER_XLU_SAT (the SATURATION
//     EARLY-OUT that skips the fragment's shade/texture/combiner/fog, the
//     overdraw win) — accumulates the FOGGED combiner color PREMULTIPLIED by
//     its TEXEL0 alpha (P3-3: XLU weights on TEXEL0 alpha, not coverage) into a
//     per-worker per-tile accumulator (premultiplied RGB565 color + 8-bit
//     accumulated alpha; selected by the `worker` id, cleared per tile). After
//     all XLU tris, a COMPOSITE pass folds the opaque terrain UNDER the
//     accumulation ONCE per touched pixel: fb = c_acc +
//     round((255-acc_alpha)*fb_terrain/255), and stamps cov=AA_COV_FULL there.
//     Pixels with no XLU contribution (acc_alpha
//     == 0) are LEFT UNTOUCHED, so an opaque-only tile is byte-identical to the
//     pre-L6 renderer (the hard regression gate). This front-to-back UNDER is
//     the correct operator: terrain is scaled by the residual transmittance
//     ONCE, never over-brightened by accumulating "under" pre-existing terrain.
// The sort is deterministic + each tile is drawn by exactly one worker into ITS
// OWN accumulator slice, so the dual-core sweep stays bit-identical to serial.
// `rstate_table` is read-only here (immutable during raster -> the dual-core
// bit-identical invariant holds).
//
// `worker` is the raster-worker id [0,RDR_NUM_RASTER_WORKERS): it selects the
// per-worker accumulator slice exactly as it selects this worker's `zbuf`/`cov`
// scratch at the call site (sched_drain_tile passes f->zbuf[worker] etc.). One
// independent slice per worker is what keeps the dual-core sweep bit-identical
// to serial.
//
// R.3-AA COVERAGE (nullable `cov`): when `cov != 0` AND aa_enabled(), each
// winning OPAQUE fragment writes an analytic per-pixel coverage byte [0,255]
// (AA_COV_FULL=255 = full/interior) into `cov` (tile-local row-major, same
// layout as `zbuf`); raster_tile clears `cov` to AA_COV_FULL on entry (like
// zbuf). The L6 XLU COMPOSITE pass stamps AA_COV_FULL on every composited pixel
// (AA ignores translucent, plan v1). When `cov == 0` OR AA is disabled the
// coverage path is COMPLETELY SKIPPED -> the rasterizer is BYTE-IDENTICAL to
// the pre-AA renderer (the flat fast path especially — the hard regression
// gate). raster_tile then calls aa_resolve_tile (within-tile, border-clamped)
// at its END when `cov` is active.
void raster_tile(int tile, const struct TileBin* bin, const struct TVtx* pool,
                 uint16_t* fb, uint16_t* zbuf,
                 const struct RenderState* rstate_table, uint8_t* cov,
                 int worker);

// Same as raster_tile but DOES NOT clear `zbuf` on entry — it rasterizes
// directly against whatever depths are already present. raster_tile is exactly
// {clear zbuf; raster_tile_noclear}. Intended for the dual-core depth-hazard
// demonstration (tests/sched) which drives the real rasterizer over a shared,
// un-recleared scratch to prove per-worker zbuf isolation is load-bearing.
// Production callers want raster_tile (the per-tile clear is part of the spec).
// `cov` is the nullable R.3-AA per-tile coverage scratch (see raster_tile); a
// null `cov` / disabled AA skips coverage entirely. `worker` selects the
// per-worker XLU accumulator slice (see raster_tile). NOTE: unlike raster_tile
// this does NOT clear `cov` (mirrors the no-zbuf-clear contract) and does NOT
// run aa_resolve_tile; it DOES clear its own per-worker XLU accumulator at
// sweep-2 start (the accumulator must never leak across tiles).
void raster_tile_noclear(int tile, const struct TileBin* bin,
                         const struct TVtx* pool, uint16_t* fb, uint16_t* zbuf,
                         const struct RenderState* rstate_table, uint8_t* cov,
                         int worker);

// R.2 DEBUG telemetry: MONOTONIC count of XLU triangles dropped because some
// tile's translucent gather exceeded the fixed per-tile XLU sort cap. Mirrors
// TileBin.dropped's drop-POLICY (never corrupt, surface the count) but is NOT a
// per-frame count — TileBin.dropped is reset each frame by geom; this counter
// is process-lifetime and never reset (no frame hook in the raster lane). Read
// it as a DELTA (before/after a render), not an absolute. NEVER feeds the
// framebuffer path; a rising value is a re-surface signal that the per-tile XLU
// cap needs raising. Not atomic across raster workers (debug only).
uint32_t raster_xlu_dropped(void);

#endif  // RDR_RASTER_H
