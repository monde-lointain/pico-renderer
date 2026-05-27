/* scanrace_test.cc — host unit tests for the scanline-race DMA watermark guard.
 *
 * Pure integer logic; verifies the DMA never crosses the committed watermark
 * (stall-rather-than-tear) and that band commits advance the runway correctly.
 */

#include "platform/scanrace.h"

#include <gtest/gtest.h>

#include "gfx/framebuffer.h"

TEST(ScanRace, InitIsEmpty) {
  struct ScanRace r;
  scanrace_init(&r, SCREEN_H);
  EXPECT_EQ(r.rows_total, SCREEN_H);
  EXPECT_EQ(r.watermark_rows, 0);
  EXPECT_EQ(r.dma_rows_sent, 0);
  EXPECT_EQ(scanrace_runway(&r), 0);
  EXPECT_FALSE(scanrace_done(&r));
  /* Nothing committed yet, frame not done -> the DMA must stall. */
  EXPECT_TRUE(scanrace_stalled(&r));
}

TEST(ScanRace, CommitBandAdvancesWatermark) {
  struct ScanRace r;
  scanrace_init(&r, 240);
  scanrace_commit_band(&r, 60);
  EXPECT_EQ(r.watermark_rows, 60);
  EXPECT_EQ(scanrace_runway(&r), 60);
  EXPECT_FALSE(scanrace_stalled(&r));
}

TEST(ScanRace, CommitBandClampsAtTotal) {
  struct ScanRace r;
  scanrace_init(&r, 240);
  scanrace_commit_band(&r, 200);
  scanrace_commit_band(&r, 200); /* would overshoot -> clamp */
  EXPECT_EQ(r.watermark_rows, 240);
}

TEST(ScanRace, DmaNeverCrossesWatermark) {
  struct ScanRace r;
  scanrace_init(&r, 240);
  scanrace_commit_band(&r, 60); /* watermark = 60 */

  /* DMA asks for the whole frame; runway must cap it at the watermark. */
  EXPECT_EQ(scanrace_runway(&r), 60);
  uint16_t sent = scanrace_advance_dma(&r, 240);
  EXPECT_EQ(sent, 60); /* clamped to watermark — never crossed */
  EXPECT_EQ(r.dma_rows_sent, 60);

  /* Caught up to the watermark, frame not done -> stalled. */
  EXPECT_EQ(scanrace_runway(&r), 0);
  EXPECT_TRUE(scanrace_stalled(&r));
  EXPECT_FALSE(scanrace_done(&r));
}

TEST(ScanRace, ChaseUnblocksOnNextBand) {
  struct ScanRace r;
  scanrace_init(&r, 240);
  scanrace_commit_band(&r, 60);
  scanrace_advance_dma(&r, 60);
  EXPECT_TRUE(scanrace_stalled(&r));

  /* Next band commits -> runway opens again, no longer stalled. */
  scanrace_commit_band(&r, 60); /* watermark = 120 */
  EXPECT_EQ(scanrace_runway(&r), 60);
  EXPECT_FALSE(scanrace_stalled(&r));
}

TEST(ScanRace, FullFrameCompletes) {
  struct ScanRace r;
  scanrace_init(&r, 240);
  for (int band = 0; band < 4; ++band) {
    scanrace_commit_band(&r, 60);
    scanrace_advance_dma(&r, 60);
  }
  EXPECT_EQ(r.dma_rows_sent, 240);
  EXPECT_TRUE(scanrace_done(&r));
  /* Done -> not stalled (no more rows to wait for). */
  EXPECT_FALSE(scanrace_stalled(&r));
  EXPECT_EQ(scanrace_runway(&r), 0);
}

TEST(ScanRace, SetWatermarkIsMonotonic) {
  struct ScanRace r;
  scanrace_init(&r, 240);
  scanrace_set_watermark(&r, 120);
  EXPECT_EQ(r.watermark_rows, 120);
  scanrace_set_watermark(&r, 60); /* never moves backwards */
  EXPECT_EQ(r.watermark_rows, 120);
  scanrace_set_watermark(&r, 999); /* clamps to total */
  EXPECT_EQ(r.watermark_rows, 240);
}
