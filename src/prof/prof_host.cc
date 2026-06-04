// prof_host.cc — HOST timer + core primitives. Compiled only on the sdl build
// (link-time substituted, like sched/dispatch_host.cc). The host has no
// pico-sdk and no SysTick, so BOTH flavors use CLOCK_MONOTONIC nanoseconds (the
// "fine" path is identical to "coarse" here — host validates the
// merge/format/algorithm only, never device perf realism). Single-threaded:
// prof_core() is always 0, PROF_NUM_CORES is 1, so the cross-core merge
// degenerates to one row.
#include "prof/prof.h"

#if PROFILER

#include <time.h>

uint64_t prof_coarse_read(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

uint64_t prof_coarse_elapsed_ns(uint64_t start) {
  return prof_coarse_read() - start;
}

uint64_t prof_fine_read(void) { return prof_coarse_read(); }

uint64_t prof_fine_elapsed_ns(uint64_t start, int* overflowed) {
  *overflowed = 0;  // monotonic ns never "wraps" in any run we care about
  return prof_coarse_read() - start;
}

uint32_t prof_core(void) { return 0; }

void prof_init(void) {}
void prof_systick_enable(void) {}

#endif  // PROFILER
