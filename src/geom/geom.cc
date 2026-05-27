// geom.cc — front-end geometry (B.1-beta). Orthodox C++.
//
// Integer-only pipeline (no FPU on RP2040): matrix stack -> transform ->
// lighting/fog -> project (perspective divide + viewport map) -> backface cull
// -> tile binning. Validated against the host float oracle within tolerance.
//
// CONTRACT GAP (see geom.h): `Frame` carries no pool/bins/state today, so
// geom_run is a stub; the testable behavior is the module functions below.
#include "geom/geom.h"

#include <stdint.h>
#include <string.h>

#include "fixed/fixed.h"

#define FX_ONE (1 << 16)

// ---- color packing (matches oracle_unpack565 / gfx rgb565) -----------------
static uint16_t pack565(int r, int g, int b) {
  if (r < 0) {
    r = 0;
  }
  if (r > 255) {
    r = 255;
  }
  if (g < 0) {
    g = 0;
  }
  if (g > 255) {
    g = 255;
  }
  if (b < 0) {
    b = 0;
  }
  if (b > 255) {
    b = 255;
  }
  return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

// ---- matrix stack ----------------------------------------------------------
void mtx_init(struct MtxStack* s) {
  mat4_identity(&s->modelview[0]);
  s->mv_top = 0;
  mat4_identity(&s->projection);
  s->mvp_dirty = 1;
}

RdrErr mtx_set(struct MtxStack* s, int target, int push,
               const struct Mat4fx* m) {
  if (target == MTX_PROJECTION) {
    s->projection = *m;
    s->mvp_dirty = 1;
    return RDR_OK;
  }
  // MTX_MODELVIEW
  if (push) {
    if (s->mv_top + 1 >= GEOM_MTX_STACK_DEPTH) {
      return RDR_EOVERFLOW;
    }
    ++s->mv_top;
  }
  s->modelview[s->mv_top] = *m;
  s->mvp_dirty = 1;
  return RDR_OK;
}

RdrErr mtx_pop(struct MtxStack* s, int target) {
  if (target == MTX_PROJECTION) {
    return RDR_EINVAL;  // single projection slot; nothing to pop
  }
  if (s->mv_top <= 0) {
    return RDR_EINVAL;  // base modelview must remain
  }
  --s->mv_top;
  s->mvp_dirty = 1;
  return RDR_OK;
}

const struct Mat4fx* mtx_mvp(struct MtxStack* s) {
  if (s->mvp_dirty) {
    mat4_mul(&s->mvp, &s->projection, &s->modelview[s->mv_top]);
    s->mvp_dirty = 0;
  }
  return &s->mvp;
}

// ---- viewport (oracle center/extent convention, W1-02) ---------------------
void vp_from_cmd(struct Viewport* vp, int x, int y, int w, int h) {
  vp->x = x;
  vp->y = y;
  vp->w = w;
  vp->h = h;
}

// ---- transform / project ---------------------------------------------------
void geom_transform_clip(struct Vec4fx* out, const struct Mat4fx* mvp,
                         const struct Vtx* v) {
  struct Vec3fx p;
  p.x = fx_from_int(v->pos[0]);
  p.y = fx_from_int(v->pos[1]);
  p.z = fx_from_int(v->pos[2]);
  mat4_transform_point(out, mvp, &p);
}

RdrErr geom_project(struct TVtx* out, const struct Vec4fx* clip,
                    const struct Viewport* vp, int16_t u, int16_t v,
                    uint16_t rgba) {
  if (clip->w <= 0) {
    return RDR_EDEGENERATE;  // behind near plane; caller near-clips first
  }
  // inv_w = 1/w in Q16.16 (fx_div widens the dividend; truncating divide).
  fx_invw const inv_w = fx_div(FX_ONE, clip->w);
  // NDC = clip.xy * inv_w (Q16.16).
  fx16_16 const ndc_x = fx_mul(clip->x, inv_w);
  fx16_16 const ndc_y = fx_mul(clip->y, inv_w);

  // Viewport map (oracle convention): center = origin + half-extent,
  // screen = center + ndc * half-extent; Y flips (screen grows downward).
  // half-extents are integer pixels -> Q16.16 for the multiply.
  fx16_16 const half_w = fx_from_int(vp->w) >> 1;
  fx16_16 const half_h = fx_from_int(vp->h) >> 1;
  fx16_16 const cx = fx_from_int(vp->x) + half_w;
  fx16_16 const cy = fx_from_int(vp->y) + half_h;
  fx16_16 const sx = cx + fx_mul(ndc_x, half_w);
  fx16_16 const sy = cy - fx_mul(ndc_y, half_h);

  // Q16.16 px -> Q12.4: shift right by 12 with round-to-nearest.
  out->x = (fx12_4)((sx + (1 << 11)) >> 12);
  out->y = (fx12_4)((sy + (1 << 11)) >> 12);
  out->inv_w = inv_w;
  // u,v are S10.5 texcoords; carry as u*inv_w / v*inv_w (Q16.16 then narrowed).
  out->u_iw = (int16_t)fx_to_int(fx_mul(fx_from_int(u), inv_w));
  out->v_iw = (int16_t)fx_to_int(fx_mul(fx_from_int(v), inv_w));
  out->rgba = rgba;
  return RDR_OK;
}

// ---- lighting --------------------------------------------------------------
uint16_t geom_shade_vertex(const struct Vtx* v, const struct LightState* ls,
                           int lit) {
  if (!lit) {
    return pack565((int)v->c.rgba[0], (int)v->c.rgba[1], (int)v->c.rgba[2]);
  }
  // Single directional light + ambient (W1 pinned scope). Normal and light dir
  // are post-modelview, packed as signed 8-bit. Diffuse = max(0, n.l) of the
  // normalized vectors. All integer.
  struct Vec3fx n;
  n.x = fx_from_int((int)v->c.nrm.n[0]);
  n.y = fx_from_int((int)v->c.nrm.n[1]);
  n.z = fx_from_int((int)v->c.nrm.n[2]);
  vec3_normalize(&n, &n);

  int r = (int)ls->ambient[0];
  int g = (int)ls->ambient[1];
  int b = (int)ls->ambient[2];

  if (ls->count > 0) {
    struct Vec3fx l;
    l.x = fx_from_int((int)ls->dirs[0].dir[0]);
    l.y = fx_from_int((int)ls->dirs[0].dir[1]);
    l.z = fx_from_int((int)ls->dirs[0].dir[2]);
    vec3_normalize(&l, &l);
    fx16_16 ndotl = vec3_dot(&n, &l);  // Q16.16 in [-1,1]
    if (ndotl < 0) {
      ndotl = 0;
    }
    // diffuse channel = lightcolor * ndotl, color in [0,255].
    r += fx_to_int(fx_mul(fx_from_int((int)ls->dirs[0].rgb[0]), ndotl));
    g += fx_to_int(fx_mul(fx_from_int((int)ls->dirs[0].rgb[1]), ndotl));
    b += fx_to_int(fx_mul(fx_from_int((int)ls->dirs[0].rgb[2]), ndotl));
  }
  return pack565(r, g, b);
}

// ---- fog -------------------------------------------------------------------
fx16_16 geom_fog_factor(const struct FogState* fog, fx16_16 z) {
  if (z <= fog->near_z) {
    return 0;
  }
  if (z >= fog->far_z) {
    return FX_ONE;
  }
  fx16_16 const range = fog->far_z - fog->near_z;
  if (range <= 0) {
    return FX_ONE;
  }
  return (fx16_16)fx_div(z - fog->near_z, range);
}

// ---- backface cull ---------------------------------------------------------
int32_t geom_signed_area2(const struct TVtx* a, const struct TVtx* b,
                          const struct TVtx* c) {
  // Edge function (b-a) x (c-a) in Q12.4 screen coords. 64-bit intermediate
  // (the Q12.4*Q12.4 product can exceed 32 bits). Only the SIGN matters to
  // callers, so collapse to {-1,0,1}.
  int64_t const abx = (int64_t)b->x - (int64_t)a->x;
  int64_t const aby = (int64_t)b->y - (int64_t)a->y;
  int64_t const acx = (int64_t)c->x - (int64_t)a->x;
  int64_t const acy = (int64_t)c->y - (int64_t)a->y;
  int64_t const area = (abx * acy) - (aby * acx);
  if (area > 0) {
    return 1;
  }
  if (area < 0) {
    return -1;
  }
  return 0;
}

int geom_modelview_det_sign(const struct Mat4fx* mv) {
  // Sign of the upper-left 3x3 determinant (column-major index col*4+row).
  // Evaluate in Q16.16 via fx_mul; only the sign matters.
  fx16_16 const a = mv->m[0];
  fx16_16 const d = mv->m[1];
  fx16_16 const gg = mv->m[2];
  fx16_16 const bb = mv->m[4];
  fx16_16 const e = mv->m[5];
  fx16_16 const h = mv->m[6];
  fx16_16 const cc = mv->m[8];
  fx16_16 const f = mv->m[9];
  fx16_16 const i = mv->m[10];
  fx16_16 const t0 = fx_mul(a, fx_mul(e, i) - fx_mul(f, h));
  fx16_16 const t1 = fx_mul(bb, fx_mul(d, i) - fx_mul(f, gg));
  fx16_16 const t2 = fx_mul(cc, fx_mul(d, h) - fx_mul(e, gg));
  fx16_16 const det = t0 - t1 + t2;
  if (det > 0) {
    return 1;
  }
  if (det < 0) {
    return -1;
  }
  return 0;
}

int geom_cull_backface(int32_t area2, int det_sign, int cull_mode) {
  if (cull_mode == CULL_NONE) {
    return (area2 == 0) ? 1 : 0;  // still drop degenerates
  }
  if (cull_mode == CULL_BOTH) {
    return 1;
  }
  if (area2 == 0) {
    return 1;  // degenerate
  }
  // Front-facing in NDC is CCW; after the viewport Y-flip a front face has
  // NEGATIVE screen-space area (oracle edge_fn convention). A mirrored
  // modelview (det<0) flips winding, so fold the determinant sign in.
  int eff = (int)area2;
  if (det_sign < 0) {
    eff = -eff;
  }
  int const front = (eff < 0);  // pinned to the oracle (see geom_test)
  if (cull_mode == CULL_BACK) {
    return front ? 0 : 1;  // drop back faces
  }
  // CULL_FRONT
  return front ? 1 : 0;
}

// ---- tile binning ----------------------------------------------------------
void geom_out_init(struct GeomOut* o, struct TVtx* tverts, uint32_t tvert_cap,
                   struct TriRef* refs, uint32_t refs_per_tile) {
  o->tverts = tverts;
  o->tvert_count = 0;
  o->tvert_cap = tvert_cap;
  o->tris_total = 0;
  o->tris_dropped = 0;
  for (int i = 0; i < GEOM_NUM_TILES; ++i) {
    o->tiles[i].refs = &refs[(size_t)i * refs_per_tile];
    o->tiles[i].count = 0;
    o->tiles[i].cap = refs_per_tile;
    o->tiles[i].dropped = 0;
  }
}

uint32_t geom_emit_tvert(struct GeomOut* o, const struct TVtx* v) {
  if (o->tvert_count >= o->tvert_cap) {
    return UINT32_MAX;
  }
  o->tverts[o->tvert_count] = *v;
  return o->tvert_count++;
}

// Map a Q12.4 screen coord to a tile index in [0, ntiles).
static int tile_index(int32_t coord_q4, int tile_px, int ntiles) {
  int32_t px = coord_q4 >> 4;  // Q12.4 -> integer pixel (floor)
  if (px < 0) {
    px = 0;
  }
  int idx = (int)(px / tile_px);
  if (idx < 0) {
    idx = 0;
  }
  if (idx >= ntiles) {
    idx = ntiles - 1;
  }
  return idx;
}

RdrErr geom_bin_tri(struct GeomOut* o, uint16_t v0, uint16_t v1, uint16_t v2,
                    uint16_t material) {
  const struct TVtx* a = &o->tverts[v0];
  const struct TVtx* b = &o->tverts[v1];
  const struct TVtx* c = &o->tverts[v2];

  // Screen bbox in Q12.4.
  int32_t minx = a->x;
  int32_t maxx = a->x;
  int32_t miny = a->y;
  int32_t maxy = a->y;
  if (b->x < minx) {
    minx = b->x;
  }
  if (b->x > maxx) {
    maxx = b->x;
  }
  if (c->x < minx) {
    minx = c->x;
  }
  if (c->x > maxx) {
    maxx = c->x;
  }
  if (b->y < miny) {
    miny = b->y;
  }
  if (b->y > maxy) {
    maxy = b->y;
  }
  if (c->y < miny) {
    miny = c->y;
  }
  if (c->y > maxy) {
    maxy = c->y;
  }

  int const tx0 = tile_index(minx, RDR_TILE_W, GEOM_TILES_X);
  int const tx1 = tile_index(maxx, RDR_TILE_W, GEOM_TILES_X);
  int const ty0 = tile_index(miny, RDR_TILE_H, GEOM_TILES_Y);
  int const ty1 = tile_index(maxy, RDR_TILE_H, GEOM_TILES_Y);

  int overflowed = 0;
  for (int ty = ty0; ty <= ty1; ++ty) {
    for (int tx = tx0; tx <= tx1; ++tx) {
      struct TileBin* bin = &o->tiles[(ty * GEOM_TILES_X) + tx];
      if (bin->count >= bin->cap) {
        ++bin->dropped;
        overflowed = 1;
        continue;
      }
      struct TriRef* ref = &bin->refs[bin->count++];
      ref->v0 = v0;
      ref->v1 = v1;
      ref->v2 = v2;
      ref->material = material;
    }
  }
  if (overflowed) {
    ++o->tris_dropped;
    return RDR_EOVERFLOW;
  }
  ++o->tris_total;
  return RDR_OK;
}

// ---- frozen entry (stubbed until Frame is extended at C1) ------------------
RdrErr geom_run(const struct Command* cmds, struct Frame* f) {
  (void)cmds;
  (void)f;
  return RDR_OK;
}
