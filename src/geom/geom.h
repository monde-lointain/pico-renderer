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
  uint32_t tris_total;    // accepted triangles (placed in bins by finalize)
  uint32_t tris_dropped;  // dropped on overflow / cull / clip / near
  // Arena-backed variable bins (Wave-E). geom_bin_tri DEFERS: it buffers each
  // surviving tri (a TriRef) into jobs[] and tallies per-tile demand in
  // tiles[].count. geom_bin_finalize prefix-sums those demands into contiguous
  // segments of the shared pool[] and replays jobs IN ORDER. See rdr/frame.h.
  struct TriRef* jobs;  // deferred surviving-tri buffer (caller-owned storage)
  uint32_t jobs_count;  // tris buffered this frame
  uint32_t jobs_cap;    // capacity; overflow drops-with-count (tris_dropped)
  struct TriRef* pool;  // shared ref pool finalize packs per-tile segments into
  uint32_t pool_cap;    // capacity; overflow drops-with-count (tiles[].dropped)
  uint32_t pool_used;   // slots consumed by finalize (high-water; telemetry)
};
// Initialize bins to empty using caller-provided storage: `pool` (the shared
// per-tile ref segments, capacity `pool_cap`) and `jobs` (the deferred
// surviving-tri buffer, capacity `jobs_cap`). tverts pool/cap set separately.
void geom_out_init(struct GeomOut* o, struct TVtx* tverts, uint32_t tvert_cap,
                   struct TriRef* pool, uint32_t pool_cap, struct TriRef* jobs,
                   uint32_t jobs_cap);

// Append a transformed vertex to the pool; returns its index, or UINT32_MAX on
// overflow.
uint32_t geom_emit_tvert(struct GeomOut* o, const struct TVtx* v);

// Bin a triangle (already-clipped, projected, culled) referencing pool indices
// v0,v1,v2 with material id. DEFERRED (Wave-E arena bins): buffers the TriRef
// into jobs[] and tallies per-tile demand (tiles[].count, += 1 per overlapped
// tile) — it does NOT write the pool yet. Call geom_bin_finalize after the
// geometry pass to pack the segments. Returns RDR_OK, or RDR_EOVERFLOW (+counts
// tris_dropped) if the job buffer is full (drop-with-count, never corrupts).
RdrErr geom_bin_tri(struct GeomOut* o, uint16_t v0, uint16_t v1, uint16_t v2,
                    uint16_t material);

// Finalize binning after the geometry pass: prefix-sum the per-tile demand
// (tiles[].count, tallied by geom_bin_tri) into contiguous segments of the
// shared pool, then replay the buffered jobs IN EMISSION ORDER into them. After
// this, tiles[].refs/count are the ready-to-raster per-tile lists (flat +
// contiguous — raster consumption is unchanged, and per-tile order matches the
// old append order so the bit-identical invariant holds). Sets tris_total
// (placed jobs); a job that can't fully place (pool exhausted for some tile)
// counts tris_dropped + tiles[].dropped. geom_run calls this automatically;
// direct geom_bin_tri callers (tests) must call it before reading tiles[].refs.
void geom_bin_finalize(struct GeomOut* o);

// ---- material interning (Wave-D D.4) ---------------------------------------
// Intern the per-frame distinct RenderStates into frame->rstate_table so each
// emitted TriRef.material indexes [0, rstate_count) (resolves C2 latent #2 —
// C1/C2 wrote 0 always). Interned single-core in geom BEFORE the dual-core
// raster reads the table, so the table is immutable during raster (the
// bit-identical invariant holds). Dedup is a deterministic linear VALUE compare
// (memcmp over the RenderState POD) — pointer identity is insufficient because
// geom copies SET_MATERIAL state into the single frame->rstate buffer, so every
// intern sees the same pointer. A few distinct states per scene (≈6), so a
// linear scan over <=RDR_MAX_MATERIALS entries is ample (no hash needed).
//
// Design split (do NOT store 128 RenderStates): the terrain's 128 mesh-cells
// SHARE one RenderState and differ only by texture pointer; that per-cell
// texture is a SEPARATE axis (a later concern), NOT a distinct material. This
// interns RenderState only — material id and texture binding stay separable.

// Reset the interned table to empty (rstate_count := 0) and clear the
// overflow-drop counter. Call once at the start of the frame's command walk.
void geom_material_reset(struct Frame* f);

// Return the interned id for `rs`: the id of an existing value-equal entry
// (dedup), else append a new entry and return its id. On a full table
// (rstate_count == RDR_MAX_MATERIALS) with no match, DROP-WITH-COUNT: the table
// is left untouched, the overflow counter is bumped, and the last valid id is
// returned (clamp; RDR_MAX_MATERIALS-1, or 0 if the table is somehow empty) so
// emitted TriRefs always carry an in-range, never-corrupting id. `rs` defaults
// to &f->rstate when null.
uint16_t geom_material_intern(struct Frame* f, const struct RenderState* rs);

// Count of intern requests dropped on a full table this frame (debug-surfaced,
// mirrors the bin/cmd drop-with-count convention). Reset by
// geom_material_reset.
uint32_t geom_material_overflow_count(void);

#endif  // RDR_GEOM_H
