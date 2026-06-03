// clip_test.cc — B.1-beta guard-band clip tests. Host-first, gtest.
#include "clip/clip.h"

#include <stdint.h>

#include "gtest/gtest.h"
#include "rdr/config.h"

// Q12.4 helper.
static fx12_4 q4(int px) { return (fx12_4)(px * 16); }

static struct TVtx mk(int x, int y) {
  struct TVtx v;
  v.x = q4(x);
  v.y = q4(y);
  v.inv_w = 0;
  v.u_iw = 0;
  v.v_iw = 0;
  v.rgba = 0;
  v.fog = 0;
  return v;
}

// A triangle wholly inside the guard band passes through unchanged (still 3).
TEST(Clip, InteriorTrianglePassesThrough) {
  struct TVtx in[3] = {mk(10, 10), mk(100, 10), mk(50, 100)};
  struct TVtx out[CLIP_MAX_OUT];
  int const n = clip_tri(in, 3, out);
  ASSERT_EQ(n, 3);
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(out[i].x, in[i].x);
    EXPECT_EQ(out[i].y, in[i].y);
  }
}

// A triangle wholly OUTSIDE the guard band (far past +x) is fully clipped.
TEST(Clip, FullyOutsideClipsToNothing) {
  int const farx = RDR_SCREEN_W + RDR_GUARD_X + 100;
  struct TVtx in[3] = {mk(farx, 10), mk(farx + 50, 10), mk(farx + 25, 60)};
  struct TVtx out[CLIP_MAX_OUT];
  EXPECT_EQ(clip_tri(in, 3, out), 0);
}

// A triangle straddling one guard-band edge gains vertices (becomes a quad+).
TEST(Clip, StraddleOneEdgeAddsVertices) {
  struct ClipRect r;
  clip_guard_rect(&r);
  // Triangle crossing the right edge: two verts inside, apex outside.
  int const maxx_px = r.maxx / 16;
  struct TVtx in[3] = {mk(maxx_px - 100, -10), mk(maxx_px - 100, 200),
                       mk(maxx_px + 200, 95)};
  struct TVtx out[CLIP_MAX_OUT];
  int const n = clip_poly_rect(in, 3, &r, out);
  EXPECT_GE(n, 4);
  EXPECT_LE(n, CLIP_MAX_OUT);
  // Every output vertex must lie within the rect.
  for (int i = 0; i < n; ++i) {
    EXPECT_GE(out[i].x, r.minx);
    EXPECT_LE(out[i].x, r.maxx);
    EXPECT_GE(out[i].y, r.miny);
    EXPECT_LE(out[i].y, r.maxy);
  }
}

// Linear attribute interpolation: clip a horizontal edge crossing a vertical
// clip line at a known midpoint; the inserted vertex's inv_w is the lerp.
TEST(Clip, InterpolatesAttributesLinearly) {
  struct ClipRect r;
  r.minx = q4(0);
  r.miny = q4(-1000);
  r.maxx = q4(100);
  r.maxy = q4(1000);
  // Edge from x=0 (inv_w=0) to x=200 (inv_w=200). Clip at x=100 -> inv_w=100.
  struct TVtx a = mk(0, 0);
  a.inv_w = 0;
  struct TVtx b = mk(200, 0);
  b.inv_w = 200;
  struct TVtx c = mk(0, 50);
  c.inv_w = 0;
  struct TVtx in[3] = {a, b, c};
  struct TVtx out[CLIP_MAX_OUT];
  int const n = clip_poly_rect(in, 3, &r, out);
  ASSERT_GT(n, 0);
  // Find the vertex placed on the maxx clip line.
  int found = 0;
  for (int i = 0; i < n; ++i) {
    if (out[i].x == r.maxx) {
      found = 1;
      EXPECT_NEAR(out[i].inv_w, 100, 1);  // 1 ulp for fixed rounding
    }
  }
  EXPECT_TRUE(found);
}

// D1: clip carries the fog attribute through the per-edge lerp (R.3-fog reads
// it post-clip). Same setup as the inv_w lerp: an edge fog 0->200 clipped at
// the x=100 midpoint must yield fog~=100 on the inserted vertex.
TEST(Clip, CarriesFogThroughLerp) {
  struct ClipRect r;
  r.minx = q4(0);
  r.miny = q4(-1000);
  r.maxx = q4(100);
  r.maxy = q4(1000);
  struct TVtx a = mk(0, 0);
  a.fog = 0;
  struct TVtx b = mk(200, 0);
  b.fog = 200;
  struct TVtx c = mk(0, 50);
  c.fog = 0;
  struct TVtx in[3] = {a, b, c};
  struct TVtx out[CLIP_MAX_OUT];
  int const n = clip_poly_rect(in, 3, &r, out);
  ASSERT_GT(n, 0);
  int found = 0;
  for (int i = 0; i < n; ++i) {
    if (out[i].x == r.maxx) {
      found = 1;
      EXPECT_NEAR((int)out[i].fog, 100, 1);  // midpoint lerp, 1 ulp
    }
  }
  EXPECT_TRUE(found);
}

// Degenerate input (n<3) clips to nothing.
TEST(Clip, DegenerateInputRejected) {
  struct TVtx in[3] = {mk(10, 10), mk(20, 20), mk(30, 30)};
  struct TVtx out[CLIP_MAX_OUT];
  EXPECT_EQ(clip_tri(in, 2, out), 0);
}
