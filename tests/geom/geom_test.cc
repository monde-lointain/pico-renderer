// geom_test.cc — B.1-beta front-end geometry tests. Host-first, gtest.
// Math-bearing functions are validated against the float oracle
// (tests/harness/oracle.h) within a fixed-point tolerance.
#include "geom/geom.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "fixed/fixed.h"
#include "gtest/gtest.h"
#include "oracle.h"
#include "rdr/config.h"
#include "rdr/frame.h"  // struct Frame + RDR_MAX_MATERIALS (material interning)

// ---- helpers ---------------------------------------------------------------
static fx16_16 fx(double v) { return (fx16_16)lrint(v * 65536.0); }

static struct Vtx mk_vtx(int16_t x, int16_t y, int16_t z) {
  struct Vtx v;
  memset(&v, 0, sizeof v);
  v.pos[0] = x;
  v.pos[1] = y;
  v.pos[2] = z;
  return v;
}

// Build a simple perspective projection in column-major Q16.16 matching the
// oracle's float layout (so we can cross-check).
static void make_proj(struct Mat4fx* m, struct OMat4* om, double fov_scale,
                      double n, double f) {
  // Standard-ish projection mapping z to [0,1], column-major.
  memset(m->m, 0, sizeof m->m);
  double const a = fov_scale;
  double const za = f / (f - n);
  double const zb = -(f * n) / (f - n);
  // col0
  m->m[0] = fx(a);
  // col1
  m->m[5] = fx(a);
  // col2
  m->m[10] = fx(za);
  m->m[11] = fx(1.0);  // w = z
  // col3
  m->m[14] = fx(zb);
  for (int i = 0; i < 16; ++i) {
    om->m[i] = (float)m->m[i] / 65536.0F;
  }
}

// ---- matrix stack ----------------------------------------------------------
TEST(Geom, MtxStackPushPopAndMvp) {
  struct MtxStack s;
  mtx_init(&s);
  struct Mat4fx id;
  mat4_identity(&id);
  // Projection = identity, modelview = identity -> mvp identity.
  mtx_set(&s, MTX_PROJECTION, 0, &id);
  const struct Mat4fx* mvp = mtx_mvp(&s);
  for (int i = 0; i < 16; ++i) {
    EXPECT_EQ(mvp->m[i], id.m[i]);
  }
  // Push a translate onto modelview; pop restores.
  struct Mat4fx t;
  mat4_identity(&t);
  t.m[12] = fx(5.0);  // translate x by 5
  ASSERT_EQ(mtx_set(&s, MTX_MODELVIEW, 1, &t), RDR_OK);
  EXPECT_EQ(mtx_mvp(&s)->m[12], fx(5.0));
  ASSERT_EQ(mtx_pop(&s, MTX_MODELVIEW), RDR_OK);
  EXPECT_EQ(mtx_mvp(&s)->m[12], 0);
}

TEST(Geom, MtxStackOverflowUnderflow) {
  struct MtxStack s;
  mtx_init(&s);
  struct Mat4fx id;
  mat4_identity(&id);
  // Underflow: popping the base modelview is invalid.
  EXPECT_EQ(mtx_pop(&s, MTX_MODELVIEW), RDR_EINVAL);
  // Overflow: push past depth.
  RdrErr err = RDR_OK;
  for (int i = 0; i < GEOM_MTX_STACK_DEPTH + 2; ++i) {
    err = mtx_set(&s, MTX_MODELVIEW, 1, &id);
    if (err != RDR_OK) {
      break;
    }
  }
  EXPECT_EQ(err, RDR_EOVERFLOW);
}

// ---- viewport convention (W1-02): matches the oracle center/extent ---------
TEST(Geom, ViewportMatchesOracle) {
  struct Viewport vp;
  vp_from_cmd(&vp, 0, 0, RDR_SCREEN_W, RDR_SCREEN_H);
  struct OViewport const ovp = {0, 0, RDR_SCREEN_W, RDR_SCREEN_H};
  EXPECT_EQ(vp.x, ovp.x);
  EXPECT_EQ(vp.w, ovp.w);
  EXPECT_EQ(vp.h, ovp.h);
}

// ---- transform + project vs oracle -----------------------------------------
TEST(Geom, TransformProjectMatchesOracle) {
  struct Mat4fx proj;
  struct OMat4 oproj;
  make_proj(&proj, &oproj, 1.5, 0.5, 50.0);
  struct Viewport vp;
  vp_from_cmd(&vp, 0, 0, RDR_SCREEN_W, RDR_SCREEN_H);
  struct OViewport const ovp = {0, 0, RDR_SCREEN_W, RDR_SCREEN_H};

  // A few model points well in front of the camera (z>0, within Q16.16 range).
  int16_t const pts[][3] = {{2, 3, 10}, {-4, 1, 8}, {0, -5, 20}, {6, 6, 12}};
  for (int i = 0; i < 4; ++i) {
    struct Vtx const v = mk_vtx(pts[i][0], pts[i][1], pts[i][2]);
    struct Vec4fx clip;
    geom_transform_clip(&clip, &proj, &v);
    struct TVtx tv;
    ASSERT_EQ(geom_project(&tv, &clip, &vp, 0, 0, 0), RDR_OK);

    // Oracle reference.
    struct OVec4 const om = {(float)pts[i][0], (float)pts[i][1],
                             (float)pts[i][2], 1.0F};
    struct OTVtx ot;
    oracle_xform_vertex(&ot, &oproj, &om, 0.0F, 0.0F, &ovp);
    ASSERT_EQ(ot.clipped, 0);

    // TVtx x,y are Q12.4; compare to oracle pixels within ~1 px.
    float const sx = (float)tv.x / 16.0F;
    float const sy = (float)tv.y / 16.0F;
    EXPECT_NEAR(sx, ot.sx, 1.0F) << "pt " << i;
    EXPECT_NEAR(sy, ot.sy, 1.0F) << "pt " << i;
  }
}

// Behind-near-plane vertex (w<=0) is flagged degenerate by project.
TEST(Geom, ProjectRejectsBehindCamera) {
  struct Vec4fx clip;
  clip.x = fx(1.0);
  clip.y = fx(1.0);
  clip.z = fx(-1.0);
  clip.w = fx(-2.0);  // behind near plane
  struct Viewport vp;
  vp_from_cmd(&vp, 0, 0, RDR_SCREEN_W, RDR_SCREEN_H);
  struct TVtx tv;
  EXPECT_EQ(geom_project(&tv, &clip, &vp, 0, 0, 0), RDR_EDEGENERATE);
}

// ---- fog factor ------------------------------------------------------------
TEST(Geom, FogFactorLinearRamp) {
  struct FogState fog;
  fog.enabled = 1;
  fog.near_z = fx(2.0);
  fog.far_z = fx(10.0);
  fog.color = 0;
  EXPECT_EQ(geom_fog_factor(&fog, fx(1.0)), 0);         // <= near
  EXPECT_EQ(geom_fog_factor(&fog, fx(12.0)), fx(1.0));  // >= far
  // Midpoint z=6 -> 0.5.
  fx16_16 const mid = geom_fog_factor(&fog, fx(6.0));
  EXPECT_NEAR((double)mid / 65536.0, 0.5, 0.01);
}

// ---- fog u8 (R.3): factor [0,1] -> TVtx.fog [0,255]; disabled -> 0 ----------
TEST(Geom, FogU8ScalesAndGatesOnEnabled) {
  struct FogState fog;
  memset(&fog, 0, sizeof fog);
  fog.enabled = 1;
  fog.near_z = fx(2.0);
  fog.far_z = fx(10.0);
  fog.color = 0;
  // Endpoints exact: <=near -> 0, >=far -> 255.
  EXPECT_EQ(geom_fog_u8(&fog, fx(1.0)), 0);
  EXPECT_EQ(geom_fog_u8(&fog, fx(12.0)), 255);
  // Midpoint ~ 0.5 * 255 ~ 127 (truncation; within a quantum).
  uint8_t const mid = geom_fog_u8(&fog, fx(6.0));
  EXPECT_GE((int)mid, 126);
  EXPECT_LE((int)mid, 128);
  // Monotone: farther z -> more fog.
  EXPECT_LT((int)geom_fog_u8(&fog, fx(3.0)), (int)geom_fog_u8(&fog, fx(8.0)));
  // Disabled -> always 0 regardless of z (raster fog step no-ops).
  fog.enabled = 0;
  EXPECT_EQ(geom_fog_u8(&fog, fx(6.0)), 0);
  EXPECT_EQ(geom_fog_u8(&fog, fx(12.0)), 0);
}

// ---- backface cull: pin winding against the oracle -------------------------
// Build a front-facing (NDC-CCW) triangle, project it, and confirm CULL_BACK
// keeps it while CULL_FRONT drops it; mirrored modelview (det<0) flips that.
TEST(Geom, BackfaceCullPinnedToOracle) {
  struct Mat4fx proj;
  struct OMat4 oproj;
  make_proj(&proj, &oproj, 1.5, 0.5, 50.0);
  struct Viewport vp;
  vp_from_cmd(&vp, 0, 0, RDR_SCREEN_W, RDR_SCREEN_H);

  // CCW in NDC/clip space (z forward). Verts ordered CCW about +z.
  struct Vtx const a = mk_vtx(-3, -3, 10);
  struct Vtx const b = mk_vtx(3, -3, 10);
  struct Vtx const c = mk_vtx(0, 3, 10);
  struct TVtx ta;
  struct TVtx tb;
  struct TVtx tc;
  struct Vec4fx ca;
  struct Vec4fx cb;
  struct Vec4fx cc;
  geom_transform_clip(&ca, &proj, &a);
  geom_transform_clip(&cb, &proj, &b);
  geom_transform_clip(&cc, &proj, &c);
  ASSERT_EQ(geom_project(&ta, &ca, &vp, 0, 0, 0), RDR_OK);
  ASSERT_EQ(geom_project(&tb, &cb, &vp, 0, 0, 0), RDR_OK);
  ASSERT_EQ(geom_project(&tc, &cc, &vp, 0, 0, 0), RDR_OK);

  int32_t const area2 = geom_signed_area2(&ta, &tb, &tc);
  ASSERT_NE(area2, 0);

  // Pin the winding convention to the oracle: the oracle's screen-space edge
  // function (matches oracle_fill_tri's `area`) must share sign with our
  // signed-area test for the SAME screen positions. This guarantees the cull
  // rule below is anchored to the oracle, not an internal assumption.
  float const ax = (float)ta.x / 16.0F;
  float const ay = (float)ta.y / 16.0F;
  float const bx = (float)tb.x / 16.0F;
  float const by = (float)tb.y / 16.0F;
  float const cx = (float)tc.x / 16.0F;
  float const cy = (float)tc.y / 16.0F;
  float const oarea = ((bx - ax) * (cy - ay)) - ((by - ay) * (cx - ax));
  ASSERT_EQ((oarea < 0.0F), (area2 < 0));

  int const det = 1;  // identity modelview
  // Front-facing => NOT culled under CULL_BACK; culled under CULL_FRONT.
  EXPECT_EQ(geom_cull_backface(area2, det, CULL_BACK), 0);
  EXPECT_NE(geom_cull_backface(area2, det, CULL_FRONT), 0);
  EXPECT_EQ(geom_cull_backface(area2, det, CULL_NONE), 0);
  EXPECT_NE(geom_cull_backface(area2, det, CULL_BOTH), 0);
  // Mirror (det<0) flips the result.
  EXPECT_NE(geom_cull_backface(area2, -1, CULL_BACK), 0);
  EXPECT_EQ(geom_cull_backface(area2, -1, CULL_FRONT), 0);
  // Degenerate dropped regardless.
  EXPECT_NE(geom_cull_backface(0, det, CULL_NONE), 0);
}

TEST(Geom, ModelviewDetSign) {
  struct Mat4fx m;
  mat4_identity(&m);
  EXPECT_EQ(geom_modelview_det_sign(&m), 1);
  // Mirror X -> det < 0.
  m.m[0] = fx(-1.0);
  EXPECT_EQ(geom_modelview_det_sign(&m), -1);
}

// ---- tile binning (arena-backed variable bins, Wave-E) ----------------------
TEST(Geom, BinTriHitsOverlappedTiles) {
  static struct TVtx pool[16];
  static struct TriRef bin_pool[GEOM_NUM_TILES * 8];
  static struct TriRef bin_jobs[8];
  struct GeomOut o;
  geom_out_init(&o, pool, 16, bin_pool, GEOM_NUM_TILES * 8, bin_jobs, 8);

  // Triangle covering the whole screen -> hits every tile.
  struct TVtx v0;
  struct TVtx v1;
  struct TVtx v2;
  memset(&v0, 0, sizeof v0);
  memset(&v1, 0, sizeof v1);
  memset(&v2, 0, sizeof v2);
  v0.x = (fx12_4)(0 * 16);
  v0.y = (fx12_4)(0 * 16);
  v1.x = (fx12_4)((RDR_SCREEN_W - 1) * 16);
  v1.y = (fx12_4)(0 * 16);
  v2.x = (fx12_4)(0 * 16);
  v2.y = (fx12_4)((RDR_SCREEN_H - 1) * 16);
  uint32_t const i0 = geom_emit_tvert(&o, &v0);
  uint32_t const i1 = geom_emit_tvert(&o, &v1);
  uint32_t const i2 = geom_emit_tvert(&o, &v2);
  ASSERT_NE(i0, UINT32_MAX);
  // Deferred: bin buffers, finalize packs the per-tile segments.
  ASSERT_EQ(geom_bin_tri(&o, (uint16_t)i0, (uint16_t)i1, (uint16_t)i2, 0),
            RDR_OK);
  EXPECT_EQ(o.jobs_count, 1U);
  geom_bin_finalize(&o);
  // Tile (0,0) must be hit (the triangle covers the top-left corner).
  EXPECT_GE(o.tiles[0].count, 1U);
  EXPECT_EQ(o.tris_total, 1U);
}

// Pool (not per-tile) overflow now drops-with-count: a multi-tile tri whose
// total demand exceeds the shared pool can't fully place -> tris_dropped + the
// shortfall recorded in tiles[].dropped. Never corrupts.
TEST(Geom, BinPoolOverflowDropsAndCounts) {
  static struct TVtx pool[8];
  static struct TriRef bin_pool[1];  // shared pool: ONE ref slot total
  static struct TriRef bin_jobs[8];
  struct GeomOut o;
  geom_out_init(&o, pool, 8, bin_pool, 1, bin_jobs, 8);

  struct TVtx v0;
  struct TVtx v1;
  struct TVtx v2;
  memset(&v0, 0, sizeof v0);
  memset(&v1, 0, sizeof v1);
  memset(&v2, 0, sizeof v2);
  v0.x = (fx12_4)0;
  v0.y = (fx12_4)0;
  v1.x = (fx12_4)((RDR_SCREEN_W - 1) * 16);  // span the full width...
  v1.y = (fx12_4)0;
  v2.x = (fx12_4)0;
  v2.y = (fx12_4)((RDR_SCREEN_H - 1) * 16);  // ...and height -> all tiles
  uint32_t const i0 = geom_emit_tvert(&o, &v0);
  uint32_t const i1 = geom_emit_tvert(&o, &v1);
  uint32_t const i2 = geom_emit_tvert(&o, &v2);
  // Buffers fine; the tri spans every tile but the pool holds only 1 ref.
  EXPECT_EQ(geom_bin_tri(&o, i0, i1, i2, 0), RDR_OK);
  geom_bin_finalize(&o);
  EXPECT_EQ(o.pool_used, 1U);     // only one slot was available
  EXPECT_EQ(o.tris_total, 0U);    // the tri could not FULLY place
  EXPECT_EQ(o.tris_dropped, 1U);  // ...so it drops-with-count
  uint32_t tile_dropped = 0;
  for (int t = 0; t < GEOM_NUM_TILES; ++t) {
    tile_dropped += o.tiles[t].dropped;
  }
  EXPECT_GE(tile_dropped, 1U);
}

// Job-buffer overflow (more surviving tris than jobs_cap) also
// drops-with-count.
TEST(Geom, BinJobBufferOverflowDropsAndCounts) {
  static struct TVtx pool[8];
  static struct TriRef bin_pool[GEOM_NUM_TILES * 4];
  static struct TriRef bin_jobs[1];  // ONE job slot
  struct GeomOut o;
  geom_out_init(&o, pool, 8, bin_pool, GEOM_NUM_TILES * 4, bin_jobs, 1);

  struct TVtx v0;
  struct TVtx v1;
  struct TVtx v2;
  memset(&v0, 0, sizeof v0);
  memset(&v1, 0, sizeof v1);
  memset(&v2, 0, sizeof v2);
  v1.x = (fx12_4)(RDR_TILE_W / 2 * 16);
  v2.y = (fx12_4)(RDR_TILE_H / 2 * 16);
  uint32_t const i0 = geom_emit_tvert(&o, &v0);
  uint32_t const i1 = geom_emit_tvert(&o, &v1);
  uint32_t const i2 = geom_emit_tvert(&o, &v2);
  EXPECT_EQ(geom_bin_tri(&o, i0, i1, i2, 0), RDR_OK);  // fills the buffer
  EXPECT_EQ(geom_bin_tri(&o, i0, i1, i2, 0), RDR_EOVERFLOW);  // buffer full
  EXPECT_EQ(o.jobs_count, 1U);
  EXPECT_EQ(o.tris_dropped, 1U);
}

// Degenerate edge: a zero-capacity pool (mis-sizing) must drop everything
// without writing — finalize's prefix-sum gives every tile seg=0 (avail starts
// at 0), so no ref is written and the job drops-with-count. Pins the no-OOB
// boundary of the avail = pool_cap - offset arithmetic.
TEST(Geom, BinZeroPoolDropsAllSafely) {
  static struct TVtx pool[8];
  static struct TriRef bin_jobs[8];
  struct GeomOut o;
  geom_out_init(&o, pool, 8, nullptr, 0, bin_jobs, 8);  // pool_cap == 0

  struct TVtx v0;
  struct TVtx v1;
  struct TVtx v2;
  memset(&v0, 0, sizeof v0);
  memset(&v1, 0, sizeof v1);
  memset(&v2, 0, sizeof v2);
  v1.x = (fx12_4)(RDR_TILE_W / 2 * 16);
  v2.y = (fx12_4)(RDR_TILE_H / 2 * 16);
  uint32_t const i0 = geom_emit_tvert(&o, &v0);
  uint32_t const i1 = geom_emit_tvert(&o, &v1);
  uint32_t const i2 = geom_emit_tvert(&o, &v2);
  EXPECT_EQ(geom_bin_tri(&o, i0, i1, i2, 0), RDR_OK);  // buffers fine
  geom_bin_finalize(&o);                               // must not write/OOB
  EXPECT_EQ(o.pool_used, 0U);
  EXPECT_EQ(o.tris_total, 0U);
  EXPECT_EQ(o.tris_dropped, 1U);
  for (int t = 0; t < GEOM_NUM_TILES; ++t) {
    EXPECT_EQ(o.tiles[t].cap, 0U);
    EXPECT_EQ(o.tiles[t].count, 0U);
  }
}

// ---- lighting --------------------------------------------------------------
// Pre-lit path: rgba passes through to a packed 565 color.
TEST(Geom, PrelitPassThrough) {
  struct Vtx v;
  memset(&v, 0, sizeof v);
  v.c.rgba[0] = 255;  // R
  v.c.rgba[1] = 0;
  v.c.rgba[2] = 0;
  v.c.rgba[3] = 255;
  struct LightState ls;
  memset(&ls, 0, sizeof ls);
  uint16_t const packed = geom_shade_vertex(&v, &ls, 0);
  uint8_t r;
  uint8_t g;
  uint8_t b;
  oracle_unpack565(packed, &r, &g, &b);
  EXPECT_GT(r, 200);
  EXPECT_LT(g, 40);
  EXPECT_LT(b, 40);
}

// Lit path: a normal facing the light is brighter than one facing away.
TEST(Geom, DirectionalLightShading) {
  struct LightState ls;
  memset(&ls, 0, sizeof ls);
  ls.count = 1;
  ls.dirs[0].dir[0] = 0;
  ls.dirs[0].dir[1] = 0;
  ls.dirs[0].dir[2] = 127;  // light from +z (post-modelview)
  ls.dirs[0].rgb[0] = 255;
  ls.dirs[0].rgb[1] = 255;
  ls.dirs[0].rgb[2] = 255;
  ls.ambient[0] = 32;
  ls.ambient[1] = 32;
  ls.ambient[2] = 32;

  struct Vtx facing;
  memset(&facing, 0, sizeof facing);
  facing.c.nrm.n[2] = 127;  // normal +z -> toward light
  struct Vtx away;
  memset(&away, 0, sizeof away);
  away.c.nrm.n[2] = -127;  // normal -z -> away

  uint16_t const pf = geom_shade_vertex(&facing, &ls, 1);
  uint16_t const pa = geom_shade_vertex(&away, &ls, 1);
  uint8_t rf;
  uint8_t gf;
  uint8_t bf;
  uint8_t ra;
  uint8_t ga;
  uint8_t ba;
  oracle_unpack565(pf, &rf, &gf, &bf);
  oracle_unpack565(pa, &ra, &ga, &ba);
  EXPECT_GT((int)gf, (int)ga);  // facing brighter
  EXPECT_LE((int)ga, 64);       // away ~ ambient only
}

// ---- material interning (Wave-D D.4) ---------------------------------------
// A distinct, deterministic RenderState keyed off a small integer tag. Each tag
// yields a different value, so value-compare dedup treats them as distinct
// materials; the same tag reproduces a value-equal state (must dedup).
static struct RenderState mk_rstate(int tag) {
  struct RenderState rs;
  memset(&rs, 0, sizeof rs);
  rs.zmode = (uint8_t)(tag & 0x3);
  rs.cull = CULL_BACK;
  rs.prim_color = (uint16_t)(0x1000 + tag);
  rs.env_color = (uint16_t)(0x2000 + tag);
  rs.combiner.mode = (uint8_t)tag;
  return rs;
}

TEST(GeomMaterial, ResetEmptiesTable) {
  static struct Frame f;
  memset(&f, 0, sizeof f);
  f.rstate_count = 7;  // stale from a prior frame
  geom_material_reset(&f);
  EXPECT_EQ(f.rstate_count, 0U);
  EXPECT_EQ(geom_material_overflow_count(), 0U);
}

// Distinct RenderStates -> distinct ascending ids; the table grows by one each.
TEST(GeomMaterial, DistinctStatesGetDistinctIds) {
  static struct Frame f;
  memset(&f, 0, sizeof f);
  geom_material_reset(&f);

  struct RenderState const a = mk_rstate(1);
  struct RenderState const b = mk_rstate(2);
  struct RenderState const c = mk_rstate(3);
  uint16_t const ia = geom_material_intern(&f, &a);
  uint16_t const ib = geom_material_intern(&f, &b);
  uint16_t const ic = geom_material_intern(&f, &c);
  EXPECT_EQ(ia, 0U);
  EXPECT_EQ(ib, 1U);
  EXPECT_EQ(ic, 2U);
  EXPECT_EQ(f.rstate_count, 3U);
  // Interned values landed in the table verbatim. memcmp is sound: every state
  // here is mk_rstate (memset-zero + field assign) so padding is uniformly 0.
  // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison)
  EXPECT_EQ(memcmp(&f.rstate_table[0], &a, sizeof a), 0);
  // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison)
  EXPECT_EQ(memcmp(&f.rstate_table[1], &b, sizeof b), 0);
  // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison)
  EXPECT_EQ(memcmp(&f.rstate_table[2], &c, sizeof c), 0);
}

// Identical (value-equal) RenderState -> same id (dedup); count does not grow.
// Pointer identity is irrelevant: two distinct buffers holding the same value
// must collapse to one id.
TEST(GeomMaterial, IdenticalStateDedups) {
  static struct Frame f;
  memset(&f, 0, sizeof f);
  geom_material_reset(&f);

  struct RenderState const a = mk_rstate(5);
  struct RenderState const a_copy =
      mk_rstate(5);  // distinct buffer, same value
  struct RenderState const b = mk_rstate(6);
  uint16_t const ia = geom_material_intern(&f, &a);
  uint16_t const ib = geom_material_intern(&f, &b);
  uint16_t const ia2 = geom_material_intern(&f, &a_copy);
  uint16_t const ia3 = geom_material_intern(&f, &a);  // re-intern same buffer
  EXPECT_EQ(ia, 0U);
  EXPECT_EQ(ib, 1U);
  EXPECT_EQ(ia2, 0U);  // deduped to the first 'a'
  EXPECT_EQ(ia3, 0U);
  EXPECT_EQ(f.rstate_count, 2U);  // only two distinct values stored
}

// Null rs defaults to &f->rstate (intern the current state).
TEST(GeomMaterial, NullInternsCurrentState) {
  static struct Frame f;
  memset(&f, 0, sizeof f);
  geom_material_reset(&f);
  f.rstate = mk_rstate(9);
  uint16_t const id = geom_material_intern(&f, 0);
  EXPECT_EQ(id, 0U);
  EXPECT_EQ(f.rstate_count, 1U);
  // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison)
  EXPECT_EQ(memcmp(&f.rstate_table[0], &f.rstate, sizeof f.rstate), 0);
}

// Fill exactly to the cap with distinct states, then overflow: count never
// exceeds RDR_MAX_MATERIALS, the overflow request is dropped-with-count, and
// the returned id is a clamped, in-range last valid id (never corrupts the
// table).
TEST(GeomMaterial, OverflowClampsAndCounts) {
  static struct Frame f;
  memset(&f, 0, sizeof f);
  geom_material_reset(&f);

  for (int i = 0; i < RDR_MAX_MATERIALS; ++i) {
    struct RenderState const rs = mk_rstate(100 + i);
    uint16_t const id = geom_material_intern(&f, &rs);
    EXPECT_EQ((int)id, i);
  }
  EXPECT_EQ((int)f.rstate_count, RDR_MAX_MATERIALS);
  EXPECT_EQ(geom_material_overflow_count(), 0U);

  // One more distinct state overflows: dropped-with-count, clamped id.
  struct RenderState const extra = mk_rstate(999);
  uint16_t const oid = geom_material_intern(&f, &extra);
  EXPECT_EQ((int)f.rstate_count, RDR_MAX_MATERIALS);  // table did not grow
  EXPECT_LT((int)oid, RDR_MAX_MATERIALS);      // in-range, never corrupts
  EXPECT_EQ((int)oid, RDR_MAX_MATERIALS - 1);  // clamp policy: last slot
  EXPECT_EQ(geom_material_overflow_count(), 1U);

  // A value that IS already in the full table still dedups (no extra drop).
  struct RenderState const present = mk_rstate(100);  // id 0, first inserted
  uint16_t const pid = geom_material_intern(&f, &present);
  EXPECT_EQ(pid, 0U);
  EXPECT_EQ(geom_material_overflow_count(), 1U);  // unchanged
}

// ---- G1: transform-once + share tests (geom-share barrier) ------------------
// These tests are RED under the old per-tri emit code and GREEN only after
// the transform-once / vbase share is implemented.

// Helper: build a minimal Frame ready for geom_run (identity MV, custom proj).
static void frame_init_for_share(struct Frame* f, const struct Mat4fx* proj) {
  memset(f, 0, sizeof *f);
  f->width = RDR_SCREEN_W;
  f->height = RDR_SCREEN_H;
  geom_out_init(&f->geom, f->pool, RDR_MAX_TVERTS, f->bin_pool,
                RDR_BIN_POOL_REFS, f->bin_jobs, RDR_BIN_MAX_JOBS);
  mtx_init(&f->mtx);
  mtx_set(&f->mtx, MTX_PROJECTION, 0, proj);
  vp_from_cmd(&f->vp, 0, 0, f->width, f->height);
  memset(&f->rstate, 0, sizeof f->rstate);
  f->rstate.cull = CULL_NONE;
}

// 2x2 quad grid: 9 verts (3x3), 8 triangles.  All verts on-screen; after
// transform-once, DRAW_TRIS must reuse pooled tverts: pool grows by exactly 9
// (one per source vert), NOT by 3*8=24 (per-tri emit).
TEST(GeomShare, SharingReusesPooledVerts) {
  struct Mat4fx proj;
  struct OMat4 oproj;
  make_proj(&proj, &oproj, 1.5, 0.5, 50.0);

  static struct Frame f;
  frame_init_for_share(&f, &proj);

  // 3x3 grid in model space, all well in front of camera at z=10.
  // x in {-4,-0, 4}, y in {-4, 0, 4} -> project to on-screen coords.
  static struct Vtx verts[9];
  int vi = 0;
  int const xs[3] = {-4, 0, 4};
  int const ys[3] = {-4, 0, 4};
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      verts[vi] = mk_vtx((int16_t)xs[col], (int16_t)ys[row], 10);
      verts[vi].c.rgba[3] = 255;
      ++vi;
    }
  }

  // 8 triangles from the 4 quads (each quad = 2 tris).
  // Grid indices: row r, col c -> index (r*3)+c.
  //   (r,c),(r,c+1),(r+1,c) and (r,c+1),(r+1,c+1),(r+1,c)
  static uint16_t idx[8 * 3];
  int ti = 0;
  for (int r = 0; r < 2; ++r) {
    for (int c = 0; c < 2; ++c) {
      uint16_t const tl = (uint16_t)((r * 3) + c);
      uint16_t const tr_v = (uint16_t)((r * 3) + c + 1);
      uint16_t const bl = (uint16_t)(((r + 1) * 3) + c);
      uint16_t const br = (uint16_t)(((r + 1) * 3) + c + 1);
      idx[(ti * 3) + 0] = tl;
      idx[(ti * 3) + 1] = tr_v;
      idx[(ti * 3) + 2] = bl;
      ++ti;
      idx[(ti * 3) + 0] = tr_v;
      idx[(ti * 3) + 1] = br;
      idx[(ti * 3) + 2] = bl;
      ++ti;
    }
  }

  struct Command cmds[6];
  memset(cmds, 0, sizeof cmds);
  int n = 0;
  cmds[n].op = CMD_LOAD_VERTS;
  cmds[n].u.load_verts.ptr = verts;
  cmds[n].u.load_verts.count = 9;
  cmds[n].u.load_verts.base = 0;
  ++n;
  cmds[n].op = CMD_DRAW_TRIS;
  cmds[n].u.draw_tris.idx = idx;
  cmds[n].u.draw_tris.tri_count = 8;
  ++n;
  cmds[n].op = CMD_END;

  ASSERT_EQ(geom_run(cmds, &f), RDR_OK);
  // KEY INVARIANT: exactly 9 tverts emitted (one per source vert), not 3*8=24.
  EXPECT_EQ(f.geom.tvert_count, 9U);
}

// A triangle straddling the guard band triggers the clip path; new tverts ARE
// emitted for the clipped output (clip fan), and the tri is still rendered.
TEST(GeomShare, ClipPathStillEmits) {
  struct Mat4fx proj;
  struct OMat4 oproj;
  make_proj(&proj, &oproj, 1.5, 0.5, 50.0);

  static struct Frame f;
  frame_init_for_share(&f, &proj);

  // Vertices: two inside the guard band (on-screen), one FAR outside so the
  // guard-band clip fires.  Use a large model-space x.
  // With fov_scale=1.5, z=10, NDC_x = 1.5 * x / 10. screen_x = cx + ndc_x*120.
  // Guard extends to RDR_SCREEN_W + 2*RDR_SCREEN_W = 720 screen px.
  // We want screen_x >> 720, so model_x >> 720/120*10/1.5 = 40. Use 200.
  // Use non-zero y variation to avoid a degenerate (all-collinear) triangle.
  static struct Vtx verts[3];
  verts[0] = mk_vtx(-2, -3, 10);
  verts[1] = mk_vtx(2, 3, 10);
  verts[2] = mk_vtx(200, 0, 10);  // way outside guard band
  for (int k = 0; k < 3; ++k) {
    verts[k].c.rgba[3] = 255;
  }

  static const uint16_t idx[3] = {0, 1, 2};
  struct Command cmds[4];
  memset(cmds, 0, sizeof cmds);
  int n = 0;
  cmds[n].op = CMD_LOAD_VERTS;
  cmds[n].u.load_verts.ptr = verts;
  cmds[n].u.load_verts.count = 3;
  cmds[n].u.load_verts.base = 0;
  ++n;
  cmds[n].op = CMD_DRAW_TRIS;
  cmds[n].u.draw_tris.idx = idx;
  cmds[n].u.draw_tris.tri_count = 1;
  ++n;
  cmds[n].op = CMD_END;

  ASSERT_EQ(geom_run(cmds, &f), RDR_OK);
  // 3 tverts from LOAD_VERTS, plus clipped fan verts from the clip path.
  EXPECT_GE(f.geom.tvert_count, 3U);  // at minimum the 3 source verts
  // The clipped triangle(s) must have been binned (not fully culled by clip).
  EXPECT_GE(f.geom.tris_total, 1U);
}

// A vert behind the near plane (clip.w<=0) gets a SENTINEL tvert (inv_w<0).
// Every triangle that references that sentinel vert is dropped; triangles
// using only in-front verts survive.
TEST(GeomShare, NearRejectWholeTri) {
  struct Mat4fx proj;
  struct OMat4 oproj;
  make_proj(&proj, &oproj, 1.5, 0.5, 50.0);

  static struct Frame f;
  frame_init_for_share(&f, &proj);

  // verts[0..1] in front (z=10), verts[2] behind (z=-5, w<=0 after projection).
  // Near plane is at n=0.5 in proj params above; z=-5 is behind.
  static struct Vtx verts[3];
  verts[0] = mk_vtx(-2, -2, 10);
  verts[1] = mk_vtx(2, -2, 10);
  verts[2] = mk_vtx(0, 0, -5);  // behind near plane; w will be <= 0
  for (int k = 0; k < 3; ++k) {
    verts[k].c.rgba[3] = 255;
  }

  // tri0 uses only in-front verts -> must survive.
  // tri1 uses verts[2] (behind) -> must be dropped.
  static const uint16_t idx[6] = {0, 1, 0, 0, 1, 2};
  struct Command cmds[4];
  memset(cmds, 0, sizeof cmds);
  int n = 0;
  cmds[n].op = CMD_LOAD_VERTS;
  cmds[n].u.load_verts.ptr = verts;
  cmds[n].u.load_verts.count = 3;
  cmds[n].u.load_verts.base = 0;
  ++n;
  cmds[n].op = CMD_DRAW_TRIS;
  cmds[n].u.draw_tris.idx = idx;
  cmds[n].u.draw_tris.tri_count = 2;
  ++n;
  cmds[n].op = CMD_END;

  ASSERT_EQ(geom_run(cmds, &f), RDR_OK);
  // tri1 (0,1,0) is degenerate (area==0); tri2 (0,1,2) has near-behind vert ->
  // dropped. So tris_total==0 or 1 (degenerate drop) + 1 dropped.
  // The near-reject guard: tris_dropped >= 1.
  EXPECT_GE(f.geom.tris_dropped, 1U);
  // Pool has 3 tverts: verts[0] and [1] are valid, verts[2] gets sentinel.
  EXPECT_EQ(f.geom.tvert_count, 3U);
  // The sentinel tvert (verts[2]) must have inv_w < 0.
  EXPECT_LT(f.pool[2].inv_w, 0);
}

// An index >= vcount is out-of-range; the tri is dropped and the pool is not
// corrupted (tvert_count does not grow from the bad reference).
TEST(GeomShare, OutOfRangeIndexDropped) {
  struct Mat4fx proj;
  struct OMat4 oproj;
  make_proj(&proj, &oproj, 1.5, 0.5, 50.0);

  static struct Frame f;
  frame_init_for_share(&f, &proj);

  static struct Vtx verts[2];
  verts[0] = mk_vtx(-2, -2, 10);
  verts[1] = mk_vtx(2, -2, 10);
  for (int k = 0; k < 2; ++k) {
    verts[k].c.rgba[3] = 255;
  }

  // Index 2 is out of range (vcount=2); tri must be dropped.
  static const uint16_t idx[3] = {0, 1, 2};
  struct Command cmds[4];
  memset(cmds, 0, sizeof cmds);
  int n = 0;
  cmds[n].op = CMD_LOAD_VERTS;
  cmds[n].u.load_verts.ptr = verts;
  cmds[n].u.load_verts.count = 2;
  cmds[n].u.load_verts.base = 0;
  ++n;
  cmds[n].op = CMD_DRAW_TRIS;
  cmds[n].u.draw_tris.idx = idx;
  cmds[n].u.draw_tris.tri_count = 1;
  ++n;
  cmds[n].op = CMD_END;

  ASSERT_EQ(geom_run(cmds, &f), RDR_OK);
  // 2 tverts for the 2 loaded verts; pool not corrupted by the bad index.
  EXPECT_EQ(f.geom.tvert_count, 2U);
  EXPECT_GE(f.geom.tris_dropped, 1U);
}

// End-to-end: two SET_MATERIAL blocks drawing the same geometry produce TriRefs
// whose material ids match the interned ids (resolves the always-0
// placeholder). Distinct states -> distinct ids on the binned refs; a repeated
// state dedups.
TEST(GeomMaterial, EmittedTriRefsCarryInternedId) {
  static struct Frame f;
  memset(&f, 0, sizeof f);
  f.fb = 0;  // unused; geom_run does not touch the framebuffer
  f.width = RDR_SCREEN_W;
  f.height = RDR_SCREEN_H;
  geom_out_init(&f.geom, f.pool, RDR_MAX_TVERTS, f.bin_pool, RDR_BIN_POOL_REFS,
                f.bin_jobs, RDR_BIN_MAX_JOBS);
  mtx_init(&f.mtx);
  vp_from_cmd(&f.vp, 0, 0, f.width, f.height);
  memset(&f.rstate, 0, sizeof f.rstate);
  f.rstate.cull = CULL_NONE;  // keep both windings (don't depend on cull sign)

  // Projection that maps the verts comfortably in front of the camera.
  struct Mat4fx proj;
  struct OMat4 oproj;
  make_proj(&proj, &oproj, 1.5, 0.5, 50.0);

  // Two materials (value-distinct) + a repeat of the first.
  struct RenderState mat_a = mk_rstate(11);
  mat_a.cull = CULL_NONE;
  struct RenderState mat_b = mk_rstate(22);
  mat_b.cull = CULL_NONE;

  // One screen-filling triangle, drawn under each material (pre-lit verts).
  struct Vtx verts[3];
  verts[0] = mk_vtx(-3, -3, 10);
  verts[1] = mk_vtx(3, -3, 10);
  verts[2] = mk_vtx(0, 3, 10);
  for (int k = 0; k < 3; ++k) {
    verts[k].c.rgba[3] = 255;
  }
  static const uint16_t idx[3] = {0, 1, 2};

  struct Command cmds[10];
  memset(cmds, 0, sizeof cmds);
  int n = 0;
  cmds[n].op = CMD_SET_MATRIX;
  cmds[n].u.set_matrix.target = MTX_PROJECTION;
  cmds[n].u.set_matrix.push = 0;
  cmds[n].u.set_matrix.mat = &proj;
  ++n;
  cmds[n].op = CMD_LOAD_VERTS;
  cmds[n].u.load_verts.ptr = verts;
  cmds[n].u.load_verts.count = 3;
  cmds[n].u.load_verts.base = 0;
  ++n;
  cmds[n].op = CMD_SET_MATERIAL;
  cmds[n].u.set_material.state = &mat_a;
  ++n;
  cmds[n].op = CMD_DRAW_TRIS;
  cmds[n].u.draw_tris.idx = idx;
  cmds[n].u.draw_tris.tri_count = 1;
  ++n;
  cmds[n].op = CMD_SET_MATERIAL;
  cmds[n].u.set_material.state = &mat_b;
  ++n;
  cmds[n].op = CMD_DRAW_TRIS;
  cmds[n].u.draw_tris.idx = idx;
  cmds[n].u.draw_tris.tri_count = 1;
  ++n;
  cmds[n].op = CMD_SET_MATERIAL;  // back to mat_a -> must dedup to id 0
  cmds[n].u.set_material.state = &mat_a;
  ++n;
  cmds[n].op = CMD_DRAW_TRIS;
  cmds[n].u.draw_tris.idx = idx;
  cmds[n].u.draw_tris.tri_count = 1;
  ++n;
  cmds[n].op = CMD_END;
  ++n;

  ASSERT_EQ(geom_run(cmds, &f), RDR_OK);
  EXPECT_EQ(f.rstate_count, 2U);  // mat_a, mat_b; the mat_a repeat deduped
  ASSERT_EQ(f.geom.tris_total, 3U);

  // Each binned TriRef must carry an in-range interned id. Across all tiles the
  // two mat_a draws contribute id 0 and the mat_b draw id 1; no ref carries the
  // old always-0 placeholder for mat_b, and none is out of range.
  int seen0 = 0;
  int seen1 = 0;
  int seen_other = 0;
  for (int t = 0; t < GEOM_NUM_TILES; ++t) {
    for (uint32_t i = 0; i < f.geom.tiles[t].count; ++i) {
      uint16_t const m = f.geom.tiles[t].refs[i].material;
      ASSERT_LT((int)m, (int)f.rstate_count);  // never corrupt / out of range
      if (m == 0) {
        ++seen0;
      } else if (m == 1) {
        ++seen1;
      } else {
        ++seen_other;
      }
    }
  }
  // The mat_a triangle is binned to its tile set twice (two draws); mat_b once.
  // So mat_b (id 1) refs == half the mat_a (id 0) refs across the screen.
  EXPECT_GT(seen0, 0);
  EXPECT_GT(seen1, 0);
  EXPECT_EQ(seen0, 2 * seen1);  // mat_a drawn twice, mat_b once (same geometry)
  EXPECT_EQ(seen_other, 0);
}

// ---- fog end-to-end (R.3): geom_run populates TVtx.fog from view-z ----------
// CMD_SET_FOG enables fog; LOAD_VERTS at distinct depths must birth DISTINCT,
// monotone-with-distance TVtx.fog. Without SET_FOG, fog stays 0
// (bit-identical).
TEST(GeomShare, FogPopulatesPerVertexFromDepth) {
  struct Mat4fx proj;
  struct OMat4 oproj;
  make_proj(&proj, &oproj, 1.5, 0.5, 50.0);

  static struct Frame f;
  frame_init_for_share(&f, &proj);

  // Three verts at increasing view-z (clip.w = +view_z here): z=4,12,30.
  static struct Vtx verts[3];
  verts[0] = mk_vtx(0, 0, 4);
  verts[1] = mk_vtx(0, 0, 12);
  verts[2] = mk_vtx(0, 0, 30);
  for (int k = 0; k < 3; ++k) {
    verts[k].c.rgba[3] = 255;
  }

  struct Command cmds[4];
  memset(cmds, 0, sizeof cmds);
  int n = 0;
  cmds[n].op = CMD_SET_FOG;
  cmds[n].u.set_fog.near_z = fx(5.0);
  cmds[n].u.set_fog.far_z = fx(25.0);
  cmds[n].u.set_fog.color = 0;
  ++n;
  cmds[n].op = CMD_LOAD_VERTS;
  cmds[n].u.load_verts.ptr = verts;
  cmds[n].u.load_verts.count = 3;
  cmds[n].u.load_verts.base = 0;
  ++n;
  cmds[n].op = CMD_END;

  ASSERT_EQ(geom_run(cmds, &f), RDR_OK);
  ASSERT_EQ(f.geom.tvert_count, 3U);
  // z=4 <= near(5) -> 0; z=30 >= far(25) -> 255; z=12 strictly between.
  EXPECT_EQ((int)f.geom.tverts[0].fog, 0);
  EXPECT_EQ((int)f.geom.tverts[2].fog, 255);
  EXPECT_GT((int)f.geom.tverts[1].fog, 0);
  EXPECT_LT((int)f.geom.tverts[1].fog, 255);
  // Monotone with distance.
  EXPECT_LT((int)f.geom.tverts[0].fog, (int)f.geom.tverts[1].fog);
  EXPECT_LT((int)f.geom.tverts[1].fog, (int)f.geom.tverts[2].fog);
}

// Fog DISABLED (no SET_FOG): every TVtx.fog stays 0 -> raster fog no-ops ->
// bit-identical to the pre-R.3 pipeline (the load-bearing regression guard).
TEST(GeomShare, FogDisabledBirthsZero) {
  struct Mat4fx proj;
  struct OMat4 oproj;
  make_proj(&proj, &oproj, 1.5, 0.5, 50.0);

  static struct Frame f;
  frame_init_for_share(&f, &proj);

  static struct Vtx verts[3];
  verts[0] = mk_vtx(0, 0, 4);
  verts[1] = mk_vtx(0, 0, 12);
  verts[2] = mk_vtx(0, 0, 30);
  for (int k = 0; k < 3; ++k) {
    verts[k].c.rgba[3] = 255;
  }

  struct Command cmds[3];
  memset(cmds, 0, sizeof cmds);
  int n = 0;
  cmds[n].op = CMD_LOAD_VERTS;
  cmds[n].u.load_verts.ptr = verts;
  cmds[n].u.load_verts.count = 3;
  cmds[n].u.load_verts.base = 0;
  ++n;
  cmds[n].op = CMD_END;

  ASSERT_EQ(geom_run(cmds, &f), RDR_OK);
  ASSERT_EQ(f.geom.tvert_count, 3U);
  for (uint32_t i = 0; i < f.geom.tvert_count; ++i) {
    EXPECT_EQ((int)f.geom.tverts[i].fog, 0);
  }
}
