// oracle_test.cc — self-tests for the float oracle. Known transform -> known
// output; raster fill rule; format conversions.

#include "oracle.h"

#include <gtest/gtest.h>
#include <math.h>

static struct OMat4 identity() {
  struct OMat4 m;
  for (int i = 0; i < 16; ++i) m.m[i] = 0.0f;
  m.m[0] = m.m[5] = m.m[10] = m.m[15] = 1.0f;
  return m;
}

TEST(Oracle, FxToFloat) {
  EXPECT_FLOAT_EQ(omat_from_fx(65536), 1.0f);
  EXPECT_FLOAT_EQ(omat_from_fx(32768), 0.5f);
  EXPECT_FLOAT_EQ(omat_from_fx(-65536), -1.0f);
}

TEST(Oracle, Q12_4Rounding) {
  EXPECT_EQ(oracle_to_q12_4(1.0f), 16);
  EXPECT_EQ(oracle_to_q12_4(0.5f), 8);
  EXPECT_EQ(oracle_to_q12_4(120.0f), 1920);
}

TEST(Oracle, MatMulIdentity) {
  struct OMat4 id = identity();
  struct OVec4 v = {3.0f, -2.0f, 5.0f, 1.0f};
  struct OVec4 out;
  oracle_mat_mul_vec(&out, &id, &v);
  EXPECT_FLOAT_EQ(out.x, 3.0f);
  EXPECT_FLOAT_EQ(out.y, -2.0f);
  EXPECT_FLOAT_EQ(out.z, 5.0f);
  EXPECT_FLOAT_EQ(out.w, 1.0f);
}

TEST(Oracle, MatMulTranslation) {
  // Column-major translation: m[12..14] = tx,ty,tz.
  struct OMat4 t = identity();
  t.m[12] = 10.0f;
  t.m[13] = 20.0f;
  t.m[14] = -5.0f;
  struct OVec4 v = {1.0f, 2.0f, 3.0f, 1.0f};
  struct OVec4 out;
  oracle_mat_mul_vec(&out, &t, &v);
  EXPECT_FLOAT_EQ(out.x, 11.0f);
  EXPECT_FLOAT_EQ(out.y, 22.0f);
  EXPECT_FLOAT_EQ(out.z, -2.0f);
}

TEST(Oracle, MatComposeAppliesRhsFirst) {
  // a = translate(+10 x), b = scale(2). out = a*b applies scale then translate.
  struct OMat4 a = identity();
  a.m[12] = 10.0f;
  struct OMat4 b = identity();
  b.m[0] = b.m[5] = b.m[10] = 2.0f;
  struct OMat4 out;
  oracle_mat_mul(&out, &a, &b);
  struct OVec4 v = {1.0f, 1.0f, 1.0f, 1.0f};
  struct OVec4 r;
  oracle_mat_mul_vec(&r, &out, &v);
  EXPECT_FLOAT_EQ(r.x, 12.0f);  // 1*2 + 10
  EXPECT_FLOAT_EQ(r.y, 2.0f);
}

TEST(Oracle, XformVertexCenterMapsToViewportCenter) {
  // Identity MVP, clip = model with w=1. NDC (0,0) -> viewport center.
  struct OMat4 id = identity();
  struct OViewport vp = {0, 0, 240, 240};
  struct OVec4 pos = {0.0f, 0.0f, 0.0f, 1.0f};
  struct OTVtx out;
  oracle_xform_vertex(&out, &id, &pos, 0.0f, 0.0f, &vp);
  EXPECT_EQ(out.clipped, 0);
  EXPECT_FLOAT_EQ(out.sx, 120.0f);
  EXPECT_FLOAT_EQ(out.sy, 120.0f);
  EXPECT_FLOAT_EQ(out.inv_w, 1.0f);
}

TEST(Oracle, XformVertexCornersAndYFlip) {
  struct OMat4 id = identity();
  struct OViewport vp = {0, 0, 240, 240};
  struct OVec4 top_right = {1.0f, 1.0f, 0.0f, 1.0f};  // NDC +x,+y
  struct OTVtx o;
  oracle_xform_vertex(&o, &id, &top_right, 0.0f, 0.0f, &vp);
  // +x NDC -> right edge (240). +y NDC -> top of screen (y=0, flipped).
  EXPECT_FLOAT_EQ(o.sx, 240.0f);
  EXPECT_FLOAT_EQ(o.sy, 0.0f);
}

TEST(Oracle, XformVertexPerspectiveDivide) {
  struct OMat4 id = identity();
  struct OViewport vp = {0, 0, 240, 240};
  // w=2 -> ndc_x = 0.5, sx = 120 + 0.5*120 = 180. inv_w = 0.5.
  struct OVec4 pos = {1.0f, 0.0f, 0.0f, 2.0f};
  struct OTVtx o;
  oracle_xform_vertex(&o, &id, &pos, 4.0f, 8.0f, &vp);
  EXPECT_FLOAT_EQ(o.inv_w, 0.5f);
  EXPECT_FLOAT_EQ(o.sx, 180.0f);
  EXPECT_FLOAT_EQ(o.u_iw, 2.0f);  // 4 * 0.5
  EXPECT_FLOAT_EQ(o.v_iw, 4.0f);  // 8 * 0.5
}

TEST(Oracle, XformVertexBehindNearClipped) {
  struct OMat4 id = identity();
  struct OViewport vp = {0, 0, 240, 240};
  struct OVec4 pos = {0.0f, 0.0f, 0.0f, -1.0f};
  struct OTVtx o;
  oracle_xform_vertex(&o, &id, &pos, 0.0f, 0.0f, &vp);
  EXPECT_NE(o.clipped, 0);
}

TEST(Oracle, Unpack565) {
  // Pure red 565 = 0xF800 -> ~ (255,0,0).
  uint8_t r, g, b;
  oracle_unpack565(0xF800, &r, &g, &b);
  EXPECT_EQ(r, 255);
  EXPECT_EQ(g, 0);
  EXPECT_EQ(b, 0);
  oracle_unpack565(0x07E0, &r, &g, &b);  // pure green
  EXPECT_EQ(r, 0);
  EXPECT_EQ(g, 255);
  EXPECT_EQ(b, 0);
  oracle_unpack565(0x001F, &r, &g, &b);  // pure blue
  EXPECT_EQ(b, 255);
}

// ---- raster ---------------------------------------------------------------
static int count_filled(const uint8_t* rgb, int n, uint8_t r, uint8_t g,
                        uint8_t b) {
  int c = 0;
  for (int i = 0; i < n; ++i) {
    if (rgb[i * 3] == r && rgb[i * 3 + 1] == g && rgb[i * 3 + 2] == b) ++c;
  }
  return c;
}

TEST(Oracle, FillAxisAlignedRightTriangleCoversHalf) {
  const int W = 10, H = 10;
  uint8_t buf[W * H * 3];
  struct OImage img = {buf, W, H};
  oracle_image_clear(&img, 0, 0, 0);
  // Right triangle covering the lower-left half of a 10x10 box.
  oracle_fill_tri(&img, 0, 0, 10, 0, 0, 10, 255, 0, 0);
  int filled = count_filled(buf, W * H, 255, 0, 0);
  // Top-left rule on a 10x10 right triangle: ~ n*(n+1)/2 = 55 pixels.
  EXPECT_GT(filled, 40);
  EXPECT_LT(filled, 60);
}

TEST(Oracle, FillFullScreenTwoTris) {
  const int W = 16, H = 16;
  uint8_t buf[W * H * 3];
  struct OImage img = {buf, W, H};
  oracle_image_clear(&img, 0, 0, 0);
  // Two tris covering [0,W]x[0,H] exactly; top-left rule => no double/no gaps.
  oracle_fill_tri(&img, 0, 0, W, 0, 0, H, 9, 9, 9);
  oracle_fill_tri(&img, W, 0, W, H, 0, H, 9, 9, 9);
  int filled = count_filled(buf, W * H, 9, 9, 9);
  EXPECT_EQ(filled, W * H);  // shared diagonal owned by exactly one tri
}

TEST(Oracle, FillDegenerateNoop) {
  const int W = 4, H = 4;
  uint8_t buf[W * H * 3];
  struct OImage img = {buf, W, H};
  oracle_image_clear(&img, 0, 0, 0);
  oracle_fill_tri(&img, 0, 0, 4, 4, 2, 2, 255, 255, 255);  // collinear
  EXPECT_EQ(count_filled(buf, W * H, 255, 255, 255), 0);
}

TEST(Oracle, FillWindingIndependent) {
  const int W = 8, H = 8;
  uint8_t a[W * H * 3], b[W * H * 3];
  struct OImage ia = {a, W, H};
  struct OImage ib = {b, W, H};
  oracle_image_clear(&ia, 0, 0, 0);
  oracle_image_clear(&ib, 0, 0, 0);
  oracle_fill_tri(&ia, 1, 1, 6, 1, 1, 6, 200, 100, 50);  // CW or CCW
  oracle_fill_tri(&ib, 1, 1, 1, 6, 6, 1, 200, 100, 50);  // reversed
  // Same coverage regardless of vertex order (winding normalized internally).
  EXPECT_EQ(memcmp(a, b, sizeof a), 0);
}
