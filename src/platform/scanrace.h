#ifndef PLATFORM_SCANRACE_H
#define PLATFORM_SCANRACE_H

#include <stdint.h>

/* scanrace — scanline-race watermark/guard for the present DMA (B.1-theta).
 *
 * The renderer commits the framebuffer top-to-bottom in tile-row bands. The
 * display DMA streams scanlines top-to-bottom in panel order over the same
 * frame. To avoid tearing/streaming-uncommitted-pixels, the DMA "chases" a
 * watermark: the last fully-committed scanline row. The DMA may stream up to
 * (and including) the watermark row, but never past it — if it catches up it
 * stalls (the platform waits for more rows to commit) rather than crossing.
 *
 * This module is pure integer logic (no hardware), so the chase arithmetic is
 * host-unit-testable. platform_pico.cc drives a real DMA from these answers;
 * platform_sdl.cc may use it to model the same band ordering for golden tests.
 *
 * Units are scanline rows (0 .. rows_total). A "band" is a contiguous run of
 * rows committed together (one tile-row, RDR_TILE_H rows on device).
 *
 * Orthodox C++: POD struct + free scanrace_* functions over a receiver pointer.
 */

struct ScanRace {
  uint16_t rows_total;     /* total scanlines in the frame (SCREEN_H) */
  uint16_t watermark_rows; /* committed rows: DMA may stream [0, watermark) */
  uint16_t dma_rows_sent;  /* rows already streamed this frame */
};

/* Reset for a new frame: nothing committed, nothing sent. rows_total is the
 * frame height (e.g. SCREEN_H). */
void scanrace_init(struct ScanRace *r, uint16_t rows_total);

/* Mark one more committed band of `band_rows` scanlines (advances watermark).
 * Clamps at rows_total. Bands are expected in top-to-bottom order. */
void scanrace_commit_band(struct ScanRace *r, uint16_t band_rows);

/* Set the watermark directly to an absolute committed-row count (clamped to
 * rows_total). Idempotent; never moves the watermark backwards. */
void scanrace_set_watermark(struct ScanRace *r, uint16_t committed_rows);

/* How many rows the DMA may stream RIGHT NOW without crossing the watermark:
 * watermark_rows - dma_rows_sent (0 if caught up / stalled). */
uint16_t scanrace_runway(const struct ScanRace *r);

/* True if the DMA has caught up to the watermark but the frame is not yet
 * fully committed — i.e. it must STALL (wait), not stream, to avoid tearing. */
bool scanrace_stalled(const struct ScanRace *r);

/* True once every row has been streamed (dma_rows_sent == rows_total). */
bool scanrace_done(const struct ScanRace *r);

/* Account for `rows` scanlines just streamed by the DMA. Caller must pass
 * rows <= scanrace_runway(r); rows beyond the runway are clamped (and would
 * indicate a guard violation upstream). Returns rows actually accepted. */
uint16_t scanrace_advance_dma(struct ScanRace *r, uint16_t rows);

#endif /* PLATFORM_SCANRACE_H */
