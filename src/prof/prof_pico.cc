// prof_pico.cc — DEVICE timer + core primitives (RP2040). Compiled only on the
// picosystem build (link-time substituted, like sched/dispatch_pico.cc). The
// hot readers are __not_in_flash_func: the renderer hot path is ENTIRELY
// flash-resident and the XIP code-cache is only 16KB, so a flash-resident timer
// read would XIP-fetch on every block enter/exit and evict the very texture /
// code lines it is measuring (worst for the per-tile FINE sweeps). The pico-sdk
// hardware idioms are non-Orthodox by nature; the surface is kept narrow.
#include "prof/prof.h"

#if PROFILER

#include "hardware/clocks.h"
#include "hardware/structs/systick.h"
#include "pico/platform.h"  // __not_in_flash_func, get_core_num
#include "pico/time.h"      // time_us_64

// SysTick CSR bits.
enum {
  SYSTICK_CSR_ENABLE = 1U << 0,
  SYSTICK_CSR_TICKINT = 1U << 1,
  SYSTICK_CSR_CLKSOURCE = 1U << 2,  // 1 = processor clock (clk_sys, 250MHz)
  SYSTICK_CSR_COUNTFLAG = 1U << 16  // set when the down-counter reached 0
};
enum { SYSTICK_MASK = 0x00FFFFFFU };  // 24-bit counter

// clk_sys Hz, cached AFTER plat_init's 250MHz overclock (else ns is wrong).
static uint32_t s_clk_hz = 0;

void prof_init(void) { s_clk_hz = clock_get_hz(clk_sys); }  // returns uint32_t

// Enable SysTick on the CALLING core (it is per-core BANKED): RVR = full 24-bit
// span, CVR = 0 (also clears COUNTFLAG), processor-clock source, no interrupt.
void prof_systick_enable(void) {
  systick_hw->rvr = SYSTICK_MASK;
  systick_hw->cvr = 0;
  systick_hw->csr = SYSTICK_CSR_ENABLE | SYSTICK_CSR_CLKSOURCE;
}

uint32_t __not_in_flash_func(prof_core)(void) {
  return (uint32_t)get_core_num();
}

uint64_t __not_in_flash_func(prof_coarse_read)(void) {
  return time_us_64() * 1000ULL;  // us -> ns (64-bit, never wraps, cross-core)
}

uint64_t __not_in_flash_func(prof_coarse_elapsed_ns)(uint64_t start) {
  return prof_coarse_read() - start;
}

uint64_t __not_in_flash_func(prof_fine_read)(void) {
  // RESET the counter to the top of its span at block start: writing CVR zeroes
  // the count AND clears COUNTFLAG, and the next cycle reloads RVR (0xFFFFFF).
  // This is what makes COUNTFLAG a TRUE >67ms detector: starting from the full
  // span, the counter reaches 0 (sets COUNTFLAG) ONLY if the block ran the
  // whole 24-bit span (~67ms). Without the reset, a free-running counter trips
  // COUNTFLAG whenever a block merely STRADDLES zero — a false positive even
  // for a 1us block. Fine blocks never nest here (sweep_xlu/aa_resolve are
  // siblings under the COARSE raster_tile), so resetting per block is safe.
  systick_hw->cvr = 0;
  return (
      uint64_t)SYSTICK_MASK;  // nominal start = top of the span (post-reload)
}

uint64_t __not_in_flash_func(prof_fine_elapsed_ns)(uint64_t start,
                                                   int* overflowed) {
  uint32_t const now = systick_hw->cvr & SYSTICK_MASK;
  // With the per-block CVR reset (prof_fine_read), COUNTFLAG set => the counter
  // ran the FULL span to 0 => a true >67ms overrun: the delta is ambiguous.
  // Tripwire only — flagged (wrap=1), not reconstructed. Coarse-time the anchor
  // instead if this ever fires (as sweep_opaque does).
  if ((systick_hw->csr & SYSTICK_CSR_COUNTFLAG) != 0U) {
    *overflowed = 1;
  }
  // DOWN counter from `start` (== full span): ticks elapsed = (start - now).
  uint32_t const dticks = ((uint32_t)start - now) & SYSTICK_MASK;
  if (s_clk_hz == 0) {
    return 0;  // prof_init not run yet (should not happen post-plat_init)
  }
  return (uint64_t)dticks * 1000000000ULL / (uint64_t)s_clk_hz;
}

#endif  // PROFILER
