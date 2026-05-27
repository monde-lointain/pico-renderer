// golden.cc — golden-image compare. See golden.h.

#include "golden.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "png_io.h"

static int file_exists(const char* path) {
  FILE* f = fopen(path, "rb");
  if (f) {
    fclose(f);
    return 1;
  }
  return 0;
}

static int env_regen(void) {
  const char* e = getenv("GOLDEN_REGEN");
  return (e && e[0]) ? 1 : 0;
}

// Build "<dir>/<basename-of-golden>.<suffix>.png" into out.
static void dump_path(char* out, size_t cap, const char* dir,
                      const char* golden_path, const char* suffix) {
  const char* base = golden_path;
  for (const char* p = golden_path; *p; ++p) {
    if (*p == '/' || *p == '\\') {
      base = p + 1;
    }
  }
  char stem[256];
  size_t n = strlen(base);
  // strip trailing ".png" if present
  if (n >= 4 && strcmp(base + n - 4, ".png") == 0) {
    n -= 4;
  }
  if (n >= sizeof(stem)) {
    n = sizeof(stem) - 1;
  }
  memcpy(stem, base, n);
  stem[n] = 0;
  snprintf(out, cap, "%s/%s.%s.png", dir, stem, suffix);
}

int golden_check(const struct GoldenParams* params, const uint8_t* actual,
                 int width, int height, struct GoldenReport* report) {
  report->result = GOLDEN_ERROR;
  report->diff_pixels = 0;
  report->max_channel_diff = 0;
  report->first_x = -1;
  report->first_y = -1;

  if (!params || !params->golden_path || !actual || width <= 0 || height <= 0) {
    return GOLDEN_ERROR;
  }

  int const want_regen = params->regen || env_regen();
  if (want_regen || !file_exists(params->golden_path)) {
    if (png_write_rgb8(params->golden_path, actual, width, height) != 0) {
      report->result = GOLDEN_ERROR;
      return GOLDEN_ERROR;
    }
    report->result = GOLDEN_WROTE;
    return GOLDEN_WROTE;
  }

  uint8_t* gold = NULL;
  int gw = 0;
  int gh = 0;
  if (png_read_rgb8(params->golden_path, &gold, &gw, &gh) != 0) {
    report->result = GOLDEN_ERROR;
    return GOLDEN_ERROR;
  }
  if (gw != width || gh != height) {
    free(gold);
    report->result = GOLDEN_ERROR;
    return GOLDEN_ERROR;
  }

  int diff_pixels = 0;
  int max_cd = 0;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      size_t const o = (((size_t)y * width) + x) * 3;
      int worst = 0;
      for (int c = 0; c < 3; ++c) {
        int d = (int)actual[o + c] - (int)gold[o + c];
        if (d < 0) {
          d = -d;
        }
        if (d > worst) {
          worst = d;
        }
      }
      if (worst > max_cd) {
        max_cd = worst;
      }
      if (worst > params->per_channel) {
        if (report->first_x < 0) {
          report->first_x = x;
          report->first_y = y;
        }
        ++diff_pixels;
      }
    }
  }
  report->diff_pixels = diff_pixels;
  report->max_channel_diff = max_cd;

  int const pass = (diff_pixels <= params->max_diff_pixels);
  report->result = pass ? GOLDEN_PASS : GOLDEN_FAIL;

  if (!pass && params->dump_dir) {
    char ap[512];
    dump_path(ap, sizeof ap, params->dump_dir, params->golden_path, "actual");
    png_write_rgb8(ap, actual, width, height);

    // Diff image: |actual-gold| per channel, amplified to be visible.
    uint8_t* diff = (uint8_t*)malloc((size_t)width * height * 3);
    if (diff) {
      for (size_t i = 0; i < (size_t)width * height * 3; ++i) {
        int d = (int)actual[i] - (int)gold[i];
        if (d < 0) {
          d = -d;
        }
        d *= 4;
        diff[i] = (uint8_t)(d > 255 ? 255 : d);
      }
      char dp[512];
      dump_path(dp, sizeof dp, params->dump_dir, params->golden_path, "diff");
      png_write_rgb8(dp, diff, width, height);
      free(diff);
    }
    fprintf(stderr,
            "[golden] FAIL %s: %d pixels exceed per-channel tol %d "
            "(max diff %d, first @ %d,%d). Dumps in %s\n",
            params->golden_path, diff_pixels, params->per_channel, max_cd,
            report->first_x, report->first_y, params->dump_dir);
  }

  free(gold);
  return report->result;
}
