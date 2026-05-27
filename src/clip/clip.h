// clip.h — module interface (Stream A contract + B.1-beta module-local).
// Orthodox C++.
//
// Scope (W1, B.1-beta): screen guard-band polygon clip (Sutherland-Hodgman) of
// an already-projected triangle expressed in screen-space TVtx. The NEAR-plane
// clip happens upstream in `geom` on homogeneous clip-space (Vec4fx) BEFORE the
// perspective divide — TVtx carries no clip-space w, so it cannot be done here;
// the frozen `clip_tri` signature is therefore the guard-band stage only (see
// design spec: "Interior X/Y handled by tile scissor + bbox").
//
// Guard-band: clip to an expanded screen rect (RDR_GUARD_X/Y, a few x screen)
// so band-crossers are true-clipped while in-band geometry is span-clamped at
// raster time. Keeps Q12.4 screen coords in int range.
//
// Output is a CONVEX vertex ring (fan order v0,v1,..,vN-1); the caller (geom)
// re-triangulates it as a fan (N-2 tris). Clip output verts are bounded by
// CLIP_MAX_OUT.
#ifndef RDR_CLIP_H
#define RDR_CLIP_H
#include "rdr/config.h"  // RDR_SCREEN_W/H
#include "rdr/types.h"

// Guard-band half-extents in PIXELS, beyond the screen rect [0,W)x[0,H).
// A ~2x-screen band on each side keeps Q12.4 (~+/-2047 px range) well clear.
#define RDR_GUARD_X (RDR_SCREEN_W * 2)
#define RDR_GUARD_Y (RDR_SCREEN_H * 2)

// Max vertices a single triangle can expand to after clipping against 4 edges.
// Sutherland-Hodgman against a convex window of E edges grows a polygon by at
// most one vertex per edge: 3 + 4 = 7. Pad to 8.
#define CLIP_MAX_OUT 8

// Frozen contract: clip one screen-space triangle (`in`, n==3) against the
// guard-band window. Writes the resulting convex ring into `out` (capacity >=
// CLIP_MAX_OUT) and returns the vertex count (0 if fully clipped, else 3..7).
// `out` must not alias `in`.
int clip_tri(const struct TVtx* in, int n, struct TVtx* out);

// ---- module-local helpers (exposed for unit/golden tests) ------------------
// Guard-band window in Q12.4 (TVtx.x/y units), inclusive bounds.
struct ClipRect {
  fx12_4 minx, miny, maxx, maxy;
};
void clip_guard_rect(struct ClipRect* r);

// Sutherland-Hodgman clip of polygon `in` (count `n`, convex) against rectangle
// `r`. Writes the clipped ring to `out` (cap CLIP_MAX_OUT) and returns the new
// vertex count. Interpolates ALL TVtx attributes linearly in screen space
// (x,y,inv_w,u_iw,v_iw,rgba). Returns 0 if fully outside. `out` != `in`.
int clip_poly_rect(const struct TVtx* in, int n, const struct ClipRect* r,
                   struct TVtx* out);

#endif  // RDR_CLIP_H
