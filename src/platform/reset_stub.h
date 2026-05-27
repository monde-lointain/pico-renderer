#ifndef PLATFORM_RESET_STUB_H
#define PLATFORM_RESET_STUB_H

#include <stdint.h>

/* reset_stub — firmware reset stub + watchdog (B.1-theta, lane-boundary call).
 *
 * Consumed by stream eta's on-target runner to drive picotool-style automated
 * test cycles: arm a watchdog so a hung firmware self-recovers, kick it from
 * the frame loop, and reboot into USB bootsel (BOOTSEL) on demand so the host
 * can flash the next UF2 without a physical button press.
 *
 * On the pico target these wrap pico-sdk hardware_watchdog + pico_bootrom
 * (reset_usb_boot). On host they are inert stubs so the same call sites build
 * and link in host tests/runners.
 *
 * Orthodox C: free functions, no state exposed.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Arm the hardware watchdog: if plat_watchdog_kick() is not called within
 * `timeout_ms`, the chip reboots (recovering a hung firmware). On RP2040 the
 * max is ~8388 ms; larger values are clamped by the HAL. timeout_ms == 0
 * disables the watchdog. Host: no-op. */
void plat_watchdog_arm(uint32_t timeout_ms);

/* Feed the watchdog (call once per frame). Host: no-op. */
void plat_watchdog_kick(void);

/* True if the most recent boot was caused by a watchdog timeout (vs power-on /
 * USB flash). Lets the runner detect and report a firmware hang. Host: false.
 */
bool plat_watchdog_caused_reboot(void);

/* Reboot into USB BOOTSEL so the host (picotool) can flash a new image.
 * Does not return on the pico target. Host: no-op (returns). */
void plat_reset_to_bootsel(void);

/* Reboot back into the flashed application (warm reset via watchdog, 0 delay).
 * Does not return on the pico target. Host: no-op (returns). */
void plat_reset_to_app(void);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_RESET_STUB_H */
