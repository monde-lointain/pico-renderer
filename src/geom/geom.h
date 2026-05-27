// geom.h — module interface (Stream A contract + B.1-beta module-local).
// Orthodox C++.
//
// Front end: command interpret -> matrix stack -> transform+light verts ->
// fog factor -> near + guard-band clip -> project -> backface cull ->
// triangle setup-lite -> tile binning into the frozen TVtx / TileBin layout.
//
// CONTRACT NOTE (gap reported to Lead): the frozen entry `geom_run(cmds,
// Frame*)` routes output through `struct Frame`, but Frame (rdr/rdr.cc) is
// Lead-owned and currently carries only {fb,width,height} — no TVtx pool, tile
// bins, matrix/viewport/material state, or clip scratch. So geom CANNOT emit
// through Frame yet. Until the C1 integration extends Frame, geom_run is a thin
// stub and ALL behavior lives in the testable module functions below, driven by
// explicit POD (GeomState + GeomOut). Tests validate those against the float
// oracle. See WORKFLOW.md "contract gaps".
//
// Lighting (PINNED, W1): pre-lit RGBA path PLUS one directional light + one
// ambient term. N lights / specular / env-texgen are W4. Light dir is supplied
// in post-modelview space (LightState.dirs[0], ambient[]).
#ifndef RDR_GEOM_H
#define RDR_GEOM_H
#include "clip/clip.h"  // CLIP_MAX_OUT, clip helpers
#include "fixed/fixed.h"
#include "rdr/config.h"
#include "rdr/types.h"

RdrErr geom_run(const struct Command* cmds, struct Frame* f);

// ---- module-local pipeline state (POD; built by the command interpreter) ----
#define GEOM_MTX_STACK_DEPTH 16

// Matrix stack: a modelview stack + a single projection slot, plus the composed
// MVP cached when either changes. Column-major Q16.16 (rdr/types.h Mat4fx).
struct MtxStack {
  struct Mat4fx modelview[GEOM_MTX_STACK_DEPTH];
  int mv_top;  // index of current modelview (>=0)
  struct Mat4fx projection;
  struct Mat4fx mvp;  // projection * modelview[mv_top], recomputed on change
  int mvp_dirty;
};
void mtx_init(struct MtxStack* s);
// Load (push==0) or push-then-load (push==1) a matrix into target.
RdrErr mtx_set(struct MtxStack* s, int target, int push,
               const struct Mat4fx* m);
RdrErr mtx_pop(struct MtxStack* s, int target);
// Recompute mvp = projection * modelview[top] if dirty; returns &s->mvp.
const struct Mat4fx* mtx_mvp(struct MtxStack* s);

// Viewport in the oracle's center/extent convention (W1-02). Built from the
// frozen CMD_SET_VIEWPORT {x,y,w,h} so goldens match tests/harness/oracle.cc.
struct Viewport {
  int x, y, w, h;
};
void vp_from_cmd(struct Viewport* vp, int x, int y, int w, int h);

// ---- transform / project ---------------------------------------------------
// Transform one model-space Vtx through `mvp` to homogeneous clip space.
void geom_transform_clip(struct Vec4fx* out, const struct Mat4fx* mvp,
                         const struct Vtx* v);

// Project a clip-space vertex (must have clip.w > 0) to a screen-space TVtx via
// perspective divide + viewport map. `u`,`v` are raw S10.5 texcoords carried as
// u*inv_w / v*inv_w. Returns 0 ok, RDR_EDEGENERATE if w<=0 (caller near-clips
// first). rgba is the already-shaded packed color.
RdrErr geom_project(struct TVtx* out, const struct Vec4fx* clip,
                    const struct Viewport* vp, int16_t u, int16_t v,
                    uint16_t rgba);

// ---- lighting --------------------------------------------------------------
// Compute the packed shade color for one vertex. lit==0: pass the pre-lit
// rgba straight through (packed from c.rgba). lit==1: one directional light +
// ambient applied to a base white, modulated by the vertex normal (c.nrm.n);
// light dir is post-modelview (no normal-matrix transform in W1). Returns the
// packed 565/4444 color matching the active RDR_COLOR_* build.
uint16_t geom_shade_vertex(const struct Vtx* v, const struct LightState* ls,
                           int lit);

// ---- fog -------------------------------------------------------------------
// Fog factor in Q16.16 [0..1]: 0 at/<=near_z, 1 at/>=far_z, linear between.
// `z` is view-space-ish depth proxy (here: clip-space w, monotone in depth).
fx16_16 geom_fog_factor(const struct FogState* fog, fx16_16 z);

// ---- backface cull ---------------------------------------------------------
// Signed screen-space area*2 of the triangle (edge function of the three
// screen positions, Q12.4). Sign encodes screen winding.
int32_t geom_signed_area2(const struct TVtx* a, const struct TVtx* b,
                          const struct TVtx* c);
// Returns nonzero if the triangle should be DROPPED for the given cull mode.
// `area2` = geom_signed_area2(...); `det_sign` = sign of the modelview
// determinant (+1, 0, -1) — winding flips with a mirrored modelview (design
// "flipping with the modelview determinant sign"). Degenerate (area2==0) is
// always dropped.
int geom_cull_backface(int32_t area2, int det_sign, int cull_mode);
// Sign of a 3x3 (upper-left) modelview determinant: +1 / 0 / -1.
int geom_modelview_det_sign(const struct Mat4fx* mv);

// ---- tile binning ----------------------------------------------------------
// Per-tile bins for the screen, row-major (RDR_SCREEN_W/H, RDR_TILE_W/H).
#define GEOM_TILES_X ((RDR_SCREEN_W + RDR_TILE_W - 1) / RDR_TILE_W)
#define GEOM_TILES_Y ((RDR_SCREEN_H + RDR_TILE_H - 1) / RDR_TILE_H)
#define GEOM_NUM_TILES (GEOM_TILES_X * GEOM_TILES_Y)

// Output sink: transformed-vertex pool + tile bins + counters. Arena-backed by
// the caller; geom only appends. POD.
struct GeomOut {
  struct TVtx* tverts;  // pool, capacity tvert_cap
  uint32_t tvert_count;
  uint32_t tvert_cap;
  struct TileBin tiles[GEOM_NUM_TILES];
  uint32_t tris_total;    // accepted triangles (binned, counted vs cap)
  uint32_t tris_dropped;  // dropped on overflow / cull / clip
};
// Initialize bins to empty using caller-provided ref storage (one segment per
// tile, each of `refs_per_tile` capacity). tverts pool/cap set separately.
void geom_out_init(struct GeomOut* o, struct TVtx* tverts, uint32_t tvert_cap,
                   struct TriRef* refs, uint32_t refs_per_tile);

// Append a transformed vertex to the pool; returns its index, or UINT32_MAX on
// overflow.
uint32_t geom_emit_tvert(struct GeomOut* o, const struct TVtx* v);

// Bin a triangle (already-clipped, projected, culled) referencing pool indices
// v0,v1,v2 with material id. Computes the screen bbox, appends a TriRef to
// every overlapped tile. Overflow drops+counts (never corrupts). Returns RDR_OK
// or RDR_EOVERFLOW.
RdrErr geom_bin_tri(struct GeomOut* o, uint16_t v0, uint16_t v1, uint16_t v2,
                    uint16_t material);

#endif  // RDR_GEOM_H
