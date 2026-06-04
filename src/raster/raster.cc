// raster.cc — half-space edge-function rasterizer. Per-material: flat-fill fast
// path (BIT-IDENTICAL to B.1-gamma) OR textured + Gouraud + combiner path
// (R.1), then a two-pass split (R.2): an OPAQUE sweep (sweep 1, unchanged from
// R.1) followed by a back-to-front-sorted TRANSLUCENT sweep (sweep 2, z-test
// only + texel-alpha alpha-over). Stream B.1-gamma + R.1 + R.2. Orthodox C++:
// POD + free functions, C headers, C-style casts, no STL/auto/lambda/templates/
// exceptions. See raster.h for contract.
#include "raster/raster.h"

#include <stdint.h>
#include <stdlib.h>  // qsort (per-tile XLU back-to-front sort)
#include <string.h>

#include "blend/blend.h"      // blend_pixel_alpha (XLU texel-alpha alpha-over)
#include "gfx/framebuffer.h"  // rgb565()
#include "rdr/config.h"
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

// R.2 XLU per-tile sort scratch cap (the SIZING decision, re-surface trigger).
// Sweep 2 gathers this tile's XLU TriRef indices into a FIXED local uint16_t
// array, then sorts them back-to-front. A full per-tile cap (bins draw from
// RDR_BIN_POOL_REFS=2048; a hot tile can hold hundreds of refs) at 2 B/entry is
// ~4 KB — that BLOWS the RP2040 core1 stack (SDK default PICO_CORE1_STACK_SIZE
// = 2 KB; the dual-core raster runs on the core stacks). So we cap the XLU
// gather at a MODEST 256 (512 B on stack, comfortable) and DROP-WITH-COUNT on
// overflow — mirroring TileBin.dropped: never corrupt, surface the count via
// raster_xlu_dropped(). The XLU footprint is the tree FOLIAGE (the scene's only
// translucent geometry, ~252 tree tris total spread across the 16 screen-tiles
// at terrain density); 256 XLU tris in ONE 60x60 tile is far beyond any
// realistic foliage density, so a drop here is a re-surface signal, not an
// expected steady state.
enum { RASTER_XLU_TILE_CAP = 256 };

// Drop-with-count for the XLU gather overflow (file-scope static, surfaced via
// the accessor below). Encapsulated state reached only through module
// functions (CLAUDE.md: file-scope static behind accessors == encapsulation).
// NOTE: not atomic. The dual-core raster partitions tiles disjointly across
// cores, but BOTH cores can bump this counter for their own tiles -> a racy
// increment could lose a count. It is a DEBUG telemetry signal only (never
// feeds the bit-identical framebuffer path); an exact dual-core count is out of
// scope. The framebuffer output stays deterministic regardless (each tile is
// drawn by exactly one worker; the drop policy is per-tile-local).
static uint32_t s_xlu_dropped = 0;

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
  int32_t area2;      // 2*signed area (Q24.8), > 0 after normalize
  int tl0, tl1, tl2;  // top-left flags for edges v0->v1, v1->v2, v2->v0
  uint16_t color;     // flat fill (provoking vertex v0)
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

  int32_t area2 = edge_eval(x0, y0, x1, y1, x2, y2);
  int32_t const mag = (area2 < 0) ? -area2 : area2;
  if (mag <= RASTER_AREA_EPS) {
    return RDR_EDEGENERATE;
  }
  if (area2 < 0) {
    // Swap v1<->v2 to normalize to positive area. CRITICAL: EVERY per-vertex
    // attribute (position, iw, u_iw, v_iw, rgba) MUST swap in lockstep — a
    // transposition here is the classic textured-raster bug (R.1 re-surface
    // trigger). Keep this block exhaustive.
    fx12_4 const tx = x1;
    fx12_4 const ty = y1;
    int32_t const tiw = iw1;
    int32_t const tuiw = uiw1;
    int32_t const tviw = viw1;
    uint16_t const trg = rg1;
    x1 = x2;
    y1 = y2;
    iw1 = iw2;
    uiw1 = uiw2;
    viw1 = viw2;
    rg1 = rg2;
    x2 = tx;
    y2 = ty;
    iw2 = tiw;
    uiw2 = tuiw;
    viw2 = tviw;
    rg2 = trg;
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
  t->area2 = area2;
  t->tl0 = edge_is_top_left(x0, y0, x1, y1);
  t->tl1 = edge_is_top_left(x1, y1, x2, y2);
  t->tl2 = edge_is_top_left(x2, y2, x0, y0);
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

// Affine (screen-linear) interpolation of one packed-565 shade channel set at a
// pixel: out8 = sum(w_i * chan_i) / area2, truncated. N64 RDP SHADE is NOT
// perspective-corrected (only UV is). The divide reuses the signed num/area2
// truncation form (P3-5 parity) so host<->device match.
static uint8_t gouraud_chan(int32_t w0, int32_t w1, int32_t w2, int c0, int c1,
                            int c2, int32_t area2) {
  int64_t const num =
      ((int64_t)w0 * c0) + ((int64_t)w1 * c1) + ((int64_t)w2 * c2);
  int32_t v = (int32_t)(num / (int64_t)area2);
  if (v < 0) {
    v = 0;
  }
  if (v > 255) {
    v = 255;
  }
  return (uint8_t)v;
}

// Rasterize one already-setup triangle into the framebuffer + tile Z scratch.
// tile_px_x0/y0 = the tile's top-left pixel in screen space. `rs` selects the
// per-pixel path: flat-fill fast path (no valid texture, BIT-IDENTICAL to the
// pre-R.1 rasterizer) or textured + Gouraud + combiner (valid texture).
static void raster_one(const struct TriSetup* t, const struct RenderState* rs,
                       int tile_px_x0, int tile_px_y0, uint16_t* fb,
                       uint16_t* zbuf) {
  int const textured = rs_has_texture(rs);
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

      if (!textured) {
        // FAST PATH (BIT-IDENTICAL to the pre-R.1 rasterizer): flat fill.
        // w-buffer: larger inv_w is closer. Pass if at least as close as
        // stored.
        if (znew < zbuf[zi]) {
          continue;
        }
        zbuf[zi] = znew;
        fb[(sy * RDR_SCREEN_W) + sx] = t->color;
        continue;
      }

      // TEXTURED + GOURAUD + COMBINER PATH (R.1, opaque pass).
      // Perspective-correct UV: affine-interp u_iw/v_iw (same num/area2 form as
      // inv_w), then recover the Q16.16 texel coord.
      int64_t const num_u = ((int64_t)w0 * t->u_iw0) +
                            ((int64_t)w1 * t->u_iw1) + ((int64_t)w2 * t->u_iw2);
      int64_t const num_v = ((int64_t)w0 * t->v_iw0) +
                            ((int64_t)w1 * t->v_iw1) + ((int64_t)w2 * t->v_iw2);
      int32_t const u_iw_p = (int32_t)(num_u / (int64_t)t->area2);
      int32_t const v_iw_p = (int32_t)(num_v / (int64_t)t->area2);
      fx16_16 const u_q16 = perspective_texcoord_q16(u_iw_p, inv_w);
      fx16_16 const v_q16 = perspective_texcoord_q16(v_iw_p, inv_w);

      // Gouraud shade (affine; NOT perspective-corrected — N64 RDP convention).
      uint8_t const gr = gouraud_chan(w0, w1, w2, (int)sr[0], (int)sr[1],
                                      (int)sr[2], t->area2);
      uint8_t const gg = gouraud_chan(w0, w1, w2, (int)sg[0], (int)sg[1],
                                      (int)sg[2], t->area2);
      uint8_t const gb = gouraud_chan(w0, w1, w2, (int)sb[0], (int)sb[1],
                                      (int)sb[2], t->area2);
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
      uint16_t const out = shade_pixel(rs, texel565, shade565, &keep);
      if (!keep) {
        continue;  // forward-compat: honor a future alpha-compare in the
                   // combiner.
      }
      // z-test AFTER cutout/keep so a discarded fragment never seeds depth.
      if (znew < zbuf[zi]) {
        continue;
      }
      zbuf[zi] = znew;
      fb[(sy * RDR_SCREEN_W) + sx] = out;
    }
  }
}

// Rasterize ONE already-setup TRANSLUCENT (XLU) triangle (R.2, sweep 2). Shares
// the opaque path's coverage + perspective-UV + Gouraud + combiner, but the
// fragment store differs in TWO ways:
//   - z-TEST only, NO z-write (so a later, FARTHER XLU fragment is not blocked
//     by an earlier nearer one — the back-to-front sort owns ordering, not the
//     depth buffer);
//   - the store is texel-alpha alpha-over: fb = blend_pixel_alpha(BLEND_ALPHA,
//     combiner_out, texel_alpha, fb) where texel_alpha = the sampled texel's
//     own alpha (rgba[3]) — P3-3: XLU blends on TEXEL0 alpha, NOT coverage.
// XLU materials are ALWAYS textured here (the flat fast path is opaque-only and
// stays in sweep 1); a non-textured XLU material would write nothing, so we
// gate on a valid texture and skip otherwise. `rs->alpha_cmp` is honored
// (discard below threshold) for generality, though the tree XLU material does
// not set it.
// `zbuf` is const here: the XLU sweep z-TESTS but never WRITES depth (the no-
// z-write invariant, enforced at the type level — a write would not compile).
static void raster_one_xlu(const struct TriSetup* t,
                           const struct RenderState* rs, int tile_px_x0,
                           int tile_px_y0, uint16_t* fb,
                           const uint16_t* zbuf) {
  if (!rs_has_texture(rs)) {
    return;  // XLU without a texture contributes nothing (flat path is opaque).
  }
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

  for (int sy = bb_miny; sy < bb_maxy; ++sy) {
    fx12_4 const cy = (fx12_4)((sy << RASTER_SUBPX) + RASTER_HALF);
    for (int sx = bb_minx; sx < bb_maxx; ++sx) {
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
      int64_t const num = ((int64_t)w0 * (int64_t)t->iw0) +
                          ((int64_t)w1 * (int64_t)t->iw1) +
                          ((int64_t)w2 * (int64_t)t->iw2);
      int32_t const inv_w = (int32_t)(num / (int64_t)t->area2);

      int const tile_x = sx - tile_px_x0;
      int const tile_y = sy - tile_px_y0;
      int const zi = (tile_y * RDR_TILE_W) + tile_x;
      uint16_t const znew = depth_pack(inv_w);

      // z-TEST against the opaque depth left by sweep 1 (NO z-write — see the
      // function comment). Reject a fragment behind the stored opaque depth.
      if (znew < zbuf[zi]) {
        continue;
      }

      // Perspective-correct UV (same num/area2 form as the opaque path).
      int64_t const num_u = ((int64_t)w0 * t->u_iw0) +
                            ((int64_t)w1 * t->u_iw1) + ((int64_t)w2 * t->u_iw2);
      int64_t const num_v = ((int64_t)w0 * t->v_iw0) +
                            ((int64_t)w1 * t->v_iw1) + ((int64_t)w2 * t->v_iw2);
      int32_t const u_iw_p = (int32_t)(num_u / (int64_t)t->area2);
      int32_t const v_iw_p = (int32_t)(num_v / (int64_t)t->area2);
      fx16_16 const u_q16 = perspective_texcoord_q16(u_iw_p, inv_w);
      fx16_16 const v_q16 = perspective_texcoord_q16(v_iw_p, inv_w);

      // Gouraud shade (affine; N64 RDP convention) — same as opaque.
      uint8_t const gr = gouraud_chan(w0, w1, w2, (int)sr[0], (int)sr[1],
                                      (int)sr[2], t->area2);
      uint8_t const gg = gouraud_chan(w0, w1, w2, (int)sg[0], (int)sg[1],
                                      (int)sg[2], t->area2);
      uint8_t const gb = gouraud_chan(w0, w1, w2, (int)sb[0], (int)sb[1],
                                      (int)sb[2], t->area2);
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
      uint16_t const combined = shade_pixel(rs, texel565, shade565, &keep);
      if (!keep) {
        continue;  // forward-compat combiner alpha-compare.
      }
      // Texel-alpha alpha-over the current framebuffer pixel. NO z-write.
      int const fbi = (sy * RDR_SCREEN_W) + sx;
      fb[fbi] = blend_pixel_alpha(BLEND_ALPHA, combined, rgba[3], fb[fbi]);
    }
  }
}

// One XLU triangle's per-tile sort entry: the back-to-front depth KEY (the sum
// of the 3 verts' inv_w; ASCENDING inv_w == farthest-first) plus the gathered
// bin order for a DETERMINISTIC tie-break. qsort is NOT stable, so the order
// index is folded into the comparator -> the rasterized order is reproducible
// (the dual==serial bit-identical invariant depends on this).
struct XluKey {
  int64_t depth;   // sum of the 3 verts' inv_w (Q16.16, may exceed int32)
  uint32_t order;  // gather order (bin index) — the tie-break
  uint16_t bin_i;  // index into bin->refs to rasterize
};

// qsort comparator: ascending depth (farthest first), then ascending gather
// order. Returns -1/0/+1 only (no subtraction — the int64 keys would overflow
// an int return).
static int xlu_key_cmp(const void* pa, const void* pb) {
  const struct XluKey* a = (const struct XluKey*)pa;
  const struct XluKey* b = (const struct XluKey*)pb;
  if (a->depth < b->depth) {
    return -1;
  }
  if (a->depth > b->depth) {
    return 1;
  }
  if (a->order < b->order) {
    return -1;
  }
  if (a->order > b->order) {
    return 1;
  }
  return 0;
}

void raster_tile_noclear(int tile, const struct TileBin* bin,
                         const struct TVtx* pool, uint16_t* fb, uint16_t* zbuf,
                         const struct RenderState* rstate_table) {
  if (bin == 0 || pool == 0 || fb == 0 || zbuf == 0 || rstate_table == 0) {
    return;
  }
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
  struct XluKey xlu[RASTER_XLU_TILE_CAP];
  uint32_t xlu_n = 0;
  for (uint32_t i = 0; i < bin->count; ++i) {
    const struct TriRef* ref = &bin->refs[i];
    if (rstate_table[ref->material].zmode == ZMODE_XLU) {
      // Defer to sweep 2: gather the depth key (sum of the 3 verts' inv_w; the
      // PRE-setup verts — the sort key is order-invariant to the winding swap)
      // with the gather order as a deterministic tie-break. Drop-with-count on
      // overflow (mirrors TileBin.dropped; never corrupt).
      if (xlu_n < (uint32_t)RASTER_XLU_TILE_CAP) {
        xlu[xlu_n].depth = (int64_t)pool[ref->v0].inv_w +
                           (int64_t)pool[ref->v1].inv_w +
                           (int64_t)pool[ref->v2].inv_w;
        xlu[xlu_n].order = i;
        xlu[xlu_n].bin_i = (uint16_t)i;
        ++xlu_n;
      } else {
        ++s_xlu_dropped;
      }
      continue;
    }
    struct TriSetup t;
    int const rc =
        tri_setup(&t, &pool[ref->v0], &pool[ref->v1], &pool[ref->v2]);
    if (rc != RDR_OK) {
      continue;  // degenerate -> rejected, no fill
    }
    // TriRef.material indexes the interned render-state table; it selects the
    // per-pixel path (flat fast path vs textured/Gouraud/combiner).
    const struct RenderState* rs = &rstate_table[ref->material];
    raster_one(&t, rs, tile_px_x0, tile_px_y0, fb, zbuf);
  }

  // SWEEP 2 — TRANSLUCENT. Sort the gathered XLU tris BACK-TO-FRONT (ascending
  // inv_w = farthest first) with the deterministic bin-order tie-break, then
  // composite each over the framebuffer (z-test against sweep-1 depth, NO
  // z-write, texel-alpha alpha-over). Each tile is drawn by exactly ONE worker,
  // so a deterministic sort makes serial == 2-worker bit-identical.
  if (xlu_n > 1) {
    qsort(xlu, (size_t)xlu_n, sizeof xlu[0], xlu_key_cmp);
  }
  for (uint32_t k = 0; k < xlu_n; ++k) {
    const struct TriRef* ref = &bin->refs[xlu[k].bin_i];
    struct TriSetup t;
    int const rc =
        tri_setup(&t, &pool[ref->v0], &pool[ref->v1], &pool[ref->v2]);
    if (rc != RDR_OK) {
      continue;  // degenerate -> rejected, no fill
    }
    const struct RenderState* rs = &rstate_table[ref->material];
    raster_one_xlu(&t, rs, tile_px_x0, tile_px_y0, fb, zbuf);
  }
}

// Debug telemetry: total XLU tris dropped (per-tile gather >
// RASTER_XLU_TILE_CAP) since process start. NEVER feeds the framebuffer path
// (drops degrade the translucent layer gracefully without corruption); a
// nonzero value is a re-surface signal that the XLU per-tile cap needs raising
// (or a Φ frame.h scratch). See RASTER_XLU_TILE_CAP. Not atomic across cores
// (debug only).
uint32_t raster_xlu_dropped(void) { return s_xlu_dropped; }

void raster_tile(int tile, const struct TileBin* bin, const struct TVtx* pool,
                 uint16_t* fb, uint16_t* zbuf,
                 const struct RenderState* rstate_table) {
  if (zbuf == 0) {
    return;
  }
  // Per-tile Z clear (spec: clear Z scratch at the start of each tile), then
  // rasterize. The clear + draw are split so a depth-hazard demonstration
  // (tests/sched: two workers sharing one un-recleared scratch) can drive the
  // real rasterizer without re-clearing; production callers use raster_tile.
  for (int i = 0; i < RDR_TILE_W * RDR_TILE_H; ++i) {
    zbuf[i] = RASTER_Z_CLEAR;
  }
  raster_tile_noclear(tile, bin, pool, fb, zbuf, rstate_table);
}
