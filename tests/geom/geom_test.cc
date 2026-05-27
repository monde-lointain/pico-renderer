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

// ---- tile binning ----------------------------------------------------------
TEST(Geom, BinTriHitsOverlappedTiles) {
  static struct TVtx pool[16];
  static struct TriRef refs[GEOM_NUM_TILES * 8];
  struct GeomOut o;
  geom_out_init(&o, pool, 16, refs, 8);

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
  ASSERT_EQ(geom_bin_tri(&o, (uint16_t)i0, (uint16_t)i1, (uint16_t)i2, 0),
            RDR_OK);
  // Tile (0,0) must be hit (the triangle covers the top-left corner).
  EXPECT_GE(o.tiles[0].count, 1U);
  EXPECT_EQ(o.tris_total, 1U);
}

TEST(Geom, BinOverflowDropsAndCounts) {
  static struct TVtx pool[8];
  static struct TriRef refs[GEOM_NUM_TILES * 1];  // cap 1 per tile
  struct GeomOut o;
  geom_out_init(&o, pool, 8, refs, 1);

  struct TVtx v0;
  struct TVtx v1;
  struct TVtx v2;
  memset(&v0, 0, sizeof v0);
  memset(&v1, 0, sizeof v1);
  memset(&v2, 0, sizeof v2);
  v0.x = (fx12_4)0;
  v0.y = (fx12_4)0;
  v1.x = (fx12_4)(RDR_TILE_W / 2 * 16);
  v1.y = (fx12_4)0;
  v2.x = (fx12_4)0;
  v2.y = (fx12_4)(RDR_TILE_H / 2 * 16);
  uint32_t const i0 = geom_emit_tvert(&o, &v0);
  uint32_t const i1 = geom_emit_tvert(&o, &v1);
  uint32_t const i2 = geom_emit_tvert(&o, &v2);
  // First into tile 0 ok; second overflows that tile's cap-1 segment.
  EXPECT_EQ(geom_bin_tri(&o, i0, i1, i2, 0), RDR_OK);
  EXPECT_EQ(geom_bin_tri(&o, i0, i1, i2, 0), RDR_EOVERFLOW);
  EXPECT_EQ(o.tiles[0].count, 1U);
  EXPECT_GE(o.tiles[0].dropped, 1U);
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
