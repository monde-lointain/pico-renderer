// png_io_test.cc — round-trip the minimal PNG codec.

#include "png_io.h"

#include <gtest/gtest.h>
#include <stdlib.h>
#include <string.h>

static const char* tmp_png() {
  static char path[256];
  snprintf(path, sizeof path, "%s/harness_png_io_test.png",
           ::testing::TempDir().c_str());
  return path;
}

TEST(PngIo, RoundTripSmall) {
  const int W = 7, H = 5;  // odd dims to exercise row strides
  uint8_t src[W * H * 3];
  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W; ++x) {
      size_t o = ((size_t)y * W + x) * 3;
      src[o + 0] = (uint8_t)(x * 30 + 1);
      src[o + 1] = (uint8_t)(y * 50 + 2);
      src[o + 2] = (uint8_t)((x + y) * 20 + 3);
    }
  }
  const char* p = tmp_png();
  ASSERT_EQ(png_write_rgb8(p, src, W, H), 0);

  uint8_t* got = nullptr;
  int gw = 0, gh = 0;
  ASSERT_EQ(png_read_rgb8(p, &got, &gw, &gh), 0);
  ASSERT_EQ(gw, W);
  ASSERT_EQ(gh, H);
  ASSERT_NE(got, nullptr);
  EXPECT_EQ(memcmp(src, got, sizeof src), 0);
  free(got);
}

TEST(PngIo, RoundTripLargerThanOneBlock) {
  // Force multiple STORED blocks (>65535 raw bytes).
  const int W = 200, H = 200;  // raw = (200*3+1)*200 = 120200 bytes > 65535
  uint8_t* src = (uint8_t*)malloc((size_t)W * H * 3);
  ASSERT_NE(src, nullptr);
  for (size_t i = 0; i < (size_t)W * H * 3; ++i) src[i] = (uint8_t)(i * 7 + 11);
  const char* p = tmp_png();
  ASSERT_EQ(png_write_rgb8(p, src, W, H), 0);

  uint8_t* got = nullptr;
  int gw = 0, gh = 0;
  ASSERT_EQ(png_read_rgb8(p, &got, &gw, &gh), 0);
  ASSERT_EQ(gw, W);
  ASSERT_EQ(gh, H);
  EXPECT_EQ(memcmp(src, got, (size_t)W * H * 3), 0);
  free(got);
  free(src);
}

TEST(PngIo, ReadMissingFails) {
  uint8_t* got = nullptr;
  int gw = 0, gh = 0;
  EXPECT_NE(png_read_rgb8("/nonexistent/dir/nope.png", &got, &gw, &gh), 0);
}
