# Inline clang-tidy for the pico (arm-none-eabi-g++) build.
#
# The host `tidy` target (cmake/StaticAnalysis.cmake) runs over the host compile
# DB and EXCLUDES *_pico.cc, so platform_pico.cc / main_pico.cc are never linted
# and shared code is only ever analyzed with SDL/host macros active. This module
# lints all our sources AS COMPILED FOR THE PICO (RP2040 macros, pico-only paths),
# via clang-tidy run inline by CMake (CMAKE_<LANG>_CLANG_TIDY) on our targets only.
#
# clang-tidy is a clang frontend; the compile DB is arm-none-eabi-g++. The cited
# recipe (https://discourse.cmake.org/t/configure-clang-tidy-when-using-an-arm-none-eabi-gcc-g-toolchain)
# bridges that: set clang's target triple, feed it the cross sysroot includes, but
# SKIP gcc's own builtin-header dirs (clang ships its own resource headers).
#
# OPT-IN: RENDERER_PICO_TIDY=OFF by default, so normal
# `make build PRESET=pico` / .uf2 generation is untouched. Mirrors
# cmake/Orthodoxy.cmake's active-flag + per-target enable-function shape.

include_guard(GLOBAL)

option(RENDERER_PICO_TIDY
  "Run clang-tidy inline on the pico build (arm-none-eabi)"
  OFF)

# Override-only. clang-tidy normally locates its own resource (builtin) headers;
# auto-deriving from a clang++-N risks pairing a mismatched version. Set this only
# if verification shows clang-tidy cannot self-locate its builtins.
set(RENDERER_PICO_TIDY_RESOURCE_DIR "" CACHE STRING
  "Override clang resource dir for pico clang-tidy (-resource-dir); empty = self-locate")

set(RENDERER_PICO_TIDY_ACTIVE OFF)

if(RENDERER_PICO_TIDY AND PLATFORM STREQUAL "picosystem")
  # Prefer the versioned binary that matches CI's toolchain; fall back to plain.
  find_program(RENDERER_CLANG_TIDY_EXE NAMES clang-tidy-22 clang-tidy)
  if(NOT RENDERER_CLANG_TIDY_EXE)
    # The user explicitly asked for the lint to run; a silent no-op would hide
    # that it never happened. Fail loudly (cf. RENDERER_REQUIRE_ORTHODOXY).
    message(FATAL_ERROR
      "RENDERER_PICO_TIDY=ON but no clang-tidy found (tried clang-tidy-22, "
      "clang-tidy). Install clang-tidy or set RENDERER_PICO_TIDY=OFF.")
  endif()

  # Base command + the warnings-as-errors gate (matches the host `tidy` target).
  # --allow-no-checks: pico-sdk injects new_delete.cpp (its only C++ source) into
  # consumers via pico_cxx_options INTERFACE sources; it sits outside our tree so
  # clang-tidy finds no .clang-tidy for it. Without this flag that "no checks
  # enabled" case is a hard error; with it, such non-project sources lint to a
  # clean no-op while our own sources still resolve our .clang-tidy normally.
  set(_pico_tidy_cmd
    "${RENDERER_CLANG_TIDY_EXE}"
    "--warnings-as-errors=*"
    "--allow-no-checks")

  # Target triple: arm-none-eabi is the SDK's PICO_DEFAULT_GCC_TRIPLE
  # (pico_arm_cortex_m0plus_gcc.cmake). The -mcpu=cortex-m0plus -mthumb already in
  # the gcc compile DB (forwarded by CMake after `--`) refine the arch for clang.
  list(APPEND _pico_tidy_cmd "--extra-arg=--target=arm-none-eabi")

  # Per-library SDK targets add -Wno-<gcc-warning-name> flags clang doesn't know;
  # tolerate them rather than tripping clang-diagnostic under --warnings-as-errors.
  list(APPEND _pico_tidy_cmd "--extra-arg=-Wno-unknown-warning-option")

  # Feed clang-tidy the cross sysroot includes, but SKIP gcc's own builtin-header
  # dirs (.../lib/gcc/...): clang provides its own resource headers for intrinsics,
  # and mixing them causes builtin/intrinsic conflicts. newlib + libstdc++ live
  # under <triple>/include (not lib/gcc), so they survive the filter.
  set(_pico_tidy_includes
    ${CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES}
    ${CMAKE_C_IMPLICIT_INCLUDE_DIRECTORIES})
  list(REMOVE_DUPLICATES _pico_tidy_includes)
  foreach(_inc IN LISTS _pico_tidy_includes)
    if(_inc MATCHES "lib/gcc")
      continue()
    endif()
    list(APPEND _pico_tidy_cmd "--extra-arg=-isystem${_inc}")
  endforeach()

  if(RENDERER_PICO_TIDY_RESOURCE_DIR)
    list(APPEND _pico_tidy_cmd
      "--extra-arg=-resource-dir=${RENDERER_PICO_TIDY_RESOURCE_DIR}")
  endif()

  set(RENDERER_PICO_TIDY_CMD "${_pico_tidy_cmd}")
  set(RENDERER_PICO_TIDY_ACTIVE ON)
  message(STATUS
    "Pico clang-tidy enabled (${RENDERER_CLANG_TIDY_EXE}): inline on "
    "src/{game,gfx,app,platform_pico} + executable")
endif()

# pico_tidy_enable(<target>): attach the inline clang-tidy command to a target's
# C++ compiles. No-op unless RENDERER_PICO_TIDY is active, so host builds
# and normal pico/.uf2 builds call it harmlessly. (set_target_properties is the only
# way to set CXX_CLANG_TIDY — there is no target_*() command for it.)
function(pico_tidy_enable target)
  if(RENDERER_PICO_TIDY_ACTIVE)
    set_target_properties(${target} PROPERTIES
      CXX_CLANG_TIDY "${RENDERER_PICO_TIDY_CMD}")
  endif()
endfunction()
