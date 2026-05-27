/* scanrace.cc — scanline-race watermark/guard implementation (B.1-theta).
 *
 * Pure integer logic; see scanrace.h for the contract. No hardware deps, so it
 * compiles and unit-tests on host and links into the pico HAL unchanged. */

#include "platform/scanrace.h"

void scanrace_init(struct ScanRace* r, uint16_t rows_total) {
  r->rows_total = rows_total;
  r->watermark_rows = 0;
  r->dma_rows_sent = 0;
}

void scanrace_commit_band(struct ScanRace* r, uint16_t band_rows) {
  uint32_t next = (uint32_t)r->watermark_rows + (uint32_t)band_rows;
  if (next > r->rows_total) {
    next = r->rows_total;
  }
  r->watermark_rows = (uint16_t)next;
}

void scanrace_set_watermark(struct ScanRace* r, uint16_t committed_rows) {
  uint16_t v = committed_rows;
  if (v > r->rows_total) {
    v = r->rows_total;
  }
  /* Monotonic: never retreat the watermark. */
  if (v > r->watermark_rows) {
    r->watermark_rows = v;
  }
}

uint16_t scanrace_runway(const struct ScanRace* r) {
  if (r->watermark_rows <= r->dma_rows_sent) {
    return 0;
  }
  return (uint16_t)(r->watermark_rows - r->dma_rows_sent);
}

bool scanrace_stalled(const struct ScanRace* r) {
  /* Caught up to the watermark, but the frame is not yet fully streamed. */
  return scanrace_runway(r) == 0 && r->dma_rows_sent < r->rows_total;
}

bool scanrace_done(const struct ScanRace* r) {
  return r->dma_rows_sent >= r->rows_total;
}

uint16_t scanrace_advance_dma(struct ScanRace* r, uint16_t rows) {
  uint16_t const runway = scanrace_runway(r);
  uint16_t const accepted =
      rows < runway ? rows : runway; /* clamp at watermark */
  r->dma_rows_sent = (uint16_t)(r->dma_rows_sent + accepted);
  return accepted;
}
