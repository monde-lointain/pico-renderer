// clip.cc — guard-band Sutherland-Hodgman polygon clip (B.1-beta). Orthodox
// C++.
//
// Integer-only so host and device are bit-identical (no FPU on RP2040). The
// per-edge intersection parameter t is formed as a 64-bit ratio of signed
// clip-plane distances and applied to every TVtx attribute via the same
// integer lerp; no floats anywhere.
#include "clip/clip.h"

#include <stdint.h>

// Linear interpolation a + (b-a)*t, t given as numerator/denominator (the
// signed-distance ratio). Rounded to nearest. 64-bit intermediate keeps the
// (b-a)*num product exact for the value ranges used here (Q12.4 coords,
// 16-bit attrs). Same integer path on host and device.
static int32_t lerp_i(int32_t a, int32_t b, int64_t num, int64_t den) {
  if (den == 0) {
    return a;
  }
  int64_t const d = (int64_t)b - (int64_t)a;
  int64_t prod = d * num;
  // Round half away from zero on division.
  if ((prod >= 0) == (den > 0)) {
    prod += (den > 0 ? den : -den) / 2;
  } else {
    prod -= (den > 0 ? den : -den) / 2;
  }
  return (int32_t)(a + (prod / den));
}

static void lerp_vtx(struct TVtx* out, const struct TVtx* a,
                     const struct TVtx* b, int64_t num, int64_t den) {
  out->x = (fx12_4)lerp_i(a->x, b->x, num, den);
  out->y = (fx12_4)lerp_i(a->y, b->y, num, den);
  out->inv_w = (fx_invw)lerp_i(a->inv_w, b->inv_w, num, den);
  out->u_iw = (int16_t)lerp_i(a->u_iw, b->u_iw, num, den);
  out->v_iw = (int16_t)lerp_i(a->v_iw, b->v_iw, num, den);
  // rgba: interpolate per channel? At guard-band scale color seams are far
  // off-screen; a packed lerp is adequate and cheap. Treat as scalar.
  out->rgba = (uint16_t)lerp_i((int32_t)a->rgba, (int32_t)b->rgba, num, den);
}

// Signed distance of a vertex to a single rectangle edge. Positive = inside.
// edge: 0=left(x>=minx) 1=right(x<=maxx) 2=top(y>=miny) 3=bottom(y<=maxy).
static int32_t edge_dist(const struct TVtx* v, const struct ClipRect* r,
                         int edge) {
  switch (edge) {
    case 0:
      return (int32_t)v->x - r->minx;
    case 1:
      return r->maxx - (int32_t)v->x;
    case 2:
      return (int32_t)v->y - r->miny;
    default:
      return r->maxy - (int32_t)v->y;
  }
}

void clip_guard_rect(struct ClipRect* r) {
  // Screen rect [0,W)x[0,H) expanded by the guard band, in Q12.4.
  r->minx = (fx12_4)((-RDR_GUARD_X) * 16);
  r->miny = (fx12_4)((-RDR_GUARD_Y) * 16);
  r->maxx = (fx12_4)((RDR_SCREEN_W + RDR_GUARD_X) * 16);
  r->maxy = (fx12_4)((RDR_SCREEN_H + RDR_GUARD_Y) * 16);
}

// Clip polygon `src` (count `sn`) against a single rectangle edge into `dst`.
// Returns dst count. Standard Sutherland-Hodgman: keep inside verts, emit an
// intersection whenever the inside/outside state flips across an edge.
static int clip_against_edge(const struct TVtx* src, int sn,
                             const struct ClipRect* r, int edge,
                             struct TVtx* dst) {
  if (sn == 0) {
    return 0;
  }
  int dn = 0;
  for (int i = 0; i < sn; ++i) {
    const struct TVtx* cur = &src[i];
    const struct TVtx* prv = &src[(i + sn - 1) % sn];
    int32_t const dc = edge_dist(cur, r, edge);
    int32_t const dp = edge_dist(prv, r, edge);
    int const cur_in = (dc >= 0);
    int const prv_in = (dp >= 0);
    if (cur_in != prv_in) {
      // Crossing: emit intersection. t = dp / (dp - dc).
      if (dn < CLIP_MAX_OUT) {
        lerp_vtx(&dst[dn], prv, cur, (int64_t)dp, (int64_t)dp - (int64_t)dc);
        // The clipped axis is exact at the boundary (axis-aligned edge); snap
        // it so a 1-ulp lerp rounding can never place a vertex outside the
        // window (keeps the output ring watertight against the clip line).
        switch (edge) {
          case 0:
            dst[dn].x = r->minx;
            break;
          case 1:
            dst[dn].x = r->maxx;
            break;
          case 2:
            dst[dn].y = r->miny;
            break;
          default:
            dst[dn].y = r->maxy;
            break;
        }
        ++dn;
      }
    }
    if (cur_in && dn < CLIP_MAX_OUT) {
      dst[dn] = *cur;
      ++dn;
    }
  }
  return dn;
}

int clip_poly_rect(const struct TVtx* in, int n, const struct ClipRect* r,
                   struct TVtx* out) {
  if (n < 3) {
    return 0;
  }
  // Ping-pong between two scratch rings across the 4 edges.
  struct TVtx buf_a[CLIP_MAX_OUT];
  struct TVtx buf_b[CLIP_MAX_OUT];
  const struct TVtx* src = in;
  int sn = n;
  struct TVtx* const scratch[2] = {buf_a, buf_b};
  for (int edge = 0; edge < 4; ++edge) {
    struct TVtx* dst = scratch[edge & 1];
    sn = clip_against_edge(src, sn, r, edge, dst);
    src = dst;
    if (sn == 0) {
      return 0;
    }
  }
  for (int i = 0; i < sn; ++i) {
    out[i] = src[i];
  }
  return sn;
}

int clip_tri(const struct TVtx* in, int n, struct TVtx* out) {
  if (n != 3) {
    return 0;
  }
  struct ClipRect r;
  clip_guard_rect(&r);
  return clip_poly_rect(in, 3, &r, out);
}
