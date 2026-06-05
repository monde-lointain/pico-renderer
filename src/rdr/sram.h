// sram.h — host-neutral shim for __not_in_flash_func.
//
// On the RP2040 ARM target (__arm__), defines __not_in_flash_func(name) as the
// GCC section attribute that places the decorated function into the
// .time_critical.<name> linker section, which the pico-sdk linker script maps
// to SRAM. This prevents the hot raster fill loop from stalling on XIP flash
// fetches (RP2040 XIP code-cache is only 16KB; inner-loop cache misses are the
// #1 fill bottleneck per T5 profiling).
//
// The attribute is defined inline here (matching the pico-sdk sections.h
// definition verbatim) to avoid dependency on pico/platform/sections.h, which
// requires the pico_platform CMake target's include path — not available to
// all consuming modules (tex/shade/blend do not link pico_platform).
// pico/platform.h is also excluded because it requires _PICO_H first.
//
// If the pico SDK has already defined __not_in_flash_func (e.g. when a TU does
// include pico/platform.h via pico.h), the #ifndef guard is a no-op.
//
// On host (Linux/SDL CI, x86-64), the no-op ensures every decorated source
// file compiles and runs identically — placement-only, zero behavioural
// change, bit-identical golden output.
//
// Usage: #include "rdr/sram.h" in any .cc that decorates a definition, then
// wrap the function name:
//   uint16_t __not_in_flash_func(my_hot_fn)(int arg) { ... }
// Orthodox C++: C headers only, no namespaces.
#ifndef RDR_SRAM_H
#define RDR_SRAM_H

// These macros DELIBERATELY mirror the pico-sdk's exact public names
// __not_in_flash[_func] (sections.h) so the SAME source compiles on host and
// device; the SDK itself defines them with these reserved/lower-case names.
// clang-tidy's reserved-identifier / macro-naming / macro-usage checks are
// therefore suppressed for this sanctioned shim block (renaming would defeat
// the purpose — the names must match the SDK's).
// NOLINTBEGIN(bugprone-reserved-identifier,readability-identifier-naming,cppcoreguidelines-macro-usage)
#ifdef __arm__
// Equivalent to the pico-sdk sections.h definition; uses standard C token-
// pasting instead of __STRING so no cdefs.h dependency is needed.
#ifndef __not_in_flash
#define __not_in_flash(group) __attribute__((section(".time_critical." group)))
#endif
#ifndef __not_in_flash_func
#define __not_in_flash_func(func_name) \
  __attribute__((section(".time_critical." #func_name))) func_name
#endif
#else
#ifndef __not_in_flash
#define __not_in_flash(group)
#endif
#ifndef __not_in_flash_func
#define __not_in_flash_func(x) x
#endif
#endif
// NOLINTEND(bugprone-reserved-identifier,readability-identifier-naming,cppcoreguidelines-macro-usage)

#endif  // RDR_SRAM_H
