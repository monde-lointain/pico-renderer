// rdr_test.cc — C1 single-core integration tests. Host-first, gtest.
//
// The core C1 proof: a static scene driven through the FULL rdr pipeline
// (cmd -> geom -> raster -> framebuffer) is compared against the float oracle
// (tests/harness/oracle.h) within tolerance. Plus determinism, control-flow,
// and robustness of the integrated front end. Orthodox C++ test style.
#include "rdr/rdr.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "gfx/framebuffer.h"
#include "gtest/gtest.h"
#include "oracle.h"
#include "rdr/config.h"
#include "rdr/frame.h"

// ---- fixed-point + scene helpers -------------------------------------------
static fx16_16 fx(double v) { return (fx16_16)lrint(v * 65536.0); }

static struct Vtx mk_cvtx(int16_t x, int16_t y, int16_t z, uint8_t r, uint8_t g,
                          uint8_t b) {
  struct Vtx v;
  memset(&v, 0, sizeof v);
  v.pos[0] = x;
  v.pos[1] = y;
  v.pos[2] = z;
  v.c.rgba[0] = r;
  v.c.rgba[1] = g;
  v.c.rgba[2] = b;
  v.c.rgba[3] = 255;
  return v;
}

// Build a simple z-to-[0,1] perspective in Q16.16, mirrored as an OMat4 float.
static void make_proj(struct Mat4fx* m, struct OMat4* om, double fov_scale,
                      double n, double f) {
  memset(m->m, 0, sizeof m->m);
  double const a = fov_scale;
  double const za = f / (f - n);
  double const zb = -(f * n) / (f - n);
  m->m[0] = fx(a);
  m->m[5] = fx(a);
  m->m[10] = fx(za);
  m->m[11] = fx(1.0);  // w = z
  m->m[14] = fx(zb);
  for (int i = 0; i < 16; ++i) {
    om->m[i] = (float)m->m[i] / 65536.0F;
  }
}

// The renderer Frame is large (pool + bins) — keep it in BSS, not on the stack.
static struct Frame g_frame;
static struct Framebuffer g_fb;

// Count framebuffer pixels whose RGB differs from the oracle image by more than
// `chan_tol` on any channel. Returns the mismatch count.
static int count_mismatch(const uint16_t* fb, const struct OImage* oimg,
                          int chan_tol) {
  int bad = 0;
  for (int y = 0; y < oimg->h; ++y) {
    for (int x = 0; x < oimg->w; ++x) {
      int const idx = (y * oimg->w) + x;
      uint8_t r;
      uint8_t g;
      uint8_t b;
      oracle_unpack565(fb[idx], &r, &g, &b);
      const uint8_t* o = &oimg->rgb[(size_t)idx * 3];
      int const dr = abs((int)r - (int)o[0]);
      int const dg = abs((int)g - (int)o[1]);
      int const db = abs((int)b - (int)o[2]);
      if (dr > chan_tol || dg > chan_tol || db > chan_tol) {
        ++bad;
      }
    }
  }
  return bad;
}

// ---- core golden: one flat triangle through the full pipeline --------------
TEST(RdrGolden, FlatTriangleMatchesOracle) {
  struct Mat4fx proj;
  struct OMat4 oproj;
  make_proj(&proj, &oproj, 1.5, 0.5, 50.0);

  // CCW-in-NDC front-facing triangle, well in front of the camera. Red.
  struct Vtx verts[3] = {
      mk_cvtx(-6, -6, 12, 255, 0, 0),
      mk_cvtx(6, -6, 12, 255, 0, 0),
      mk_cvtx(0, 6, 12, 255, 0, 0),
  };
  uint16_t idx[3] = {0, 1, 2};

  struct RenderState rs;
  memset(&rs, 0, sizeof rs);
  rs.cull = CULL_NONE;  // golden compares fill, not winding policy
  rs.lit = 0;

  struct Command cmds[6];
  memset(cmds, 0, sizeof cmds);
  cmds[0].op = CMD_SET_MATRIX;
  cmds[0].u.set_matrix.target = MTX_PROJECTION;
  cmds[0].u.set_matrix.push = 0;
  cmds[0].u.set_matrix.mat = &proj;
  cmds[1].op = CMD_SET_MATERIAL;
  cmds[1].u.set_material.state = &rs;
  cmds[2].op = CMD_LOAD_VERTS;
  cmds[2].u.load_verts.ptr = verts;
  cmds[2].u.load_verts.count = 3;
  cmds[2].u.load_verts.base = 0;
  cmds[3].op = CMD_DRAW_TRIS;
  cmds[3].u.draw_tris.idx = idx;
  cmds[3].u.draw_tris.tri_count = 1;
  cmds[4].op = CMD_CLEAR;
  cmds[4].u.clear.color = 0;  // black
  cmds[5].op = CMD_END;
  // CLEAR must precede draws so end_frame clears first; reorder so it walks
  // before DRAW_TRIS (the clear is recorded regardless of stream position, but
  // keep semantics clear: clear is its own command before draw).
  struct Command ordered[6];
  ordered[0] = cmds[0];
  ordered[1] = cmds[4];  // clear
  ordered[2] = cmds[1];
  ordered[3] = cmds[2];
  ordered[4] = cmds[3];
  ordered[5] = cmds[5];

  g_frame.fb = g_fb.px;
  g_frame.width = RDR_SCREEN_W;
  g_frame.height = RDR_SCREEN_H;
  ASSERT_EQ(rdr_begin_frame(&g_frame), RDR_OK);
  ASSERT_EQ(rdr_submit(&g_frame, ordered), RDR_OK);
  ASSERT_EQ(rdr_end_frame(&g_frame), RDR_OK);

  // Oracle: same transform + viewport, fill the projected screen triangle.
  static uint8_t orgb[RDR_SCREEN_W * RDR_SCREEN_H * 3];
  struct OImage oimg = {orgb, RDR_SCREEN_W, RDR_SCREEN_H};
  oracle_image_clear(&oimg, 0, 0, 0);
  struct OViewport const ovp = {0, 0, RDR_SCREEN_W, RDR_SCREEN_H};
  struct OTVtx ot[3];
  for (int k = 0; k < 3; ++k) {
    struct OVec4 const om = {(float)verts[k].pos[0], (float)verts[k].pos[1],
                             (float)verts[k].pos[2], 1.0F};
    oracle_xform_vertex(&ot[k], &oproj, &om, 0.0F, 0.0F, &ovp);
    ASSERT_EQ(ot[k].clipped, 0);
  }
  oracle_fill_tri(&oimg, ot[0].sx, ot[0].sy, ot[1].sx, ot[1].sy, ot[2].sx,
                  ot[2].sy, 255, 0, 0);

  // RGB565 quantizes 255->248 etc; allow channel tolerance. Edge pixels differ
  // by at most a thin boundary; allow a small mismatch budget.
  int const bad = count_mismatch(g_frame.fb, &oimg, 12);
  int const total = RDR_SCREEN_W * RDR_SCREEN_H;
  EXPECT_LT(bad, total / 200) << "mismatched pixels: " << bad << "/" << total;
}

// ---- determinism: same stream -> byte-identical framebuffer ----------------
static uint32_t fb_crc(const uint16_t* fb, int n) {
  uint32_t h = 2166136261U;  // FNV-1a over the framebuffer bytes
  const uint8_t* p = (const uint8_t*)fb;
  for (int i = 0; i < n * 2; ++i) {
    h ^= p[i];
    h *= 16777619U;
  }
  return h;
}

TEST(RdrDeterminism, SameStreamSameFrame) {
  struct Mat4fx proj;
  struct OMat4 oproj;
  make_proj(&proj, &oproj, 1.5, 0.5, 50.0);
  struct Vtx verts[3] = {
      mk_cvtx(-6, -6, 12, 0, 255, 0),
      mk_cvtx(6, -6, 12, 0, 255, 0),
      mk_cvtx(0, 6, 12, 0, 255, 0),
  };
  uint16_t idx[3] = {0, 1, 2};
  struct RenderState rs;
  memset(&rs, 0, sizeof rs);
  rs.cull = CULL_NONE;
  struct Command s[5];
  memset(s, 0, sizeof s);
  s[0].op = CMD_SET_MATRIX;
  s[0].u.set_matrix.target = MTX_PROJECTION;
  s[0].u.set_matrix.mat = &proj;
  s[1].op = CMD_SET_MATERIAL;
  s[1].u.set_material.state = &rs;
  s[2].op = CMD_LOAD_VERTS;
  s[2].u.load_verts.ptr = verts;
  s[2].u.load_verts.count = 3;
  s[3].op = CMD_DRAW_TRIS;
  s[3].u.draw_tris.idx = idx;
  s[3].u.draw_tris.tri_count = 1;
  s[4].op = CMD_END;

  g_frame.fb = g_fb.px;
  g_frame.width = RDR_SCREEN_W;
  g_frame.height = RDR_SCREEN_H;
  rdr_begin_frame(&g_frame);
  rdr_submit(&g_frame, s);
  rdr_end_frame(&g_frame);
  uint32_t const crc1 = fb_crc(g_frame.fb, RDR_SCREEN_W * RDR_SCREEN_H);

  static struct Framebuffer fb2;
  g_frame.fb = fb2.px;
  rdr_begin_frame(&g_frame);
  rdr_submit(&g_frame, s);
  rdr_end_frame(&g_frame);
  uint32_t const crc2 = fb_crc(g_frame.fb, RDR_SCREEN_W * RDR_SCREEN_H);
  EXPECT_EQ(crc1, crc2);
}

// ---- control flow: CALL_LIST / END / BRANCH_LESS_Z / nesting ---------------
TEST(RdrControlFlow, CallListReplayAndEnd) {
  struct Mat4fx proj;
  struct OMat4 oproj;
  make_proj(&proj, &oproj, 1.5, 0.5, 50.0);
  struct Vtx verts[3] = {
      mk_cvtx(-6, -6, 12, 0, 0, 255),
      mk_cvtx(6, -6, 12, 0, 0, 255),
      mk_cvtx(0, 6, 12, 0, 0, 255),
  };
  uint16_t idx[3] = {0, 1, 2};
  struct RenderState rs;
  memset(&rs, 0, sizeof rs);
  rs.cull = CULL_NONE;

  // Sub-list: draw the triangle, then RETURN.
  struct Command sub[3];
  memset(sub, 0, sizeof sub);
  sub[0].op = CMD_DRAW_TRIS;
  sub[0].u.draw_tris.idx = idx;
  sub[0].u.draw_tris.tri_count = 1;
  sub[1].op = CMD_RETURN;
  sub[2].op = CMD_END;

  struct Command main_dl[5];
  memset(main_dl, 0, sizeof main_dl);
  main_dl[0].op = CMD_SET_MATRIX;
  main_dl[0].u.set_matrix.target = MTX_PROJECTION;
  main_dl[0].u.set_matrix.mat = &proj;
  main_dl[1].op = CMD_SET_MATERIAL;
  main_dl[1].u.set_material.state = &rs;
  main_dl[2].op = CMD_LOAD_VERTS;
  main_dl[2].u.load_verts.ptr = verts;
  main_dl[2].u.load_verts.count = 3;
  main_dl[3].op = CMD_CALL_LIST;
  main_dl[3].u.call_list.ptr = sub;
  main_dl[4].op = CMD_END;

  g_frame.fb = g_fb.px;
  g_frame.width = RDR_SCREEN_W;
  g_frame.height = RDR_SCREEN_H;
  rdr_begin_frame(&g_frame);
  ASSERT_EQ(rdr_submit(&g_frame, main_dl), RDR_OK);
  rdr_end_frame(&g_frame);
  EXPECT_EQ(g_frame.geom.tris_total, 1U);  // the CALL_LIST drew exactly one
}

TEST(RdrControlFlow, BranchLessZTakenAndNotTaken) {
  struct Mat4fx proj;
  struct OMat4 oproj;
  make_proj(&proj, &oproj, 1.5, 0.5, 50.0);
  // Vertex 0 at z=12 -> inv_w ~ 1/12. A threshold below that => taken (closer).
  struct Vtx verts[3] = {
      mk_cvtx(0, 0, 12, 255, 255, 0),
      mk_cvtx(6, -6, 12, 255, 255, 0),
      mk_cvtx(0, 6, 12, 255, 255, 0),
  };
  uint16_t idx[3] = {0, 1, 2};
  struct RenderState rs;
  memset(&rs, 0, sizeof rs);
  rs.cull = CULL_NONE;

  // Branch target draws the triangle then ends.
  struct Command target[3];
  memset(target, 0, sizeof target);
  target[0].op = CMD_DRAW_TRIS;
  target[0].u.draw_tris.idx = idx;
  target[0].u.draw_tris.tri_count = 1;
  target[1].op = CMD_END;
  target[2].op = CMD_END;

  struct Command dl[5];
  memset(dl, 0, sizeof dl);
  dl[0].op = CMD_SET_MATRIX;
  dl[0].u.set_matrix.target = MTX_PROJECTION;
  dl[0].u.set_matrix.mat = &proj;
  dl[1].op = CMD_SET_MATERIAL;
  dl[1].u.set_material.state = &rs;
  dl[2].op = CMD_LOAD_VERTS;
  dl[2].u.load_verts.ptr = verts;
  dl[2].u.load_verts.count = 3;
  dl[3].op = CMD_BRANCH_LESS_Z;
  dl[3].u.branch_less_z.vtx = 0;
  dl[3].u.branch_less_z.dl = target;
  dl[4].op = CMD_END;  // fall-through path draws nothing

  // Taken: tiny threshold z (vertex is closer) -> branch -> draws 1 tri.
  dl[3].u.branch_less_z.z = (fx_invw)1;  // ~0; inv_w(12) >> this -> taken
  g_frame.fb = g_fb.px;
  g_frame.width = RDR_SCREEN_W;
  g_frame.height = RDR_SCREEN_H;
  rdr_begin_frame(&g_frame);
  rdr_submit(&g_frame, dl);
  rdr_end_frame(&g_frame);
  EXPECT_EQ(g_frame.geom.tris_total, 1U);

  // Not taken: huge threshold z (nothing is closer) -> fall through -> 0 tris.
  dl[3].u.branch_less_z.z = (fx_invw)0x7fffffff;
  rdr_begin_frame(&g_frame);
  rdr_submit(&g_frame, dl);
  rdr_end_frame(&g_frame);
  EXPECT_EQ(g_frame.geom.tris_total, 0U);
}

TEST(RdrControlFlow, NestedCallList) {
  struct Vtx verts[3] = {
      mk_cvtx(0, 0, 12, 200, 200, 200),
      mk_cvtx(6, -6, 12, 200, 200, 200),
      mk_cvtx(0, 6, 12, 200, 200, 200),
  };
  uint16_t idx[3] = {0, 1, 2};
  struct Mat4fx proj;
  struct OMat4 oproj;
  make_proj(&proj, &oproj, 1.5, 0.5, 50.0);
  struct RenderState rs;
  memset(&rs, 0, sizeof rs);
  rs.cull = CULL_NONE;

  struct Command inner[2];
  memset(inner, 0, sizeof inner);
  inner[0].op = CMD_DRAW_TRIS;
  inner[0].u.draw_tris.idx = idx;
  inner[0].u.draw_tris.tri_count = 1;
  inner[1].op = CMD_END;

  struct Command mid[3];
  memset(mid, 0, sizeof mid);
  mid[0].op = CMD_CALL_LIST;
  mid[0].u.call_list.ptr = inner;
  mid[1].op = CMD_CALL_LIST;
  mid[1].u.call_list.ptr = inner;  // call inner twice
  mid[2].op = CMD_END;

  struct Command top[5];
  memset(top, 0, sizeof top);
  top[0].op = CMD_SET_MATRIX;
  top[0].u.set_matrix.target = MTX_PROJECTION;
  top[0].u.set_matrix.mat = &proj;
  top[1].op = CMD_SET_MATERIAL;
  top[1].u.set_material.state = &rs;
  top[2].op = CMD_LOAD_VERTS;
  top[2].u.load_verts.ptr = verts;
  top[2].u.load_verts.count = 3;
  top[3].op = CMD_CALL_LIST;
  top[3].u.call_list.ptr = mid;
  top[4].op = CMD_END;

  g_frame.fb = g_fb.px;
  g_frame.width = RDR_SCREEN_W;
  g_frame.height = RDR_SCREEN_H;
  rdr_begin_frame(&g_frame);
  rdr_submit(&g_frame, top);
  rdr_end_frame(&g_frame);
  EXPECT_EQ(g_frame.geom.tris_total, 2U);  // inner drawn twice
}

// ---- robustness ------------------------------------------------------------
TEST(RdrRobust, EmptyStreamEndOnly) {
  struct Command s[1];
  s[0].op = CMD_END;
  g_frame.fb = g_fb.px;
  g_frame.width = RDR_SCREEN_W;
  g_frame.height = RDR_SCREEN_H;
  ASSERT_EQ(rdr_begin_frame(&g_frame), RDR_OK);
  ASSERT_EQ(rdr_submit(&g_frame, s), RDR_OK);
  ASSERT_EQ(rdr_end_frame(&g_frame), RDR_OK);
  EXPECT_EQ(g_frame.geom.tris_total, 0U);
}

TEST(RdrRobust, ClearOnlyFillsFramebuffer) {
  struct Command s[2];
  memset(s, 0, sizeof s);
  s[0].op = CMD_CLEAR;
  s[0].u.clear.color = rgb565(0, 0, 255);  // blue
  s[1].op = CMD_END;
  g_frame.fb = g_fb.px;
  g_frame.width = RDR_SCREEN_W;
  g_frame.height = RDR_SCREEN_H;
  rdr_begin_frame(&g_frame);
  rdr_submit(&g_frame, s);
  rdr_end_frame(&g_frame);
  uint16_t const want = rgb565(0, 0, 255);
  EXPECT_EQ(g_frame.fb[0], want);
  EXPECT_EQ(g_frame.fb[(RDR_SCREEN_W * RDR_SCREEN_H) - 1], want);
}

TEST(RdrRobust, AllBackfaceCulled) {
  struct Mat4fx proj;
  struct OMat4 oproj;
  make_proj(&proj, &oproj, 1.5, 0.5, 50.0);
  // CW-in-NDC (back-facing) winding under CULL_BACK -> dropped.
  struct Vtx verts[3] = {
      mk_cvtx(-6, -6, 12, 255, 0, 0),
      mk_cvtx(0, 6, 12, 255, 0, 0),
      mk_cvtx(6, -6, 12, 255, 0, 0),
  };
  uint16_t idx[3] = {0, 1, 2};
  struct RenderState rs;
  memset(&rs, 0, sizeof rs);
  rs.cull = CULL_BACK;
  struct Command s[5];
  memset(s, 0, sizeof s);
  s[0].op = CMD_SET_MATRIX;
  s[0].u.set_matrix.target = MTX_PROJECTION;
  s[0].u.set_matrix.mat = &proj;
  s[1].op = CMD_SET_MATERIAL;
  s[1].u.set_material.state = &rs;
  s[2].op = CMD_LOAD_VERTS;
  s[2].u.load_verts.ptr = verts;
  s[2].u.load_verts.count = 3;
  s[3].op = CMD_DRAW_TRIS;
  s[3].u.draw_tris.idx = idx;
  s[3].u.draw_tris.tri_count = 1;
  s[4].op = CMD_END;
  g_frame.fb = g_fb.px;
  g_frame.width = RDR_SCREEN_W;
  g_frame.height = RDR_SCREEN_H;
  rdr_begin_frame(&g_frame);
  rdr_submit(&g_frame, s);
  rdr_end_frame(&g_frame);
  EXPECT_EQ(g_frame.geom.tris_total, 0U);
  EXPECT_GE(g_frame.geom.tris_dropped, 1U);
}

TEST(RdrRobust, BehindCameraRejected) {
  struct Mat4fx proj;
  struct OMat4 oproj;
  make_proj(&proj, &oproj, 1.5, 0.5, 50.0);
  // z negative => behind near plane (w <= 0) => whole-tri near reject.
  struct Vtx verts[3] = {
      mk_cvtx(-6, -6, -12, 255, 0, 0),
      mk_cvtx(6, -6, -12, 255, 0, 0),
      mk_cvtx(0, 6, -12, 255, 0, 0),
  };
  uint16_t idx[3] = {0, 1, 2};
  struct RenderState rs;
  memset(&rs, 0, sizeof rs);
  rs.cull = CULL_NONE;
  struct Command s[5];
  memset(s, 0, sizeof s);
  s[0].op = CMD_SET_MATRIX;
  s[0].u.set_matrix.target = MTX_PROJECTION;
  s[0].u.set_matrix.mat = &proj;
  s[1].op = CMD_SET_MATERIAL;
  s[1].u.set_material.state = &rs;
  s[2].op = CMD_LOAD_VERTS;
  s[2].u.load_verts.ptr = verts;
  s[2].u.load_verts.count = 3;
  s[3].op = CMD_DRAW_TRIS;
  s[3].u.draw_tris.idx = idx;
  s[3].u.draw_tris.tri_count = 1;
  s[4].op = CMD_END;
  g_frame.fb = g_fb.px;
  g_frame.width = RDR_SCREEN_W;
  g_frame.height = RDR_SCREEN_H;
  rdr_begin_frame(&g_frame);
  rdr_submit(&g_frame, s);
  rdr_end_frame(&g_frame);
  EXPECT_EQ(g_frame.geom.tris_total, 0U);
}

TEST(RdrRobust, OutOfRangeIndexDropped) {
  struct Mat4fx proj;
  struct OMat4 oproj;
  make_proj(&proj, &oproj, 1.5, 0.5, 50.0);
  struct Vtx verts[3] = {
      mk_cvtx(-6, -6, 12, 255, 0, 0),
      mk_cvtx(6, -6, 12, 255, 0, 0),
      mk_cvtx(0, 6, 12, 255, 0, 0),
  };
  uint16_t idx[3] = {0, 1, 99};  // index 99 out of range (count=3)
  struct RenderState rs;
  memset(&rs, 0, sizeof rs);
  rs.cull = CULL_NONE;
  struct Command s[5];
  memset(s, 0, sizeof s);
  s[0].op = CMD_SET_MATRIX;
  s[0].u.set_matrix.target = MTX_PROJECTION;
  s[0].u.set_matrix.mat = &proj;
  s[1].op = CMD_SET_MATERIAL;
  s[1].u.set_material.state = &rs;
  s[2].op = CMD_LOAD_VERTS;
  s[2].u.load_verts.ptr = verts;
  s[2].u.load_verts.count = 3;
  s[3].op = CMD_DRAW_TRIS;
  s[3].u.draw_tris.idx = idx;
  s[3].u.draw_tris.tri_count = 1;
  s[4].op = CMD_END;
  g_frame.fb = g_fb.px;
  g_frame.width = RDR_SCREEN_W;
  g_frame.height = RDR_SCREEN_H;
  rdr_begin_frame(&g_frame);
  rdr_submit(&g_frame, s);
  rdr_end_frame(&g_frame);
  EXPECT_EQ(g_frame.geom.tris_total, 0U);
  EXPECT_GE(g_frame.geom.tris_dropped, 1U);
}

// ---- golden: a two-triangle quad through the full pipeline -----------------
TEST(RdrGolden, QuadMatchesOracle) {
  struct Mat4fx proj;
  struct OMat4 oproj;
  make_proj(&proj, &oproj, 1.5, 0.5, 50.0);
  // A flat green quad (CCW), two tris sharing the diagonal.
  struct Vtx verts[4] = {
      mk_cvtx(-6, -6, 14, 0, 255, 0),
      mk_cvtx(6, -6, 14, 0, 255, 0),
      mk_cvtx(6, 6, 14, 0, 255, 0),
      mk_cvtx(-6, 6, 14, 0, 255, 0),
  };
  uint16_t idx[6] = {0, 1, 2, 0, 2, 3};
  struct RenderState rs;
  memset(&rs, 0, sizeof rs);
  rs.cull = CULL_NONE;
  struct Command s[5];
  memset(s, 0, sizeof s);
  s[0].op = CMD_SET_MATRIX;
  s[0].u.set_matrix.target = MTX_PROJECTION;
  s[0].u.set_matrix.mat = &proj;
  s[1].op = CMD_SET_MATERIAL;
  s[1].u.set_material.state = &rs;
  s[2].op = CMD_LOAD_VERTS;
  s[2].u.load_verts.ptr = verts;
  s[2].u.load_verts.count = 4;
  s[3].op = CMD_DRAW_TRIS;
  s[3].u.draw_tris.idx = idx;
  s[3].u.draw_tris.tri_count = 2;
  s[4].op = CMD_END;

  g_frame.fb = g_fb.px;
  g_frame.width = RDR_SCREEN_W;
  g_frame.height = RDR_SCREEN_H;
  rdr_begin_frame(&g_frame);
  ASSERT_EQ(rdr_submit(&g_frame, s), RDR_OK);
  rdr_end_frame(&g_frame);
  EXPECT_EQ(g_frame.geom.tris_total, 2U);

  static uint8_t orgb[RDR_SCREEN_W * RDR_SCREEN_H * 3];
  struct OImage oimg = {orgb, RDR_SCREEN_W, RDR_SCREEN_H};
  oracle_image_clear(&oimg, 0, 0, 0);
  struct OViewport const ovp = {0, 0, RDR_SCREEN_W, RDR_SCREEN_H};
  struct OTVtx ot[4];
  for (int k = 0; k < 4; ++k) {
    struct OVec4 const om = {(float)verts[k].pos[0], (float)verts[k].pos[1],
                             (float)verts[k].pos[2], 1.0F};
    oracle_xform_vertex(&ot[k], &oproj, &om, 0.0F, 0.0F, &ovp);
  }
  oracle_fill_tri(&oimg, ot[0].sx, ot[0].sy, ot[1].sx, ot[1].sy, ot[2].sx,
                  ot[2].sy, 0, 255, 0);
  oracle_fill_tri(&oimg, ot[0].sx, ot[0].sy, ot[2].sx, ot[2].sy, ot[3].sx,
                  ot[3].sy, 0, 255, 0);
  int const bad = count_mismatch(g_frame.fb, &oimg, 12);
  int const total = RDR_SCREEN_W * RDR_SCREEN_H;
  EXPECT_LT(bad, total / 150) << "mismatched pixels: " << bad << "/" << total;
}

// ---- depth: a near triangle occludes a far one (per-tile w-buffer) ---------
TEST(RdrDepth, NearOccludesFar) {
  struct Mat4fx proj;
  struct OMat4 oproj;
  make_proj(&proj, &oproj, 1.5, 0.5, 50.0);
  // Two overlapping tris at the screen center: red far (z=20), green near
  // (z=8). Green must win at the shared center pixel.
  struct Vtx verts[6] = {
      mk_cvtx(-6, -6, 20, 255, 0, 0), mk_cvtx(6, -6, 20, 255, 0, 0),
      mk_cvtx(0, 6, 20, 255, 0, 0),   mk_cvtx(-6, -6, 8, 0, 255, 0),
      mk_cvtx(6, -6, 8, 0, 255, 0),   mk_cvtx(0, 6, 8, 0, 255, 0),
  };
  uint16_t idx[6] = {0, 1, 2, 3, 4, 5};
  struct RenderState rs;
  memset(&rs, 0, sizeof rs);
  rs.cull = CULL_NONE;
  struct Command s[5];
  memset(s, 0, sizeof s);
  s[0].op = CMD_SET_MATRIX;
  s[0].u.set_matrix.target = MTX_PROJECTION;
  s[0].u.set_matrix.mat = &proj;
  s[1].op = CMD_SET_MATERIAL;
  s[1].u.set_material.state = &rs;
  s[2].op = CMD_LOAD_VERTS;
  s[2].u.load_verts.ptr = verts;
  s[2].u.load_verts.count = 6;
  s[3].op = CMD_DRAW_TRIS;
  s[3].u.draw_tris.idx = idx;
  s[3].u.draw_tris.tri_count = 2;
  s[4].op = CMD_END;

  g_frame.fb = g_fb.px;
  g_frame.width = RDR_SCREEN_W;
  g_frame.height = RDR_SCREEN_H;
  rdr_begin_frame(&g_frame);
  rdr_submit(&g_frame, s);
  rdr_end_frame(&g_frame);
  // Center pixel: the nearer (green) triangle wins.
  int const cidx = ((RDR_SCREEN_H / 2) * RDR_SCREEN_W) + (RDR_SCREEN_W / 2);
  uint8_t r;
  uint8_t g;
  uint8_t b;
  oracle_unpack565(g_frame.fb[cidx], &r, &g, &b);
  EXPECT_GT((int)g, 200);
  EXPECT_LT((int)r, 60);
}

// ---- lit path: a directional-lit triangle renders non-background ----------
TEST(RdrLit, DirectionalLitTriangleDraws) {
  struct Mat4fx proj;
  struct OMat4 oproj;
  make_proj(&proj, &oproj, 1.5, 0.5, 50.0);
  // Lit verts: normals toward +z (the light), so they receive full diffuse.
  struct Vtx verts[3];
  for (int k = 0; k < 3; ++k) {
    memset(&verts[k], 0, sizeof verts[k]);
    verts[k].c.nrm.n[2] = 127;  // normal +z
    verts[k].c.nrm.a = 255;
  }
  verts[0].pos[0] = -6;
  verts[0].pos[1] = -6;
  verts[0].pos[2] = 12;
  verts[1].pos[0] = 6;
  verts[1].pos[1] = -6;
  verts[1].pos[2] = 12;
  verts[2].pos[0] = 0;
  verts[2].pos[1] = 6;
  verts[2].pos[2] = 12;
  uint16_t idx[3] = {0, 1, 2};

  struct RenderState rs;
  memset(&rs, 0, sizeof rs);
  rs.cull = CULL_NONE;
  rs.lit = 1;
  rs.lights.count = 1;
  rs.lights.dirs[0].dir[2] = 127;  // light from +z
  rs.lights.dirs[0].rgb[0] = 255;
  rs.lights.dirs[0].rgb[1] = 255;
  rs.lights.dirs[0].rgb[2] = 255;
  rs.lights.ambient[0] = 32;
  rs.lights.ambient[1] = 32;
  rs.lights.ambient[2] = 32;

  struct Command s[5];
  memset(s, 0, sizeof s);
  s[0].op = CMD_SET_MATRIX;
  s[0].u.set_matrix.target = MTX_PROJECTION;
  s[0].u.set_matrix.mat = &proj;
  s[1].op = CMD_SET_MATERIAL;
  s[1].u.set_material.state = &rs;
  s[2].op = CMD_LOAD_VERTS;
  s[2].u.load_verts.ptr = verts;
  s[2].u.load_verts.count = 3;
  s[3].op = CMD_DRAW_TRIS;
  s[3].u.draw_tris.idx = idx;
  s[3].u.draw_tris.tri_count = 1;
  s[4].op = CMD_END;

  g_frame.fb = g_fb.px;
  g_frame.width = RDR_SCREEN_W;
  g_frame.height = RDR_SCREEN_H;
  rdr_begin_frame(&g_frame);
  rdr_submit(&g_frame, s);
  rdr_end_frame(&g_frame);
  EXPECT_EQ(g_frame.geom.tris_total, 1U);
  // The triangle center should be bright (lit white-ish), not background.
  int const cidx = ((RDR_SCREEN_H / 2) * RDR_SCREEN_W) + (RDR_SCREEN_W / 2);
  uint8_t r;
  uint8_t g;
  uint8_t b;
  oracle_unpack565(g_frame.fb[cidx], &r, &g, &b);
  EXPECT_GT((int)r + (int)g + (int)b, 300);
}

// ---- keep the original contract canary -------------------------------------
TEST(Rdr, ContractCompiles) { SUCCEED(); }
