// raster.cc — half-space edge-function rasterizer. Per-material: flat-fill fast
// path (BIT-IDENTICAL to B.1-gamma) OR textured + Gouraud + combiner path
// (R.1), then a two-pass split (R.2): an OPAQUE sweep (sweep 1, unchanged from
// R.1) followed by a front-to-back-sorted TRANSLUCENT sweep (sweep 2, T5/L6:
// z-test only, premultiplied-UNDER accumulation + saturation early-out, then a
// per-tile composite that folds the opaque terrain under). Stream B.1-gamma +
// R.1 + R.2 + T5/L6. Orthodox C++:
// POD + free functions, C headers, C-style casts, no STL/auto/lambda/templates/
// exceptions. See raster.h for contract.
#include "raster/raster.h"

#include <stdint.h>
#include <string.h>

#include "aa/aa.h"        // AA_COV_FULL, aa_enabled, aa_resolve_tile
#include "blend/blend.h"  // blend_premul_accumulate/_resolve (XLU UNDER), fog_lerp
#include "gfx/framebuffer.h"  // rgb565()
#include "prof/prof.h"  // T5: PROF_RASTER_TILE + FINE sweep blocks (no-op when off)
#include "raster/interp.h"  // T5 L1: exact-integer DDA interpolant stepper
#include "rdr/config.h"
#include "rdr/sram.h"     // __not_in_flash_func (SRAM placement; no-op on host)
#include "shade/shade.h"  // shade_pixel (color combiner)
#include "tex/tex.h"      // tex_sample / tex_sample_rgba

// ---- subpixel / depth constants --------------------------------------------
// Q12.4 screen coords: 16 (=2^4) subpixel steps per pixel. Pixel-center sample
// is integer-pixel + 0.5px = +8 subpixel units.
enum { RASTER_SUBPX = 4, RASTER_HALF = 8 };

// Degenerate reject: |2*area| in Q12.4 area units. 2*area is the cross product
// of two Q12.4 edge vectors -> Q24.8; one full subpixel-cell of area is 16*16 =
// 256. Reject anything not strictly larger than that (a sliver < 1 px area
// carries no reliable winding / would blow up the 1/area reciprocal).
enum { RASTER_AREA_EPS = 256 };

// ---- R.3-AA analytic coverage (the WRITE) ---------------------------------
// DERIVATION (mirror of the R.1 UV-derivation comment style; integer-only ->
// host<->device bit-identical, P3-5):
//   The edge function w_i = edge_eval(a,b,p) is the signed cross product of two
//   Q12.4 vectors -> Q24.8. It equals 2*area of the (a,b,p) sub-triangle, so
//   the SIGNED perpendicular distance (in PIXELS) of p to edge a->b is
//     d_i = w_i / (2 * area_unit / len_px)  =  w_i / (256 * len_px),
//   because |grad w| w.r.t. a Q12.4 position step is the edge length and the
//   Q24.8 scale is 256. With len_px = len_q124 / 16 (the edge length measured
//   in Q12.4 units divided back to pixels):
//     d_i (pixels) = w_i / (256 * len_q124 / 16) = w_i / (16 * len_q124).
//   Coverage = clamp(0.5 + min(d0,d1,d2), 0, 1) (oracle_coverage). As a byte,
//   FULL(255) = covered, 128 ~ a centre exactly on an edge (0.5):
//     cov255 = clamp(128 + round(256 * d_min), 0, 255),
//   and  256 * d_i = 256 * w_i / (16 * len_q124) = 16 * w_i / len_q124.
//   To avoid a per-pixel divide we PRECOMPUTE per edge (constant over the tri)
//   a reciprocal  recip_i = round(2^AA_RECIP_SHIFT / len_q124)  in tri_setup,
//   then per pixel:
//     d256_i = (16 * (int64)w_i * recip_i + ROUND) >> AA_RECIP_SHIFT  (=
//     256*d_i) cov255 = clamp(128 + d256_min, 0, 255).
//   len_q124 = isqrt(dx*dx + dy*dy), dx/dy the edge's Q12.4 deltas. Coords are
//   guard-band clipped (clip.h RDR_GUARD_X/Y = 2*screen = +/-480 px -> ~7680
//   Q12.4), so an edge delta reaches ~15360 and dx*dx+dy*dy ~5e8 -> still well
//   within int32 (< 2^31), and the isqrt is exact. 16*w*recip fits int64
//   comfortably. AA_RECIP_SHIFT=22: 2^22/len has ~22 bits of headroom even for
//   a 1px edge (len_q124~16 -> recip ~262144); precision near the edge (small
//   w) is the only regime that matters since interiors saturate to 255.
enum { AA_RECIP_SHIFT = 22 };

// Integer sqrt (floor) of a non-negative int32 via Newton-free bit search. Used
// once per edge in tri_setup (not per pixel) so cost is irrelevant; exact for
// the bounded edge-length-squared input.
static int32_t isqrt_i32(int32_t v) {
  if (v <= 0) {
    return 0;
  }
  int32_t bit = 1 << 30;
  while (bit > v) {
    bit >>= 2;
  }
  int32_t res = 0;
  while (bit != 0) {
    if (v >= res + bit) {
      v -= res + bit;
      res = (res >> 1) + bit;
    } else {
      res >>= 1;
    }
    bit >>= 2;
  }
  return res;
}

// Per-edge gradient-magnitude reciprocal recip = round(2^AA_RECIP_SHIFT /
// len_q124), len_q124 = |edge| in Q12.4 units. Degenerate (len 0) edges already
// imply a degenerate tri (rejected at setup); guard returns 0 -> that edge
// contributes d=0 conservatively (cannot happen for an accepted tri).
static int32_t edge_recip(fx12_4 ax, fx12_4 ay, fx12_4 bx, fx12_4 by) {
  int32_t const dx = (int32_t)bx - (int32_t)ax;
  int32_t const dy = (int32_t)by - (int32_t)ay;
  int32_t const len = isqrt_i32((dx * dx) + (dy * dy));
  if (len <= 0) {
    return 0;
  }
  return (int32_t)(((int64_t)1 << AA_RECIP_SHIFT) / (int64_t)len);
}

// Analytic coverage byte at a pixel from the 3 edge functions (Q24.8) and the
// precomputed per-edge reciprocals. cov255 = clamp(128 + 256*min(d_i), 0, 255).
// Integer-only (P3-5 parity). ROUND = half an AA_RECIP_SHIFT step.
static uint8_t cov_byte(int32_t w0, int32_t w1, int32_t w2, int32_t r0,
                        int32_t r1, int32_t r2) {
  int64_t const round = (int64_t)1 << (AA_RECIP_SHIFT - 1);
  int32_t const d0 =
      (int32_t)((((int64_t)16 * (int64_t)w0 * (int64_t)r0) + round) >>
                AA_RECIP_SHIFT);
  int32_t const d1 =
      (int32_t)((((int64_t)16 * (int64_t)w1 * (int64_t)r1) + round) >>
                AA_RECIP_SHIFT);
  int32_t const d2 =
      (int32_t)((((int64_t)16 * (int64_t)w2 * (int64_t)r2) + round) >>
                AA_RECIP_SHIFT);
  int32_t dmin = d0;
  if (d1 < dmin) {
    dmin = d1;
  }
  if (d2 < dmin) {
    dmin = d2;
  }
  int32_t cov = 128 + dmin;
  if (cov < 0) {
    cov = 0;
  }
  if (cov > 255) {
    cov = 255;
  }
  return (uint8_t)cov;
}

// R.2 XLU per-tile sort scratch cap (the SIZING decision, re-surface trigger).
// Sweep 2 gathers this tile's XLU TriRef BIN INDICES into a FIXED local
// uint16_t array (2 B/entry), then STABLE-sorts them back-to-front. A FULL
// per-tile cap (bins draw from RDR_BIN_POOL_REFS=2048; a hot tile can hold
// hundreds of refs) at 2 B/entry would be ~4 KB — that BLOWS the RP2040 core1
// stack (SDK default PICO_CORE1_STACK_SIZE = 0x800 = 2 KB, no project override;
// the dual-core raster runs ON the core stacks). So cap the gather at a MODEST
// 256 -> 256 * 2 B = 512 B on stack (comfortable; the rest of the chain is
// TriSetup ~80 B + raster_one[_xlu] + dispatch frames, all well under 2 KB) and
// DROP-WITH-COUNT beyond. The XLU footprint is the tree FOLIAGE (the scene's
// only translucent geometry, ~252 tree tris TOTAL across the 16 screen-tiles at
// terrain density), so 256 XLU tris in ONE 60x60 tile is far beyond realistic
// foliage density -> the real scene never drops; a drop is a re-surface signal,
// not steady state. (NOTE: the array holds only indices, NOT a key struct — an
// int64-depth key struct would be 16 B-aligned = 4 KB at 256, the very overflow
// this cap exists to avoid; the depth key is recomputed inline during the
// sort.)
enum { RASTER_XLU_TILE_CAP = 256 };

// Drop-with-count for the XLU gather overflow (file-scope static, surfaced via
// the accessor below). Encapsulated state reached only through module
// functions (CLAUDE.md: file-scope static behind accessors == encapsulation).
// It mirrors TileBin's drop POLICY (never corrupt; surface the count) but is
// NOT a per-frame count like TileBin.dropped (which geom resets every frame):
// this is a MONOTONIC "the per-tile XLU cap was ever exceeded since boot"
// signal. There is no frame hook in the raster lane to reset it (and adding one
// is out of scope), so a consumer reading it must take a DELTA, not an
// absolute. NOTE: not atomic. The dual-core raster partitions tiles disjointly
// across cores, but BOTH cores can bump this counter for their own tiles -> a
// racy increment could lose a count. DEBUG telemetry only (never feeds the bit-
// identical framebuffer path); the framebuffer output stays deterministic
// regardless (each tile is drawn by exactly one worker; the drop is per-tile).
static uint32_t s_xlu_dropped = 0;

#if PROFILER && PROF_OVERDRAW
// T5 overdraw probe (PROFILER + PROF_OVERDRAW only): per-WORKER, per-tile,
// per-pixel count of XLU fragments that pass the z-test, reset per tile. Used
// to transition-count distinct touched pixels (0->1) and pixels reaching
// depth>=2 (1->2) so the report can derive mean XLU overdraw (=
// frags/distinct). Gated behind PROF_OVERDRAW (default OFF) because this ~7.2KB
// per-worker array does not fit alongside L6's accumulators in the PROFILER=ON
// pico build; overdraw is already characterized (6.12x), so it is opt-in.
// PROFILER=0 erases it too -> golden bit-identical.
static uint8_t g_prof_xlu_depth[PROF_NUM_CORES][RDR_TILE_W * RDR_TILE_H];
#endif

// L6 FRONT-TO-BACK XLU ACCUMULATORS (the overdraw lever) ---------------------
// Per-tile premultiplied-UNDER accumulation scratch, ONE independent slice per
// raster worker (RASTER_NUM_WORKERS, == frame.h RDR_NUM_RASTER_WORKERS). The
// front-to-back sweep clears these to zero at sweep-2 start, accumulates each
// XLU fragment in, then the final composite folds terrain under and writes fb.
//
// CONTRACT-BARRIER NOTE (frame.h, flagged for the Lead): the spec called for
// these as `struct Frame` members (C_acc/acc_alpha) plumbed through
// raster_tile/raster_tile_noclear like zbuf/cov. That plumbing is BLOCKED for
// this stream: raster_tile's parameter list is Q1-owned (the
// __not_in_flash_func wrap) and the one call site (src/sched/drain.h) is not in
// this lane's grant — so a Frame member cannot be reached without edits this
// stream may not make. The accumulators are PURE within-tile scratch (cleared,
// filled, and consumed entirely inside ONE raster_tile_noclear call; never read
// across calls), so file-scope-static-per-worker storage is the exact same
// encapsulation pattern as g_prof_xlu_depth above and the per-worker zbuf/cov
// in Frame — and it preserves the dual==serial invariant identically (each
// worker writes only its OWN slice, selected by the `worker` id THREADED into
// raster_tile/raster_tile_noclear — exactly how zbuf[worker]/cov[worker] are
// selected by sched_drain_tile, NOT a separate get_core_num() mechanism). The
// SRAM cost is the SAME wherever it lives; documenting it here (and in
// frame.h's budget comment) gives the Lead the contract delta without the
// un-editable plumbing. If the Lead prefers a Frame member, it's a 2-line
// signature change
// + the drain.h call site, reconciled at the barrier merge after Q1 lands.
//
// SIZES: C_acc 565 = RASTER_NUM_WORKERS * 3600 * 2 B = 14.4 KB; acc_alpha =
// NW * 3600 * 1 B = 7.2 KB. Total ~21.6 KB on device. 565 (not 888) is
// MANDATORY for the SRAM budget. RASTER_NUM_WORKERS (raster.h) is the
// build-wide raster fan-out; src/sched/drain.h static_asserts it == frame.h
// RDR_NUM_RASTER_WORKERS so the threaded `worker` id indexes both alike.
static uint16_t g_xlu_c_acc[RASTER_NUM_WORKERS][RDR_TILE_W * RDR_TILE_H];
static uint8_t g_xlu_acc_alpha[RASTER_NUM_WORKERS][RDR_TILE_W * RDR_TILE_H];

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
  // Per-vertex texcoord*inv_w (S10.5 * inv_w_real, truncated as geom_project
  // births TVtx.u_iw/v_iw). int32 (widened from the int16 contract field) so
  // the affine interp numerator does not overflow before the divide.
  int32_t u_iw0, u_iw1, u_iw2;
  int32_t v_iw0, v_iw1, v_iw2;
  // Per-vertex packed shade color (RGB565), post winding-normalize. Affine
  // (screen-linear) Gouraud interpolation unpacks these in-loop.
  uint16_t rgba0, rgba1, rgba2;
  // Per-vertex fog factor [0,255] (TVtx.fog), post winding-normalize. R.3:
  // interpolated AFFINE (screen-linear, NOT perspective-corrected — matches the
  // N64 per-vertex fog convention, same path as Gouraud SHADE) and applied
  // post-combiner (fog_lerp toward rstate.fog.color). 0 -> no fog (no-op).
  uint8_t fog0, fog1, fog2;
  int32_t area2;      // 2*signed area (Q24.8), > 0 after normalize
  int tl0, tl1, tl2;  // top-left flags for edges v0->v1, v1->v2, v2->v0
  // R.3-AA: per-edge gradient-magnitude reciprocal (round(2^AA_RECIP_SHIFT /
  // edge_len_q124)), matched to w0/w1/w2: r0 <-> edge v1->v2 (w0), r1 <-> edge
  // v2->v0 (w1), r2 <-> edge v0->v1 (w2). Computed once here (constant per
  // tri); only read in the coverage WRITE path (AA on).
  int32_t r0, r1, r2;
  uint16_t color;  // flat fill (provoking vertex v0)
};

// Unpack an RGB565 color to 8-bit RGB (matches gfx/framebuffer.h rgb565 layout:
// R[15:11] G[10:5] B[4:0], bit-replicated to 8 bits — same as shade.cc's
// unpack565 and oracle_unpack565).
static void unpack565(uint16_t c, uint8_t* r, uint8_t* g, uint8_t* b) {
  uint8_t const r5 = (uint8_t)((c >> 11) & 0x1F);
  uint8_t const g6 = (uint8_t)((c >> 5) & 0x3F);
  uint8_t const b5 = (uint8_t)(c & 0x1F);
  *r = (uint8_t)((r5 << 3) | (r5 >> 2));
  *g = (uint8_t)((g6 << 2) | (g6 >> 4));
  *b = (uint8_t)((b5 << 3) | (b5 >> 2));
}

// True iff this render state has a sampleable texture. A null/zero-dim TexDesc
// is "no texture" -> the flat-fill fast path (preserving bit-identical output).
static int rs_has_texture(const struct RenderState* rs) {
  if (rs == 0) {
    return 0;
  }
  return rs->tex.data != 0 && rs->tex.w != 0 && rs->tex.h != 0;
}

// R.4 — true iff this render state requests the 2-cycle detail multitexture
// path: cycle == COMBINE_TWO_CYCLE AND tex1 is a sampleable (valid) descriptor.
// When false (the DEFAULT: cycle == COMBINE_ONE_CYCLE, or tex1 null/zero-dim)
// the per-pixel path is EXACTLY the pre-R.4 shade_pixel path -> byte-identical
// output (the hard regression gate). The base TEXEL0 validity is checked
// separately (rs_has_texture); a flat material never reaches the textured path.
static int rs_two_cycle(const struct RenderState* rs) {
  if (rs == 0) {
    return 0;
  }
  if (rs->cycle != COMBINE_TWO_CYCLE) {
    return 0;
  }
  return rs->tex1.data != 0 && rs->tex1.w != 0 && rs->tex1.h != 0;
}

// R.4 — sample the detail texture (TEXEL1) at the base Q16.16 texel coord
// left-shifted by detail_shift (tiles the small detail tex at a higher
// frequency). Δ4 WATCH: detail_shift left-shifts the Q16.16 texel coord, not
// the u_iw numerator, so the worst case is u_q16 << shift; tex.cc wraps via a
// pow2 mask AFTER flooring (q>>16), so even a large product is masked into
// range — no |u_iw|-style affine-numerator overflow here. The shift EXPONENT is
// masked to [0,31] (detail_shift is a uint8 with no upper bound in types.h; a
// shift >= 32 is C++ shift-exponent UB — UBSan would trip it). A shift >= 32 is
// nonsensical anyway (it pushes every integer texel bit out of a 32-bit Q16.16
// coord); the mask keeps it defined. Honors tex1.filter/wrap.
static uint16_t sample_texel1(const struct RenderState* rs, fx16_16 u_q16,
                              fx16_16 v_q16) {
  int const sh = (int)rs->detail_shift & 31;
  fx16_16 const u1 = (fx16_16)((int32_t)u_q16 << sh);
  fx16_16 const v1 = (fx16_16)((int32_t)v_q16 << sh);
  return tex_sample(&rs->tex1, u1, v1, 0);
}

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
  // Per-vertex texcoord*inv_w + packed shade, widened from the int16 contract.
  int32_t const uiw0 = (int32_t)a->u_iw;
  int32_t uiw1 = (int32_t)b->u_iw;
  int32_t uiw2 = (int32_t)c->u_iw;
  int32_t const viw0 = (int32_t)a->v_iw;
  int32_t viw1 = (int32_t)b->v_iw;
  int32_t viw2 = (int32_t)c->v_iw;
  uint16_t const rg0 = a->rgba;
  uint16_t rg1 = b->rgba;
  uint16_t rg2 = c->rgba;
  // Per-vertex fog factor [0,255], widened to int32 for the lockstep swap.
  int32_t const fg0 = (int32_t)a->fog;
  int32_t fg1 = (int32_t)b->fog;
  int32_t fg2 = (int32_t)c->fog;

  int32_t area2 = edge_eval(x0, y0, x1, y1, x2, y2);
  int32_t const mag = (area2 < 0) ? -area2 : area2;
  if (mag <= RASTER_AREA_EPS) {
    return RDR_EDEGENERATE;
  }
  if (area2 < 0) {
    // Swap v1<->v2 to normalize to positive area. CRITICAL: EVERY per-vertex
    // attribute (position, iw, u_iw, v_iw, rgba, fog) MUST swap in lockstep — a
    // transposition here is the classic textured-raster bug (R.1 re-surface
    // trigger). Keep this block exhaustive.
    fx12_4 const tx = x1;
    fx12_4 const ty = y1;
    int32_t const tiw = iw1;
    int32_t const tuiw = uiw1;
    int32_t const tviw = viw1;
    uint16_t const trg = rg1;
    int32_t const tfg = fg1;
    x1 = x2;
    y1 = y2;
    iw1 = iw2;
    uiw1 = uiw2;
    viw1 = viw2;
    rg1 = rg2;
    fg1 = fg2;
    x2 = tx;
    y2 = ty;
    iw2 = tiw;
    uiw2 = tuiw;
    viw2 = tviw;
    rg2 = trg;
    fg2 = tfg;
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
  t->u_iw0 = uiw0;
  t->u_iw1 = uiw1;
  t->u_iw2 = uiw2;
  t->v_iw0 = viw0;
  t->v_iw1 = viw1;
  t->v_iw2 = viw2;
  t->rgba0 = rg0;
  t->rgba1 = rg1;
  t->rgba2 = rg2;
  t->fog0 = (uint8_t)fg0;
  t->fog1 = (uint8_t)fg1;
  t->fog2 = (uint8_t)fg2;
  t->area2 = area2;
  t->tl0 = edge_is_top_left(x0, y0, x1, y1);
  t->tl1 = edge_is_top_left(x1, y1, x2, y2);
  t->tl2 = edge_is_top_left(x2, y2, x0, y0);
  // R.3-AA per-edge gradient reciprocals (constant per tri), matched to the
  // edge each w_i evaluates: w0=edge(v1,v2), w1=edge(v2,v0), w2=edge(v0,v1).
  t->r0 = edge_recip(x1, y1, x2, y2);
  t->r1 = edge_recip(x2, y2, x0, y0);
  t->r2 = edge_recip(x0, y0, x1, y1);
  t->color = a->rgba;  // flat fast path: provoking vertex (v0), pre-swap
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

// Recover the perspective-correct texel coordinate (Q16.16 texel space) at a
// pixel, from the affinely-interpolated num_uiw (= S10.5*inv_w_real) and the
// pixel's interpolated inv_w (Q16.16). DERIVATION (verified against
// oracle_sample_texel; see tests/raster + the R.1 report):
//   geom births  u_iw = trunc(u_s105 * inv_w_real).  At a pixel, affine interp
//   of u_iw recovers  num_uiw = sum(w_i*u_iw_i)/area2 = u_s105_p *
//   inv_w_p_real. Perspective recover S10.5:  u_s105_p = num_uiw / inv_w_p_real
//                                        = num_uiw * 65536 / inv_w_p.
//   Map S10.5 -> texel (32 S10.5 units = 1 texel, 5 frac bits) in Q16.16:
//     u_q16 = (u_s105_p / 32) << 16 = u_s105_p << 11
//           = (num_uiw << 27) / inv_w_p   [fused: keeps fractional texel bits
//                                          for THREE_POINT; POINT idx =
//                                          q16>>16].
//   PARITY (P3-5): the int64-num / int64-(inv_w_p) signed divide truncates
//   toward zero exactly like the existing inv_w divide (raster_one) and
//   fx_div -> host<->device bit-identical. Guards inv_w_p>0 (depth_pack rejects
//   <=0; a valid in-front fragment has inv_w_p>0).
static fx16_16 perspective_texcoord_q16(int32_t num_uiw, int32_t inv_w_p) {
  if (inv_w_p <= 0) {
    // Mirrors the flat path: a non-positive inv_w is a behind-camera / invalid
    // fragment (clip rejects those upstream, and depth_pack already clamps it
    // to the clear sentinel) -> return a defined 0 texel coord, no
    // divide-by-zero.
    return 0;
  }
  int64_t const q = ((int64_t)num_uiw << 27) / (int64_t)inv_w_p;
  return (fx16_16)q;
}

// Clamp a stepped value to uint8 [0,255] — the Gouraud/fog readout clamp. The
// affine SHADE/fog interpolation (out8 = sum(w_i*chan_i)/area2, truncated; N64
// RDP SHADE is NOT perspective-corrected, only UV is) is now the L1 stepper;
// this applies the post-divide [0,255] clamp to the stepped value.
static uint8_t clamp_u8(int32_t v) {
  if (v < 0) {
    return 0;
  }
  if (v > 255) {
    return 255;
  }
  return (uint8_t)v;
}

// T5 L1: screen-affine basis for one (triangle, tile) — the three edge values
// at the bounding-box origin pixel center plus their exact per-pixel (+x) and
// per-scanline (+y) integer deltas. The edge functions are linear in the pixel
// center (one pixel = 1<<RASTER_SUBPX in Q12.4), so each interpolant
// num = w0*Q0 + w1*Q1 + w2*Q2 is screen-affine and DDA-steppable from this
// basis (Abrash Black Book ch66: 1/z, s/z, t/z vary linearly in screenspace).
struct EdgeAffine {
  int32_t w0_o, w1_o, w2_o;        // edge values at (bb_minx,bb_miny) pixel ctr
  int32_t dw0_dx, dw1_dx, dw2_dx;  // per-pixel +x deltas
  int32_t dw0_dy, dw1_dy, dw2_dy;  // per-scanline +y deltas
  int32_t area2;                   // shared divisor (> 0)
};

// noinline: this runs per (triangle,tile) — NOT per pixel — so keep its code
// OUT of the SRAM-resident (.time_critical) raster_tile that inlines
// raster_one; flash-resident setup keeps the tight RP2040 SRAM for the hot
// per-pixel loop.
static void INTERP_NOINLINE edge_affine_init(struct EdgeAffine* e,
                                             const struct TriSetup* t,
                                             int bb_minx, int bb_miny) {
  fx12_4 const cx0 = (fx12_4)((bb_minx << RASTER_SUBPX) + RASTER_HALF);
  fx12_4 const cy0 = (fx12_4)((bb_miny << RASTER_SUBPX) + RASTER_HALF);
  e->w0_o = edge_eval(t->x1, t->y1, t->x2, t->y2, cx0, cy0);
  e->w1_o = edge_eval(t->x2, t->y2, t->x0, t->y0, cx0, cy0);
  e->w2_o = edge_eval(t->x0, t->y0, t->x1, t->y1, cx0, cy0);
  // d w_i / d(pixel) from edge_eval's closed form (one pixel =
  // 1<<RASTER_SUBPX).
  int32_t const step = (int32_t)1 << RASTER_SUBPX;
  e->dw0_dx = (int32_t)(t->y1 - t->y2) * step;
  e->dw1_dx = (int32_t)(t->y2 - t->y0) * step;
  e->dw2_dx = (int32_t)(t->y0 - t->y1) * step;
  e->dw0_dy = (int32_t)(t->x2 - t->x1) * step;
  e->dw1_dy = (int32_t)(t->x0 - t->x2) * step;
  e->dw2_dy = (int32_t)(t->x1 - t->x0) * step;
  e->area2 = t->area2;
}

// Set up one interpolant stepper from its three per-vertex values over the
// EdgeAffine basis. Seeds one pixel LEFT of bb_minx (num_origin - dx) so the
// loop-top interp_step_x lands on bb_minx and advances UNCONDITIONALLY per
// pixel (the L1 desync invariant — the body's existing `continue`s stay valid
// because every accumulator is stepped before the inside test / any early-out).
// noinline (flash-resident): per-(triangle,tile) setup, not per pixel — see
// edge_affine_init. Collapses the divide-heavy floor_divmod setup to one flash
// copy + call sites instead of inlining it into the SRAM hot path.
static void INTERP_NOINLINE interp_setup(struct Interp* p,
                                         const struct EdgeAffine* e, int32_t q0,
                                         int32_t q1, int32_t q2) {
  int64_t const num_o = ((int64_t)e->w0_o * q0) + ((int64_t)e->w1_o * q1) +
                        ((int64_t)e->w2_o * q2);
  int64_t const dx = ((int64_t)e->dw0_dx * q0) + ((int64_t)e->dw1_dx * q1) +
                     ((int64_t)e->dw2_dx * q2);
  int64_t const dy = ((int64_t)e->dw0_dy * q0) + ((int64_t)e->dw1_dy * q1) +
                     ((int64_t)e->dw2_dy * q2);
  interp_init(p, num_o - dx, dx, dy, e->area2);
}

// Rasterize one already-setup triangle into the framebuffer + tile Z scratch.
// tile_px_x0/y0 = the tile's top-left pixel in screen space. `rs` selects the
// per-pixel path: flat-fill fast path (no valid texture, BIT-IDENTICAL to the
// pre-R.1 rasterizer) or textured + Gouraud + combiner (valid texture).
// Returns the number of pixels that passed the inside test (entered the heavy
// per-pixel body) when PROFILER is on — the denominator for the opaque-fill
// ns/pixel metric. Returns 0 when PROFILER=0: the counter compiles away and the
// production path is byte-for-byte unchanged.
static uint32_t raster_one(const struct TriSetup* t,
                           const struct RenderState* rs, int tile_px_x0,
                           int tile_px_y0, uint16_t* fb, uint16_t* zbuf,
                           uint8_t* cov) {
  int const textured = rs_has_texture(rs);
#if PROFILER
  uint32_t px_inside = 0;  // pixels entering the heavy body (ns/px denominator)
#endif
  // R.4: 2-cycle detail multitexture iff cycle==TWO and tex1 valid (constant
  // per triangle). When 0, the combiner step below is the EXACT pre-R.4
  // shade_pixel path -> byte-identical (the regression gate).
  int const two_cyc = textured && rs_two_cycle(rs);
  // Pre-unpack the three shade colors once (textured path only; the fast path
  // never touches them so it stays bit-identical).
  uint8_t sr[3];
  uint8_t sg[3];
  uint8_t sb[3];
  if (textured) {
    unpack565(t->rgba0, &sr[0], &sg[0], &sb[0]);
    unpack565(t->rgba1, &sr[1], &sg[1], &sb[1]);
    unpack565(t->rgba2, &sr[2], &sg[2], &sb[2]);
  }
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

  // T5 L1: set up the screen-affine interpolant steppers for this (tri,tile).
  // inv_w always; fog if enabled; u/v + Gouraud only on the textured path. The
  // per-pixel num/area2 divides become adds (interp_step_x) + a readout.
  struct EdgeAffine ea;
  edge_affine_init(&ea, t, bb_minx, bb_miny);
  struct Interp i_invw;
  interp_setup(&i_invw, &ea, t->iw0, t->iw1, t->iw2);
  int const fog_en = rs->fog.enabled;
  struct Interp i_fog = {};
  if (fog_en) {
    interp_setup(&i_fog, &ea, (int32_t)t->fog0, (int32_t)t->fog1,
                 (int32_t)t->fog2);
  }
  struct Interp i_u = {};
  struct Interp i_v = {};
  struct Interp i_gr = {};
  struct Interp i_gg = {};
  struct Interp i_gb = {};
  if (textured) {
    interp_setup(&i_u, &ea, t->u_iw0, t->u_iw1, t->u_iw2);
    interp_setup(&i_v, &ea, t->v_iw0, t->v_iw1, t->v_iw2);
    interp_setup(&i_gr, &ea, (int32_t)sr[0], (int32_t)sr[1], (int32_t)sr[2]);
    interp_setup(&i_gg, &ea, (int32_t)sg[0], (int32_t)sg[1], (int32_t)sg[2]);
    interp_setup(&i_gb, &ea, (int32_t)sb[0], (int32_t)sb[1], (int32_t)sb[2]);
  }

  for (int sy = bb_miny; sy < bb_maxy; ++sy) {
    fx12_4 const cy = (fx12_4)((sy << RASTER_SUBPX) + RASTER_HALF);
    interp_begin_row(&i_invw);
    if (fog_en) {
      interp_begin_row(&i_fog);
    }
    if (textured) {
      interp_begin_row(&i_u);
      interp_begin_row(&i_v);
      interp_begin_row(&i_gr);
      interp_begin_row(&i_gg);
      interp_begin_row(&i_gb);
    }
    for (int sx = bb_minx; sx < bb_maxx; ++sx) {
      // T5 L1: advance every active stepper UNCONDITIONALLY (before the inside
      // test / any early-out) so each accumulator stays in lockstep with sx.
      interp_step_x(&i_invw);
      if (fog_en) {
        interp_step_x(&i_fog);
      }
      if (textured) {
        interp_step_x(&i_u);
        interp_step_x(&i_v);
        interp_step_x(&i_gr);
        interp_step_x(&i_gg);
        interp_step_x(&i_gb);
      }
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
#if PROFILER
      ++px_inside;  // entered the heavy body — counted pre-z-test on purpose:
                    // the textured path pays full texture+combiner BEFORE the
                    // z-test, so this is the true per-pixel-work count.
#endif
      // Interpolate 1/w (perspective-correct in 1/w is screen-linear): the
      // (w0*iw0 + w1*iw1 + w2*iw2)/area2 divide is now the L1 stepper readout,
      // bit-identical to (int32_t)(num/area2).
      int32_t const inv_w = interp_value(&i_invw);

      int const tile_x = sx - tile_px_x0;
      int const tile_y = sy - tile_px_y0;
      int const zi = (tile_y * RDR_TILE_W) + tile_x;
      uint16_t const znew = depth_pack(inv_w);

      if (!textured) {
        // FAST PATH (BIT-IDENTICAL to the pre-R.1 rasterizer WHEN FOG OFF):
        // flat fill. w-buffer: larger inv_w is closer. Pass if at least as
        // close as stored.
        if (znew < zbuf[zi]) {
          continue;
        }
        zbuf[zi] = znew;
        // R.3 FOG: an untextured material can still carry fog. Disabled fog
        // skips this branch -> the write stays t->color, BIT-IDENTICAL to the
        // pre-R.3 flat path (the regression gate). Affine per-pixel factor.
        uint16_t out = t->color;
        if (fog_en) {
          uint8_t const fogf = clamp_u8(interp_value(&i_fog));
          out = fog_lerp(out, rs->fog.color, fogf);
        }
        fb[(sy * RDR_SCREEN_W) + sx] = out;
        // R.3-AA: analytic coverage of the WINNING fragment (same point as the
        // fb store). `cov` is null / left untouched when AA is off -> the flat
        // fast path stays BYTE-IDENTICAL to the pre-AA renderer.
        if (cov != 0) {
          cov[zi] = cov_byte(w0, w1, w2, t->r0, t->r1, t->r2);
        }
        continue;
      }

      // TEXTURED + GOURAUD + COMBINER PATH (R.1, opaque pass).
      // Perspective-correct UV: affine-interp u_iw/v_iw (L1 stepper readouts,
      // same num/area2 values as before), then recover the Q16.16 texel coord.
      int32_t const u_iw_p = interp_value(&i_u);
      int32_t const v_iw_p = interp_value(&i_v);
      fx16_16 const u_q16 = perspective_texcoord_q16(u_iw_p, inv_w);
      fx16_16 const v_q16 = perspective_texcoord_q16(v_iw_p, inv_w);

      // Gouraud shade (affine; NOT perspective-corrected — N64 RDP convention).
      // L1 stepper readouts (same num/area2 values), clamped as gouraud_chan
      // did.
      uint8_t const gr = clamp_u8(interp_value(&i_gr));
      uint8_t const gg = clamp_u8(interp_value(&i_gg));
      uint8_t const gb = clamp_u8(interp_value(&i_gb));
      uint16_t const shade565 = rgb565(gr, gg, gb);

      // Alpha-cutout (N11): if alpha-compare is on, fetch full RGBA and DISCARD
      // (skip BOTH fb AND zbuf writes) when texel alpha < threshold. Otherwise
      // sample just the 565 texel. The combiner runs on the 565 texel either
      // way (RGB565 target carries no alpha; cutout is binary keep/drop here).
      uint16_t texel565;
      if (rs->alpha_cmp != 0) {
        uint8_t rgba[4];
        tex_sample_rgba(&rs->tex, u_q16, v_q16, 0, rgba);
        if (rgba[3] < rs->alpha_cmp) {
          continue;  // discard BEFORE z-write: leaves zbuf[zi] untouched.
        }
        texel565 = rgb565(rgba[0], rgba[1], rgba[2]);
      } else {
        texel565 = tex_sample(&rs->tex, u_q16, v_q16, 0);
      }

      uint8_t keep = 1;
      uint16_t out;
      if (two_cyc) {
        // R.4: sample the detail texel (TEXEL1) at the detail-shifted UV, then
        // run the 2-cycle combiner (combiner -> combiner2, CC_COMBINED
        // feed-forward).
        uint16_t const texel1_565 = sample_texel1(rs, u_q16, v_q16);
        out = shade_pixel2(rs, texel565, texel1_565, shade565, &keep);
      } else {
        out = shade_pixel(rs, texel565, shade565, &keep);
      }
      if (!keep) {
        continue;  // forward-compat: honor a future alpha-compare in the
                   // combiner.
      }
      // R.3 FOG (post-combiner): lerp the combiner color toward
      // rstate.fog.color by the per-pixel fog factor (affine / screen-linear
      // interp of the per-vertex TVtx.fog — NOT perspective-corrected, matching
      // the N64 per-vertex fog convention; same form/truncation as
      // gouraud_chan). Disabled fog -> skip (TVtx.fog is born 0 then, so this
      // is bit-identical to no-fog regardless, but the gate also avoids the
      // work).
      if (fog_en) {
        uint8_t const fogf = clamp_u8(interp_value(&i_fog));
        out = fog_lerp(out, rs->fog.color, fogf);
      }
      // z-test AFTER cutout/keep so a discarded fragment never seeds depth.
      if (znew < zbuf[zi]) {
#if PROFILER
        // Overdraw probe: this textured fragment shaded fully then LOST the
        // z-test — exactly the work Q3 (z-test-first) would elide. Sizes Q3.
        ++g_prof_opaque_zrej[prof_core()];
#endif
        continue;
      }
      zbuf[zi] = znew;
      fb[(sy * RDR_SCREEN_W) + sx] = out;
      // R.3-AA coverage of the winning textured fragment (after z-pass + keep,
      // same point as the fb store). Skipped when AA is off (null `cov`).
      if (cov != 0) {
        cov[zi] = cov_byte(w0, w1, w2, t->r0, t->r1, t->r2);
      }
    }
  }
#if PROFILER
  return px_inside;
#else
  return 0;
#endif
}

// Rasterize ONE already-setup TRANSLUCENT (XLU) triangle (L6, sweep 2). Shares
// the opaque path's coverage + perspective-UV + Gouraud + combiner + fog, but
// the fragment store is the L6 FRONT-TO-BACK PREMULTIPLIED UNDER accumulate:
//   - z-TEST only, NO z-write (a farther XLU fragment is not blocked by a
//     nearer one — the FRONT-TO-BACK sort + the accumulator own ordering, not
//     the depth buffer);
//   - SATURATION EARLY-OUT: if this pixel's accumulated alpha already reached
//     RASTER_XLU_SAT, skip the fragment entirely (this is the overdraw win —
//     farther fragments contribute ~0 through the residual transmittance);
//   - otherwise accumulate the FOGGED combiner color premultiplied by the
//     effective contribution alpha into the per-worker per-tile accumulator
//     (g_xlu_c_acc / g_xlu_acc_alpha) via blend_premul_accumulate. NO fb write
//     here (the final composite pass folds terrain under and writes fb); NO cov
//     write here (the composite pass stamps AA_COV_FULL on composited pixels,
//     preserving the R.3-AA "AA ignores XLU" behavior exactly).
// XLU materials are ALWAYS textured here (the flat fast path is opaque-only and
// stays in sweep 1); a non-textured XLU material would write nothing, so we
// gate on a valid texture and skip otherwise. `rs->alpha_cmp` is honored
// (discard below threshold) — the soft-edge tree XLU material sets it to 1 to
// drop only fully-transparent texels while keeping+blending the bilinear soft
// ring.
// `zbuf` is const here: the XLU sweep z-TESTS but never WRITES depth (the no-
// z-write invariant, enforced at the type level — a write would not compile).
// L6 drops the `fb`/`cov` parameters this function carried under R.2: the
// per-fragment path no longer touches the framebuffer or coverage — it writes
// only the accumulator, and the composite pass in raster_tile_noclear owns the
// single fb write + cov stamp. `c_acc`/`acc_alpha` are this worker's already-
// sliced premultiplied-UNDER accumulator (the caller selected the slice via the
// threaded `worker` id and cleared it at sweep-2 start); this tri accumulates
// front-to-back into it. Returns inside-pixel count when PROFILER is on
// (mirrors raster_one — same ns/px denominator), 0 otherwise.
static uint32_t raster_one_xlu(const struct TriSetup* t,
                               const struct RenderState* rs, int tile_px_x0,
                               int tile_px_y0, const uint16_t* zbuf,
                               uint16_t* c_acc, uint8_t* acc_alpha) {
  int const textured = rs_has_texture(rs);
  if (!textured) {
    return 0;  // XLU without a texture contributes nothing (flat is opaque).
  }
#if PROFILER
  uint32_t px_inside = 0;  // pixels passing the inside test (ns/px denominator)
#endif
  // R.4: 2-cycle detail iff cycle==TWO and tex1 valid (constant per tri); else
  // the EXACT pre-R.4 shade_pixel XLU path (byte-identical regression gate).
  // The `textured &&` is redundant here (the early-return above guarantees it)
  // but mirrors raster_one's form so a future refactor can't silently drop the
  // base-texture guard.
  int const two_cyc = textured && rs_two_cycle(rs);
  uint8_t sr[3];
  uint8_t sg[3];
  uint8_t sb[3];
  unpack565(t->rgba0, &sr[0], &sg[0], &sb[0]);
  unpack565(t->rgba1, &sr[1], &sg[1], &sb[1]);
  unpack565(t->rgba2, &sr[2], &sg[2], &sb[2]);

  // Triangle screen bounding box (pixels), intersected with the tile rect —
  // identical to raster_one's setup.
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

  int bb_minx = (int)(minx_q >> RASTER_SUBPX);
  int bb_miny = (int)(miny_q >> RASTER_SUBPX);
  int bb_maxx = (int)((maxx_q + 15) >> RASTER_SUBPX);
  int bb_maxy = (int)((maxy_q + 15) >> RASTER_SUBPX);

  int const tile_minx = tile_px_x0;
  int const tile_miny = tile_px_y0;
  int const tile_maxx = tile_px_x0 + RDR_TILE_W;  // exclusive
  int const tile_maxy = tile_px_y0 + RDR_TILE_H;  // exclusive
  bb_minx = clampi(bb_minx, tile_minx, tile_maxx);
  bb_miny = clampi(bb_miny, tile_miny, tile_maxy);
  bb_maxx = clampi(bb_maxx, tile_minx, tile_maxx);
  bb_maxy = clampi(bb_maxy, tile_miny, tile_maxy);

  // T5 L1: screen-affine interpolant steppers (XLU is always textured here, so
  // inv_w + u/v + Gouraud are always set up; fog if enabled). Same
  // exact-integer DDA as raster_one — golden-neutral.
  struct EdgeAffine ea;
  edge_affine_init(&ea, t, bb_minx, bb_miny);
  struct Interp i_invw;
  struct Interp i_u;
  struct Interp i_v;
  struct Interp i_gr;
  struct Interp i_gg;
  struct Interp i_gb;
  interp_setup(&i_invw, &ea, t->iw0, t->iw1, t->iw2);
  interp_setup(&i_u, &ea, t->u_iw0, t->u_iw1, t->u_iw2);
  interp_setup(&i_v, &ea, t->v_iw0, t->v_iw1, t->v_iw2);
  interp_setup(&i_gr, &ea, (int32_t)sr[0], (int32_t)sr[1], (int32_t)sr[2]);
  interp_setup(&i_gg, &ea, (int32_t)sg[0], (int32_t)sg[1], (int32_t)sg[2]);
  interp_setup(&i_gb, &ea, (int32_t)sb[0], (int32_t)sb[1], (int32_t)sb[2]);
  int const fog_en = rs->fog.enabled;
  struct Interp i_fog = {};
  if (fog_en) {
    interp_setup(&i_fog, &ea, (int32_t)t->fog0, (int32_t)t->fog1,
                 (int32_t)t->fog2);
  }

  for (int sy = bb_miny; sy < bb_maxy; ++sy) {
    fx12_4 const cy = (fx12_4)((sy << RASTER_SUBPX) + RASTER_HALF);
    interp_begin_row(&i_invw);
    interp_begin_row(&i_u);
    interp_begin_row(&i_v);
    interp_begin_row(&i_gr);
    interp_begin_row(&i_gg);
    interp_begin_row(&i_gb);
    if (fog_en) {
      interp_begin_row(&i_fog);
    }
    for (int sx = bb_minx; sx < bb_maxx; ++sx) {
      // T5 L1: advance every stepper UNCONDITIONALLY (before the inside test /
      // z-test / saturation early-out) so each stays in lockstep with sx.
      interp_step_x(&i_invw);
      interp_step_x(&i_u);
      interp_step_x(&i_v);
      interp_step_x(&i_gr);
      interp_step_x(&i_gg);
      interp_step_x(&i_gb);
      if (fog_en) {
        interp_step_x(&i_fog);
      }
      fx12_4 const cx = (fx12_4)((sx << RASTER_SUBPX) + RASTER_HALF);
      int32_t const w0 = edge_eval(t->x1, t->y1, t->x2, t->y2, cx, cy);
      int32_t const w1 = edge_eval(t->x2, t->y2, t->x0, t->y0, cx, cy);
      int32_t const w2 = edge_eval(t->x0, t->y0, t->x1, t->y1, cx, cy);
      int const in0 = (w0 > 0) || (w0 == 0 && t->tl1);
      int const in1 = (w1 > 0) || (w1 == 0 && t->tl2);
      int const in2 = (w2 > 0) || (w2 == 0 && t->tl0);
      if (!(in0 && in1 && in2)) {
        continue;
      }
#if PROFILER
      ++px_inside;  // inside the tri (same denominator as opaque). NOTE: XLU
                    // z-TESTS before shading (below), so a z-rejected XLU pixel
                    // is cheap — unlike opaque, which shades before the z-test.
#endif
      int32_t const inv_w =
          interp_value(&i_invw);  // L1 stepper (golden-neutral)

      int const tile_x = sx - tile_px_x0;
      int const tile_y = sy - tile_px_y0;
      int const zi = (tile_y * RDR_TILE_W) + tile_x;
      uint16_t const znew = depth_pack(inv_w);

      // z-TEST against the opaque depth left by sweep 1 (NO z-write — see the
      // function comment). Reject a fragment behind the stored opaque depth.
      if (znew < zbuf[zi]) {
        continue;
      }
#if PROFILER && PROF_OVERDRAW
      // Overdraw probe: a z-passing XLU fragment. Transition-count the
      // per-pixel depth so the report derives mean overdraw (frags/distinct) +
      // the depth>=2 fraction — the L6 go/no-go gate. (Placed after the z-test,
      // so it counts only fragments that would actually composite.)
      {
        uint32_t const pc = prof_core();
        uint8_t* const dp = &g_prof_xlu_depth[pc][zi];
        if (*dp == 0) {
          ++g_prof_xlu_distinct[pc];
        } else if (*dp == 1) {
          ++g_prof_xlu_ge2[pc];
        }
        if (*dp < 255) {
          ++(*dp);
        }
        ++g_prof_xlu_frags[pc];
      }
#endif

      // L6 SATURATION EARLY-OUT (the overdraw win): if this pixel's accumulated
      // alpha already reached RASTER_XLU_SAT, every FARTHER fragment (we are
      // front-to-back) contributes ~0 through the residual transmittance — skip
      // ALL of its shade/texture/combiner/fog work. Placed AFTER the profiler
      // probe so the probe still reports the true (pre-early-out) overdraw, and
      // BEFORE the expensive shade so the skip is maximally cheap. The compare
      // is >= against a uint8 accumulated alpha, so RASTER_XLU_SAT==256 (> any
      // uint8) never triggers (pure-reorder, for re-running the A/B).
      if ((uint32_t)acc_alpha[zi] >= (uint32_t)RASTER_XLU_SAT) {
        continue;
      }

      // Perspective-correct UV (L1 stepper readouts; same num/area2 values).
      int32_t const u_iw_p = interp_value(&i_u);
      int32_t const v_iw_p = interp_value(&i_v);
      fx16_16 const u_q16 = perspective_texcoord_q16(u_iw_p, inv_w);
      fx16_16 const v_q16 = perspective_texcoord_q16(v_iw_p, inv_w);

      // Gouraud shade (affine; N64 RDP convention) — L1 stepper readouts.
      uint8_t const gr = clamp_u8(interp_value(&i_gr));
      uint8_t const gg = clamp_u8(interp_value(&i_gg));
      uint8_t const gb = clamp_u8(interp_value(&i_gb));
      uint16_t const shade565 = rgb565(gr, gg, gb);

      // XLU ALWAYS needs the texel's own alpha for the blend, so fetch full
      // RGBA (not just the 565). Honor alpha-compare discard if configured.
      uint8_t rgba[4];
      tex_sample_rgba(&rs->tex, u_q16, v_q16, 0, rgba);
      if (rs->alpha_cmp != 0 && rgba[3] < rs->alpha_cmp) {
        continue;  // discard (no fb touch, and never wrote zbuf anyway)
      }
      uint16_t const texel565 = rgb565(rgba[0], rgba[1], rgba[2]);

      uint8_t keep = 1;
      uint16_t combined;
      if (two_cyc) {
        // R.4: detail texel (TEXEL1) at the detail-shifted UV, then the 2-cycle
        // combiner. XLU still blends on TEXEL0's alpha (rgba[3]) below — the
        // detail multitexture only affects the combined RGB.
        uint16_t const texel1_565 = sample_texel1(rs, u_q16, v_q16);
        combined = shade_pixel2(rs, texel565, texel1_565, shade565, &keep);
      } else {
        combined = shade_pixel(rs, texel565, shade565, &keep);
      }
      if (!keep) {
        continue;  // forward-compat combiner alpha-compare.
      }
      // R.3 FOG (post-combiner, BEFORE the premultiply): fog the combiner color
      // toward rstate.fog.color by the affine per-pixel fog factor, then
      // accumulate the FOGGED color premultiplied by its texel alpha (faithful:
      // a distant translucent fragment fogs first, then contributes). Affine
      // interp (N64 per-vertex fog), same path as the opaque sweep.
      if (fog_en) {
        uint8_t const fogf = clamp_u8(interp_value(&i_fog));
        combined = fog_lerp(combined, rs->fog.color, fogf);
      }
      // L6 FRONT-TO-BACK PREMULTIPLIED UNDER: accumulate the fogged combiner
      // color (straight) by its TEXEL0 alpha (rgba[3]) into this worker's
      // per-tile accumulator. NO fb write (the composite pass folds terrain
      // under and writes fb) and NO cov stamp here (the composite pass stamps
      // AA_COV_FULL on composited pixels, preserving "AA ignores XLU" exactly).
      blend_premul_accumulate(&c_acc[zi], &acc_alpha[zi], combined, rgba[3]);
    }
  }
#if PROFILER
  return px_inside;
#else
  return 0;
#endif
}

// Per-tri depth key: the sum of the 3 verts' inv_w (LARGER inv_w == NEARER).
// int64 because the sum of three Q16.16 values can exceed int32. inv_w is
// winding-swap-invariant under sum, so the PRE-setup verts give the same key as
// the post-setup ones.
static int64_t xlu_depth_key(const struct TVtx* pool,
                             const struct TriRef* ref) {
  return (int64_t)pool[ref->v0].inv_w + (int64_t)pool[ref->v1].inv_w +
         (int64_t)pool[ref->v2].inv_w;
}

// STABLE insertion sort of the gathered XLU bin-index array `idx[0..n)` by
// depth key. L6: sorted FRONT-TO-BACK = DESCENDING inv_w (nearest first), the
// reverse of R.2's back-to-front ascending order — front-to-back is what lets
// the premultiplied-UNDER accumulation early-out once a pixel's alpha
// saturates. Stability is load-bearing and PRESERVED: the shift condition is
// strict (<), so equal-key tris keep their GATHER order (== bin order, since
// gather is in bin order) — exactly the deterministic tie-break the
// dual==serial bit-identical invariant relies on.
// (std::stable_sort/qsort-with-index would do the same; a hand-written
// insertion sort keeps it Orthodox, allocation-free, and avoids the int64-key
// comparator-return overflow trap. n
// <= RASTER_XLU_TILE_CAP=256, so the O(n^2) worst case is bounded and tiny vs
// the per-pixel raster cost.)
static void xlu_sort_stable(uint16_t* idx, uint32_t n, const struct TVtx* pool,
                            const struct TileBin* bin) {
  for (uint32_t i = 1; i < n; ++i) {
    uint16_t const cur = idx[i];
    int64_t const cur_key = xlu_depth_key(pool, &bin->refs[cur]);
    uint32_t j = i;
    // DESCENDING (nearest first): shift strictly-SMALLER-key elements right;
    // STOP on <= so equal keys keep their earlier (smaller-index, == gather/bin
    // order) position -> stable. Flipping > to < here reverses R.2's order.
    while (j > 0 && xlu_depth_key(pool, &bin->refs[idx[j - 1]]) < cur_key) {
      idx[j] = idx[j - 1];
      --j;
    }
    idx[j] = cur;
  }
}

void __not_in_flash_func(raster_tile_noclear)(
    int tile, const struct TileBin* bin, const struct TVtx* pool, uint16_t* fb,
    uint16_t* zbuf, const struct RenderState* rstate_table, uint8_t* cov,
    int worker) {
  if (bin == 0 || pool == 0 || fb == 0 || zbuf == 0 || rstate_table == 0) {
    return;
  }
  // R.3-AA: the coverage WRITE is gated on an active scratch AND the runtime
  // enable. A null `cov` here means coverage is OFF for this sweep (so the
  // rasterizer is byte-identical to pre-AA); pass the same `cov_active` down so
  // raster_one/raster_one_xlu skip the coverage path entirely otherwise.
  uint8_t* const cov_active = (cov != 0 && aa_enabled() != 0) ? cov : 0;
  // Tile grid is row-major; compute this tile's top-left screen pixel.
  int const tiles_per_row = RDR_SCREEN_W / RDR_TILE_W;
  int const tile_px_x0 = (tile % tiles_per_row) * RDR_TILE_W;
  int const tile_px_y0 = (tile / tiles_per_row) * RDR_TILE_H;

  // SWEEP 1 — OPAQUE. Rasterize every NON-XLU ref EXACTLY as R.1 did (z-test +
  // z-write). ZMODE_OPAQUE and ZMODE_DECAL both come here; DECAL is treated as
  // opaque for now. NOTE: real decal depth-bias (decal-dZ co-planar offset) is
  // OUT OF T2 SCOPE — not implemented here; DECAL just routes to the opaque
  // sweep. This sweep is byte-identical to the pre-R.2 single loop for any
  // opaque-only bin (the hard regression gate).
  // Gather just the BIN INDICES of XLU refs (2 B each) into a fixed local
  // array. 256 * 2 B = 512 B on the stack — comfortable on the 2 KB RP2040
  // core1 stack (see RASTER_XLU_TILE_CAP). Overflow drops-with-count.
  uint16_t xlu_idx[RASTER_XLU_TILE_CAP];
  static_assert(sizeof(xlu_idx) <= 512,
                "XLU sort scratch must stay <= 512 B (RP2040 core1 stack is "
                "2 KB; a larger frame risks on-device overflow, invisible to "
                "host CI)");
  uint32_t xlu_n = 0;
  {
    // T5: COARSE (time_us_64), not FINE — the worst near-camera tile's opaque
    // fill exceeds SysTick's ~67ms span (measured on-target: wrap=1). 1us
    // resolution is ample for a ~25ms sweep.
    PROF_BLOCK(PROF_SWEEP_OPAQUE);
    for (uint32_t i = 0; i < bin->count; ++i) {
      const struct TriRef* ref = &bin->refs[i];
      if (rstate_table[ref->material].zmode == ZMODE_XLU) {
        // Defer to sweep 2: record the bin index IN BIN ORDER (the gather order
        // == array position is the deterministic tie-break for the stable
        // sort). Drop-with-count on overflow (mirrors TileBin's drop POLICY;
        // never corrupt).
        if (xlu_n < (uint32_t)RASTER_XLU_TILE_CAP) {
          xlu_idx[xlu_n] = (uint16_t)i;
          ++xlu_n;
        } else {
          ++s_xlu_dropped;
        }
        continue;
      }
      struct TriSetup t;
      int rc;
      {
        // T5 FINE: per-triangle setup span (SysTick) — << 67ms, never wraps.
        PROF_BLOCK_FINE(PROF_OPAQUE_SETUP);
        rc = tri_setup(&t, &pool[ref->v0], &pool[ref->v1], &pool[ref->v2]);
      }
      if (rc != RDR_OK) {
        continue;  // degenerate -> rejected, no fill
      }
      // TriRef.material indexes the interned render-state table; it selects the
      // per-pixel path (flat fast path vs textured/Gouraud/combiner).
      const struct RenderState* rs = &rstate_table[ref->material];
      uint32_t px;
      {
        // T5 FINE: per-triangle fill span (SysTick) — the per-pixel inner loop.
        PROF_BLOCK_FINE(PROF_OPAQUE_FILL);
        px = raster_one(&t, rs, tile_px_x0, tile_px_y0, fb, zbuf, cov_active);
      }
#if PROFILER
      // Accumulate filled-pixel count into this core's slot — once per triangle
      // (prof_core() + add, negligible), NOT per pixel. Outside the FINE block.
      g_prof_opaque_px[prof_core()] += px;
#else
      (void)px;
#endif
    }
  }

  // SWEEP 2 — TRANSLUCENT (L6 FRONT-TO-BACK). STABLE-sort the gathered XLU
  // indices FRONT-TO-BACK (descending inv_w = nearest first); equal-depth tris
  // keep bin order. Each tri ACCUMULATES premultiplied UNDER into this worker's
  // per-tile accumulator (g_xlu_c_acc/g_xlu_acc_alpha) with a saturation
  // early-out — no fb write per fragment. Then the COMPOSITE pass folds the
  // opaque terrain under the accumulation and writes fb once per touched pixel.
  // Each tile is drawn by exactly ONE worker, and the worker's accumulator
  // slice is cleared here per tile, so a deterministic (stable) sort makes
  // serial == 2-worker bit-identical (the accumulator never leaks across tiles
  // or workers). The slice is selected by the THREADED `worker` id (the same id
  // that selects zbuf[worker]/cov[worker] in sched_drain_tile) — masked into
  // [0,RASTER_NUM_WORKERS) for the static analyzer (RASTER_NUM_WORKERS is a
  // power of two).
  uint32_t const w_acc = (uint32_t)worker & (uint32_t)(RASTER_NUM_WORKERS - 1);
  uint16_t* const c_acc = g_xlu_c_acc[w_acc];
  uint8_t* const acc_alpha = g_xlu_acc_alpha[w_acc];
#if PROFILER && PROF_OVERDRAW
  // Overdraw probe: reset THIS worker's per-pixel XLU depth for this tile,
  // OUTSIDE the timed sweep_xlu block (so the memset doesn't perturb the
  // measurement). Per-core slice; the XLU sweep below counts into it.
  memset(g_prof_xlu_depth[prof_core()], 0, sizeof g_prof_xlu_depth[0]);
#endif
  {
    // T5: sweep 2 — sort + translucent accumulate + composite. COARSE
    // (time_us_64), NOT FINE: a tree-heavy tile's XLU fill (soft billboards,
    // heavy overdraw) exceeds SysTick's ~67ms span — measured wrap=1 on every
    // window when FINE, which UNDER-reported XLU and hid its true cost in the
    // coarse raster_tile "unaccounted" remainder. Coarse, like sweep_opaque, is
    // wrap-free and gives the real XLU magnitude for the opaque-vs-XLU compare.
    PROF_BLOCK(PROF_SWEEP_XLU);
    // L6: clear this worker's per-tile premultiplied accumulator (color 0 +
    // alpha 0 = "nothing accumulated"). Per-tile, like the zbuf/cov clear, so
    // the accumulator carries nothing across tiles -> dual==serial holds. Skip
    // the clear+sweep entirely when this tile has no XLU (the common
    // opaque-only tile stays byte-identical to pre-L6: no accumulator touched,
    // no fb write).
    if (xlu_n > 0) {
      memset(c_acc, 0, (size_t)(RDR_TILE_W * RDR_TILE_H) * sizeof c_acc[0]);
      memset(acc_alpha, 0,
             (size_t)(RDR_TILE_W * RDR_TILE_H) * sizeof acc_alpha[0]);
    }
    if (xlu_n > 1) {
      xlu_sort_stable(xlu_idx, xlu_n, pool, bin);
    }
    for (uint32_t k = 0; k < xlu_n; ++k) {
      const struct TriRef* ref = &bin->refs[xlu_idx[k]];
      struct TriSetup t;
      int const rc =
          tri_setup(&t, &pool[ref->v0], &pool[ref->v1], &pool[ref->v2]);
      if (rc != RDR_OK) {
        continue;  // degenerate -> rejected, no fill
      }
      const struct RenderState* rs = &rstate_table[ref->material];
      uint32_t const px = raster_one_xlu(&t, rs, tile_px_x0, tile_px_y0, zbuf,
                                         c_acc, acc_alpha);
#if PROFILER
      // Per-core XLU inside-pixel count (same denominator as opaque) — once per
      // tri, not per pixel. ns/px = coarse sweep_xlu / xlu_px (µs-granular,
      // fine for a hundreds-of-ms sweep).
      g_prof_xlu_px[prof_core()] += px;
#else
      (void)px;
#endif
    }

    // L6 COMPOSITE PASS: fold the opaque terrain UNDER the accumulation, once
    // per touched pixel. For each tile pixel with acc_alpha != 0:
    //   fb = blend_premul_resolve(c_acc, acc_alpha, fb_terrain)
    // and stamp cov = AA_COV_FULL (preserves "AA ignores XLU" — the composited
    // pixel reads as interior in the resolve). Pixels with acc_alpha == 0 are
    // LEFT UNTOUCHED, so an opaque-only tile is byte-identical to pre-L6 (the
    // hard regression gate). Only runs when this tile had XLU geometry.
    if (xlu_n > 0) {
      for (int ty = 0; ty < RDR_TILE_H; ++ty) {
        for (int tx = 0; tx < RDR_TILE_W; ++tx) {
          int const zi = (ty * RDR_TILE_W) + tx;
          if (acc_alpha[zi] == 0U) {
            continue;  // no XLU contribution here -> leave the terrain as-is
          }
          int const px = ((tile_px_y0 + ty) * RDR_SCREEN_W) + (tile_px_x0 + tx);
          fb[px] = blend_premul_resolve(c_acc[zi], acc_alpha[zi], fb[px]);
          if (cov_active != 0) {
            cov_active[zi] = AA_COV_FULL;
          }
        }
      }
    }
  }
}

// Debug telemetry: MONOTONIC total of XLU tris dropped (any per-tile gather >
// RASTER_XLU_TILE_CAP) since process start — NOT a per-frame count and never
// reset (no frame hook in this lane). Consumers take a DELTA (before/after),
// not an absolute. NEVER feeds the framebuffer path (drops degrade the
// translucent layer gracefully without corruption); a rising value is a
// re-surface signal that the per-tile XLU cap needs raising (or a Φ frame.h
// scratch). See RASTER_XLU_TILE_CAP. Not atomic across cores (debug only).
uint32_t raster_xlu_dropped(void) { return s_xlu_dropped; }

void __not_in_flash_func(raster_tile)(int tile, const struct TileBin* bin,
                                      const struct TVtx* pool, uint16_t* fb,
                                      uint16_t* zbuf,
                                      const struct RenderState* rstate_table,
                                      uint8_t* cov, int worker) {
  if (zbuf == 0) {
    return;
  }
  PROF_BLOCK(PROF_RASTER_TILE);  // T5: whole-tile span (both cores) — COARSE/us
  // R.3-AA: coverage is active only when a scratch is supplied AND the runtime
  // enable is set. When inactive, every coverage step below is skipped -> the
  // rasterizer (and the framebuffer) is BYTE-IDENTICAL to the pre-AA renderer
  // (the golden-neutral gate).
  uint8_t* const cov_active = (cov != 0 && aa_enabled() != 0) ? cov : 0;
  // Per-tile Z clear (spec: clear Z scratch at the start of each tile), then
  // rasterize. The clear + draw are split so a depth-hazard demonstration
  // (tests/sched: two workers sharing one un-recleared scratch) can drive the
  // real rasterizer without re-clearing; production callers use raster_tile.
  for (int i = 0; i < RDR_TILE_W * RDR_TILE_H; ++i) {
    zbuf[i] = RASTER_Z_CLEAR;
  }
  // R.3-AA: clear the coverage scratch to FULL on entry (like zbuf), so a pixel
  // no fragment touches reads as interior -> the resolve leaves it untouched.
  if (cov_active != 0) {
    for (int i = 0; i < RDR_TILE_W * RDR_TILE_H; ++i) {
      cov_active[i] = AA_COV_FULL;
    }
  }
  raster_tile_noclear(tile, bin, pool, fb, zbuf, rstate_table, cov_active,
                      worker);
  // R.3-AA: within-tile, edge-gated, border-clamped resolve. Reads only this
  // tile's pixels (no cross-tile / cross-core FB access) -> the dual-core sweep
  // stays bit-identical to serial. aa_resolve_tile no-ops if AA is off, but we
  // already gate on cov_active so it only runs when coverage was written.
  if (cov_active != 0) {
    int const tiles_per_row = RDR_SCREEN_W / RDR_TILE_W;
    int const tile_px_x0 = (tile % tiles_per_row) * RDR_TILE_W;
    int const tile_px_y0 = (tile / tiles_per_row) * RDR_TILE_H;
    {
      PROF_BLOCK_FINE(PROF_AA_RESOLVE);  // T5: coverage resolve (FINE/SysTick)
      aa_resolve_tile(fb, cov_active, tile_px_x0, tile_px_y0);
    }
  }
}
