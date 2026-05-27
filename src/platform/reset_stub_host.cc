/* reset_stub_host.cc — host stubs for the reset/watchdog interface (B.1-theta).
 *
 * Inert no-ops so the same call sites (stream eta's runner, demo loop) build
 * and link on host. The real watchdog/BOOTSEL behaviour lives in
 * reset_stub_pico.cc.
 */

#include "platform/reset_stub.h"

void plat_watchdog_arm(uint32_t timeout_ms) { (void)timeout_ms; }

void plat_watchdog_kick(void) {}

bool plat_watchdog_caused_reboot(void) { return false; }

void plat_reset_to_bootsel(void) {}

void plat_reset_to_app(void) {}
