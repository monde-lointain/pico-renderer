# Clang/GCC sanitizer support (ASan + UBSan), opt-in and target-scoped.
#
# Runtime complement to clang-tidy: catches UB the static checkers can't see
# (out-of-bounds, misaligned loads, signed overflow, bad shifts, use-after-scope)
# so latent host-only-defined UB can't silently miscompile on the arm device.
#
# OFF by default. Enable with -DRENDERER_SANITIZE=address,undefined on
# a host (sdl) clang/gcc build. The flags ride on an INTERFACE target linked
# PUBLIC into game / gfx / app / project_warnings, so every dependent exe and
# test is instrumented automatically.

# Comma/semicolon list of sanitizers, e.g. "address,undefined". Empty = off.
set(RENDERER_SANITIZE "" CACHE STRING
  "Sanitizers to enable on host builds (comma list: address,undefined,thread,memory). Empty = off.")

# -fno-sanitize-recover=all is a COMPILE-time flag: baked in, the binary aborts
# on the first finding, which makes a collect-all survey impossible. Keep it a
# separate toggle so the survey build omits it (recoverable; runtime
# halt_on_error=0 collects everything) and only the gate build sets it ON.
option(RENDERER_SANITIZE_HALT
  "Bake -fno-sanitize-recover=all so the first sanitizer finding aborts (gate build)"
  OFF)

# Always define the interface target so the PUBLIC links in game/gfx/app are
# valid even when sanitizers are off (then it carries no flags = no-op).
add_library(renderer_sanitizers INTERFACE)
add_library(renderer::sanitizers ALIAS renderer_sanitizers)

if(RENDERER_SANITIZE)
  # Host-only: arm-none-eabi (the picosystem build) has no compiler-rt runtime,
  # and game/gfx/app are dual-platform targets, so an un-guarded flag would break
  # the device link. Fail loudly rather than emit a broken arm build.
  if(NOT PLATFORM STREQUAL "sdl")
    message(FATAL_ERROR
      "RENDERER_SANITIZE='${RENDERER_SANITIZE}' requested "
      "but PLATFORM='${PLATFORM}'. Sanitizers are host-only (the arm-none-eabi "
      "device build has no compiler-rt runtime). Configure the sdl host preset "
      "instead.")
  endif()

  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR
      "RENDERER_SANITIZE requested but the compiler is "
      "'${CMAKE_CXX_COMPILER_ID}'; sanitizers need GCC or Clang.")
  endif()

  # Normalize commas to semicolons so it's a real CMake list, then reject
  # mutually incompatible runtimes (they share shadow memory).
  string(REPLACE "," ";" _pt_san_list "${RENDERER_SANITIZE}")
  if("address" IN_LIST _pt_san_list AND "thread" IN_LIST _pt_san_list)
    message(FATAL_ERROR "RENDERER_SANITIZE: 'address' and 'thread' are incompatible.")
  endif()
  if("memory" IN_LIST _pt_san_list AND
     ("address" IN_LIST _pt_san_list OR "thread" IN_LIST _pt_san_list))
    message(FATAL_ERROR "RENDERER_SANITIZE: 'memory' is incompatible with address/thread.")
  endif()

  string(REPLACE ";" "," _pt_san_flag "${_pt_san_list}")

  # Compile: instrument + frame pointers (readable traces) + debug line info.
  target_compile_options(renderer_sanitizers INTERFACE
    -fsanitize=${_pt_san_flag}
    -fno-omit-frame-pointer
    -g)

  # UBSan source-level ignorelist for confirmed-intentional sites. Created in
  # tools/; passed only when it exists so a fresh checkout still configures.
  if(EXISTS ${PROJECT_SOURCE_DIR}/tools/ubsan.ignorelist)
    target_compile_options(renderer_sanitizers INTERFACE
      -fsanitize-ignorelist=${PROJECT_SOURCE_DIR}/tools/ubsan.ignorelist)
  endif()

  # Gate build only: abort on the first finding (see RENDERER_SANITIZE_HALT above).
  if(RENDERER_SANITIZE_HALT)
    target_compile_options(renderer_sanitizers INTERFACE -fno-sanitize-recover=all)
  endif()

  # Link the runtime into every consumer.
  target_link_options(renderer_sanitizers INTERFACE -fsanitize=${_pt_san_flag})

  message(STATUS "Sanitizers enabled: ${_pt_san_flag} "
    "(halt-on-first=${RENDERER_SANITIZE_HALT})")
endif()
