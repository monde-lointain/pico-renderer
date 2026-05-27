// golden.h — golden-image compare framework for the host harness.
//
// Workflow:
//   1. A test renders into an RGB8 buffer (width*height*3, row-major).
//   2. golden_check() compares it against a committed golden PNG:
//        - if the golden file is missing OR regen is requested -> WRITE it,
//          report GOLDEN_WROTE (test should treat first-run write as a soft
//          pass but flag that no comparison happened).
//        - else READ it back and compare within tolerance.
//   3. Tolerance has two knobs (both must hold to PASS):
//        - per_channel: max allowed abs difference on any R/G/B channel.
//        - max_diff_pixels: max number of pixels allowed to exceed per_channel.
//   4. On mismatch a diff report is filled in (counts + first offending pixel)
//      and, if dump_dir is set, actual + diff PNGs are written there.
//
// Regen is controlled by the GOLDEN_REGEN environment variable (any non-empty
// value) or the `regen` flag on the params struct.
//
// This file is part of the harness carve-out (NOT orthodoxy_enforced) but stays
// C-like: POD structs + free functions, error codes, no exceptions.
#ifndef HARNESS_GOLDEN_H
#define HARNESS_GOLDEN_H

#include <stdint.h>

enum GoldenResult {
  GOLDEN_PASS = 0,   // within tolerance
  GOLDEN_FAIL = 1,   // exceeded tolerance (see GoldenReport)
  GOLDEN_WROTE = 2,  // golden was missing/regenerated; written, not compared
  GOLDEN_ERROR = 3   // I/O / size mismatch / internal error
};

struct GoldenParams {
  const char* golden_path;  // path to the committed golden PNG
  const char* dump_dir;     // dir for actual/diff dumps on fail (NULL = none)
  int per_channel;          // max abs per-channel diff tolerated per pixel
  int max_diff_pixels;      // max count of pixels exceeding per_channel
  int regen;                // nonzero forces (re)writing the golden
};

struct GoldenReport {
  int result;            // enum GoldenResult
  int diff_pixels;       // pixels exceeding per_channel
  int max_channel_diff;  // worst per-channel diff observed
  int first_x, first_y;  // first offending pixel (-1 if none)
};

// Compare `actual` (RGB8, width*height*3) against the golden at
// params->golden_path. Fills *report. Returns report->result for convenience.
int golden_check(const struct GoldenParams* params, const uint8_t* actual,
                 int width, int height, struct GoldenReport* report);

#endif  // HARNESS_GOLDEN_H
