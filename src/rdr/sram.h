// sram.h — host-neutral shim for __not_in_flash_func.
//
// On device (PICO_ON_DEVICE), includes pico/platform.h which provides the real
// __not_in_flash_func(name) attribute: places the decorated function into the
// .time_critical RAM section, so the hot raster fill loop runs from SRAM
// instead of stalling on XIP flash fetches (the RP2040 XIP code-cache is only
// 16KB; inner-loop cache misses are the #1 fill bottleneck per T5 profiling).
//
// On host (Linux/SDL CI build), pico/platform.h is absent; define a no-op so
// every decorated source file compiles and runs identically — placement-only,
// zero behavioural change, bit-identical golden output.
//
// Usage: #include "rdr/sram.h" in any .cc that decorates a definition, then
// wrap the function name:
//   uint16_t __not_in_flash_func(my_hot_fn)(int arg) { ... }
// Orthodox C++: C headers only, no namespaces.
#ifndef RDR_SRAM_H
#define RDR_SRAM_H

#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
#include "pico/platform.h"
#else
#ifndef __not_in_flash_func
#define __not_in_flash_func(x) x
#endif
#endif

#endif  // RDR_SRAM_H
