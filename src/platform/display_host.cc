/* display_host.cc — host stub for the display-fork watermark hook (B.1-theta).
 *
 * The SDL presenter uploads a whole finished framebuffer at once, so the
 * scanline-race watermark is a no-op on host: call sites link, the fast path is
 * simply inert. The real chase logic lives in platform_pico.cc. */

#include "platform/display.h"

void plat_present_watermark(uint16_t committed_rows) { (void)committed_rows; }
