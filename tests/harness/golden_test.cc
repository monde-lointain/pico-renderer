// golden_test.cc — exercise the golden-compare framework, INCLUDING the
// mandatory injected-mismatch failure (proves the comparison is wired before
// any other stream trusts a green run).

#include "golden.h"

#include <gtest/gtest.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "gfx/framebuffer.h"  // SCREEN_W/H, rgb565()
#include "oracle.h"

static void temp_path(char* out, size_t cap, const char* name) {
  snprintf(out, cap, "%s/%s", ::testing::TempDir().c_str(), name);
}

// Render a small deterministic scene through the oracle: clear + one tri.
static void render_scene(uint8_t* rgb, int w, int h, int tweak) {
  struct OImage img = {rgb, w, h};
  oracle_image_clear(&img, 16, 16, 32);
  // tweak shifts one vertex by 1px to create a controlled mismatch.
  oracle_fill_tri(&img, 8.0F, 8.0F, (float)(w - 8 + tweak), 8.0F, 8.0F,
                  (float)(h - 8), 220, 40, 40);
}

TEST(Golden, FirstRunWritesThenComparePasses) {
  const int width = 48;
  const int height = 48;
  uint8_t buf[width * height * 3];
  render_scene(buf, width, height, 0);

  char gpath[512];
  temp_path(gpath, sizeof gpath, "golden_firstrun.png");
  remove(gpath);  // ensure missing

  struct GoldenParams p;
  memset(&p, 0, sizeof p);
  p.golden_path = gpath;
  p.per_channel = 0;
  p.max_diff_pixels = 0;

  struct GoldenReport rep;
  // First run: golden missing -> WROTE.
  EXPECT_EQ(golden_check(&p, buf, width, height, &rep), GOLDEN_WROTE);

  // Second run, identical render -> PASS (exact, zero tolerance).
  EXPECT_EQ(golden_check(&p, buf, width, height, &rep), GOLDEN_PASS);
  EXPECT_EQ(rep.diff_pixels, 0);
  remove(gpath);
}

// THE wiring proof: a known-wrong image MUST fail.
TEST(Golden, InjectedMismatchFails) {
  const int width = 48;
  const int height = 48;
  uint8_t good[width * height * 3];
  uint8_t bad[width * height * 3];
  render_scene(good, width, height, 0);
  render_scene(bad, width, height,
               6);  // moved a vertex 6px -> many differing pixels

  char gpath[512];
  temp_path(gpath, sizeof gpath, "golden_inject.png");
  remove(gpath);

  struct GoldenParams p;
  memset(&p, 0, sizeof p);
  p.golden_path = gpath;
  std::string const tmp_dir = ::testing::TempDir();
  p.dump_dir = tmp_dir.c_str();
  p.per_channel = 8;
  p.max_diff_pixels = 0;

  struct GoldenReport rep;
  // Establish golden from the GOOD render.
  ASSERT_EQ(golden_check(&p, good, width, height, &rep), GOLDEN_WROTE);

  // Compare the BAD render -> MUST FAIL.
  int const r = golden_check(&p, bad, width, height, &rep);
  EXPECT_EQ(r, GOLDEN_FAIL);
  EXPECT_GT(rep.diff_pixels, 0);
  EXPECT_GE(rep.first_x, 0);
  EXPECT_GE(rep.first_y, 0);
  remove(gpath);
}

TEST(Golden, ToleranceAbsorbsSmallNoise) {
  const int width = 32;
  const int height = 32;
  uint8_t base[width * height * 3];
  uint8_t noisy[width * height * 3];
  render_scene(base, width, height, 0);
  memcpy(noisy, base, sizeof base);
  // Perturb a handful of pixels by a small amount within per-channel tol.
  for (size_t i = 0; i < 5; ++i) {
    noisy[i * 3] = (uint8_t)(base[i * 3] + 3);
  }

  char gpath[512];
  temp_path(gpath, sizeof gpath, "golden_tol.png");
  remove(gpath);

  struct GoldenParams p;
  memset(&p, 0, sizeof p);
  p.golden_path = gpath;
  p.per_channel = 4;  // 3 <= 4: absorbed
  p.max_diff_pixels = 0;

  struct GoldenReport rep;
  ASSERT_EQ(golden_check(&p, base, width, height, &rep), GOLDEN_WROTE);
  EXPECT_EQ(golden_check(&p, noisy, width, height, &rep), GOLDEN_PASS);
  EXPECT_EQ(rep.diff_pixels, 0);  // all within per-channel tol

  // Tighten per-channel below the noise -> now those pixels count, and with a
  // zero pixel budget it fails.
  p.per_channel = 2;
  EXPECT_EQ(golden_check(&p, noisy, width, height, &rep), GOLDEN_FAIL);
  EXPECT_EQ(rep.diff_pixels, 5);
  remove(gpath);
}

TEST(Golden, PixelBudgetAllowsBoundedDifference) {
  const int width = 32;
  const int height = 32;
  uint8_t base[width * height * 3];
  uint8_t noisy[width * height * 3];
  render_scene(base, width, height, 0);
  memcpy(noisy, base, sizeof base);
  for (size_t i = 0; i < 3; ++i) {
    noisy[i * 3] = (uint8_t)(base[i * 3] ^ 0x40);
  }

  char gpath[512];
  temp_path(gpath, sizeof gpath, "golden_budget.png");
  remove(gpath);

  struct GoldenParams p;
  memset(&p, 0, sizeof p);
  p.golden_path = gpath;
  p.per_channel = 1;
  p.max_diff_pixels = 3;  // exactly the number of perturbed pixels

  struct GoldenReport rep;
  ASSERT_EQ(golden_check(&p, base, width, height, &rep), GOLDEN_WROTE);
  EXPECT_EQ(golden_check(&p, noisy, width, height, &rep),
            GOLDEN_PASS);  // 3 <= 3
  p.max_diff_pixels = 2;
  EXPECT_EQ(golden_check(&p, noisy, width, height, &rep),
            GOLDEN_FAIL);  // 3 > 2
  remove(gpath);
}

// End-to-end against a COMMITTED golden (regression anchor): full transform +
// raster pipeline rendered to a 240x240 RGB565-derived image, compared to a
// PNG checked into tests/harness/golden/. Set GOLDEN_REGEN=1 to refresh.
TEST(Golden, CommittedSceneRegression) {
  const int width = SCREEN_W;
  const int height = SCREEN_H;
  std::vector<uint8_t> buf((size_t)width * height * 3);
  struct OImage img = {buf.data(), width, height};

  // Background from a 565 clear color, unpacked the same way the device will.
  uint8_t br;
  uint8_t bg;
  uint8_t bb;
  oracle_unpack565(rgb565(20, 30, 60), &br, &bg, &bb);
  oracle_image_clear(&img, br, bg, bb);

  // Transform a unit quad (two tris) through the oracle and fill it.
  struct OMat4 mvp;
  for (int i = 0; i < 16; ++i) {
    mvp.m[i] = 0.0F;
  }
  mvp.m[0] = mvp.m[5] = mvp.m[10] = mvp.m[15] = 1.0F;  // identity
  struct OViewport const vp = {0, 0, width, height};

  struct OVec4 const quad[4] = {{-0.6F, -0.6F, 0.0F, 1.0F},
                                {0.6F, -0.6F, 0.0F, 1.0F},
                                {0.6F, 0.6F, 0.0F, 1.0F},
                                {-0.6F, 0.6F, 0.0F, 1.0F}};
  struct OTVtx tv[4];
  for (int i = 0; i < 4; ++i) {
    oracle_xform_vertex(&tv[i], &mvp, &quad[i], 0.0F, 0.0F, &vp);
  }

  uint8_t qr;
  uint8_t qg;
  uint8_t qb;
  oracle_unpack565(rgb565(200, 180, 40), &qr, &qg, &qb);
  oracle_fill_tri(&img, tv[0].sx, tv[0].sy, tv[1].sx, tv[1].sy, tv[2].sx,
                  tv[2].sy, qr, qg, qb);
  oracle_fill_tri(&img, tv[0].sx, tv[0].sy, tv[2].sx, tv[2].sy, tv[3].sx,
                  tv[3].sy, qr, qg, qb);

  struct GoldenParams p;
  memset(&p, 0, sizeof p);
  p.golden_path = HARNESS_GOLDEN_DIR "/scene_quad.png";
  std::string const tmp_dir = ::testing::TempDir();
  p.dump_dir = tmp_dir.c_str();
  p.per_channel = 0;  // oracle is deterministic; exact match expected
  p.max_diff_pixels = 0;

  struct GoldenReport rep;
  int const r = golden_check(&p, buf.data(), width, height, &rep);
  // PASS (golden present, matches) or WROTE (first time / regen). Never FAIL.
  EXPECT_NE(r, GOLDEN_FAIL);
  EXPECT_NE(r, GOLDEN_ERROR);
}
