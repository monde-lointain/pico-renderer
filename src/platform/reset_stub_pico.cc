/* reset_stub_pico.cc — firmware reset stub + watchdog, pico target (B.1-theta).
 *
 * Wraps pico-sdk hardware_watchdog + pico_bootrom for stream eta's on-target
 * runner: arm/kick a watchdog so a hung firmware self-recovers, and reboot into
 * USB BOOTSEL (or back into the app) on demand for picotool-driven flashing.
 *
 * References:
 *   - hardware/watchdog.h: watchdog_enable / watchdog_update /
 *     watchdog_caused_reboot / watchdog_reboot (pico-sdk).
 *   - pico/bootrom.h reset_usb_boot() (pico-sdk, ROM BOOTSEL entry).
 *   - reset_interface.c: watchdog_reboot(0,0,delay) = reboot to flash.
 */

#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/stdio.h" /* stdio_flush — drain telemetry before a reboot */
#include "pico/time.h"  /* sleep_ms — let the stdio_usb timer pump tud_task */
#include "platform/reset_stub.h"

/* RP2040 watchdog max load is ~8388 ms (RP2040-E1; see watchdog.h). */
enum { WATCHDOG_MAX_MS = 8388 };

void plat_watchdog_arm(uint32_t timeout_ms) {
  if (timeout_ms == 0) {
    watchdog_disable();
    return;
  }
  if (timeout_ms > WATCHDOG_MAX_MS) {
    timeout_ms = WATCHDOG_MAX_MS;
  }
  /* pause_on_debug=true: don't trip the dog while halted at a breakpoint. */
  watchdog_enable(timeout_ms, true);
}

void plat_watchdog_kick(void) { watchdog_update(); }

bool plat_watchdog_caused_reboot(void) { return watchdog_caused_reboot(); }

void plat_reset_to_bootsel(void) {
  /* Drain buffered stdout (e.g. the final RESULT=PASS sentinel the on-target
   * runner waits for) BEFORE rebooting: main returns right after logging it, so
   * without a final pump the USB-CDC TX FIFO never flushes and the last line is
   * lost (observed on the bounded demo exit). stdio_flush pushes it into the CDC
   * driver; the sleep lets the stdio_usb background timer run tud_task to
   * transmit it before the ROM reboot tears USB down. */
  stdio_flush();
  sleep_ms(100);
  /* Reboot to ROM USB boot, both interfaces enabled, no activity LED. */
  reset_usb_boot(0, 0);
}

void plat_reset_to_app(void) {
  /* Warm reset into the flashed application: watchdog reboot, pc/sp 0 = normal
   * boot, 0 ms delay = immediate (reset_interface.c uses the same idiom). */
  watchdog_reboot(0, 0, 0);
  while (true) { /* unreachable: wait for the reboot to take effect */
  }
}
