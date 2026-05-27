// png_io_test.cc — round-trip the minimal PNG codec.

#include "png_io.h"

#include <gtest/gtest.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

static const char* tmp_png() {
  static char path[256];
  snprintf(path, sizeof path, "%s/harness_png_io_test.png",
           ::testing::TempDir().c_str());
  return path;
}

TEST(PngIo, RoundTripSmall) {
  const int width = 7;
  const int height = 5;  // odd dims to exercise row strides
  uint8_t src[width * height * 3];
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      size_t const o = (((size_t)y * width) + x) * 3;
      src[o + 0] = (uint8_t)((x * 30) + 1);
      src[o + 1] = (uint8_t)((y * 50) + 2);
      src[o + 2] = (uint8_t)(((x + y) * 20) + 3);
    }
  }
  const char* p = tmp_png();
  ASSERT_EQ(png_write_rgb8(p, src, width, height), 0);

  uint8_t* got = nullptr;
  int gw = 0;
  int gh = 0;
  ASSERT_EQ(png_read_rgb8(p, &got, &gw, &gh), 0);
  ASSERT_EQ(gw, width);
  ASSERT_EQ(gh, height);
  ASSERT_NE(got, nullptr);
  EXPECT_EQ(memcmp(src, got, sizeof src), 0);
  free(got);
}

TEST(PngIo, RoundTripLargerThanOneBlock) {
  // Force multiple STORED blocks (>65535 raw bytes).
  const int width = 200;
  const int height = 200;  // raw = (200*3+1)*200 = 120200 bytes > 65535
  std::vector<uint8_t> src((size_t)width * height * 3);
  for (size_t i = 0; i < src.size(); ++i) {
    src[i] = (uint8_t)((i * 7) + 11);
  }
  const char* p = tmp_png();
  ASSERT_EQ(png_write_rgb8(p, src.data(), width, height), 0);

  uint8_t* got = nullptr;
  int gw = 0;
  int gh = 0;
  ASSERT_EQ(png_read_rgb8(p, &got, &gw, &gh), 0);
  ASSERT_EQ(gw, width);
  ASSERT_EQ(gh, height);
  EXPECT_EQ(memcmp(src.data(), got, src.size()), 0);
  free(got);
}

TEST(PngIo, ReadMissingFails) {
  uint8_t* got = nullptr;
  int gw = 0;
  int gh = 0;
  EXPECT_NE(png_read_rgb8("/nonexistent/dir/nope.png", &got, &gw, &gh), 0);
}
