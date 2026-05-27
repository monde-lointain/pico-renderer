#ifndef PLATFORM_DISPLAY_H
#define PLATFORM_DISPLAY_H

#include <stdint.h>

/* display.h — display-fork hooks beyond the frozen platform.h HAL (B.1-theta).
 *
 * The frozen platform.h contract (plat_present) presents a finished frame. For
 * the scanline-race optimization the renderer/scheduler can additionally
 * publish a running watermark of fully-committed scanline rows while a frame is
 * still being rasterized; the present DMA chases that watermark and never
 * streams past it (stall-rather-than-tear). This is an OPTIONAL fast path: a
 * frame presented via plat_present() alone (watermark left at 0) still renders
 * correctly by streaming the whole frame after TE.
 *
 * Lives in the platform lane (not the frozen platform.h) so it can evolve
 * without a contract-delta. Host SDL builds provide inert stubs.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Publish the count of scanline rows committed so far this frame (monotonic,
 * 0 .. SCREEN_H). Called by the scheduler as each tile-row band resolves; the
 * present DMA may stream up to but not past this row. Reset implicitly at the
 * start of the next plat_present(). */
void plat_present_watermark(uint16_t committed_rows);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_DISPLAY_H */
