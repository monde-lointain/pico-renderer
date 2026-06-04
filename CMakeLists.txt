cmake_minimum_required(VERSION 3.28)

# ---- Platform selection --------------------------------------------------
set(PLATFORM "sdl" CACHE STRING "Target platform: sdl | picosystem")
set_property(CACHE PLATFORM PROPERTY STRINGS sdl picosystem)

if(PLATFORM STREQUAL "picosystem")
  # pico-sdk must be imported BEFORE project(). PICO_SDK_PATH is resolved by
  # pico_sdk_import.cmake from the cache, environment, or the
  # PICO_SDK_FETCH_FROM_GIT fallback (it errors if none are set) — no
  # machine-specific local default baked in here.
  set(PICO_BOARD pimoroni_picosystem CACHE STRING "")
  include(cmake/pico_sdk_import.cmake)
  project(renderer VERSION 0.1.0 LANGUAGES C CXX ASM)
  pico_sdk_init()
else()
  project(renderer VERSION 0.1.0 LANGUAGES C CXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BUILD_TESTS "Build host unit tests" ${PROJECT_IS_TOP_LEVEL})

# Tests + Orthodoxy only make sense on the host (sdl) build.
if(PLATFORM STREQUAL "picosystem")
  set(BUILD_TESTS OFF)
endif()

# Enable testing EARLY (before cmake/Orthodoxy.cmake), so the Orthodoxy canary's
# add_test() — registered while including that module — lands in a generated
# CTestTestfile. enable_testing() only affects the current dir and below from the
# point it is called, so a late call would silently drop the canary. PRE_TEST is
# set here for the same reason (it governs the later gtest_discover_tests too).
if(BUILD_TESTS AND PLATFORM STREQUAL "sdl")
  set(CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE PRE_TEST)
  enable_testing()
endif()

# ---- Warning flags (our targets only; never third-party / SDK) -----------
add_library(project_warnings INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(project_warnings INTERFACE -Wall -Wextra -Wpedantic)
endif()

# ---- Orthodox C++ enforcement (clang host only, scoped, optional) --------
include(cmake/Orthodoxy.cmake)

# ---- Sanitizers (ASan/UBSan, opt-in via RENDERER_SANITIZE) -----
# Defines the always-present renderer::sanitizers interface target
# (no-op when off). Unconditional (NOT inside the sdl block) because game/gfx/app
# link it PUBLIC on BOTH platforms — the target must exist for the pico build too.
# Linked into project_warnings so the exe inherits it; the libs link it PUBLIC in
# their own CMakeLists so the test binaries are instrumented.
include(cmake/Sanitizers.cmake)
target_link_libraries(project_warnings INTERFACE renderer::sanitizers)

# ---- Profiler (granular intra-frame timers, opt-in via RENDERER_PROFILER) ----
# Defines the always-present renderer::prof_config interface (PROFILER=1 only
# when ON). Unconditional (both platforms): OFF by default so golden/CI builds
# stay bit-identical; the pico-prof preset flips it ON for on-target probing.
include(cmake/Profiler.cmake)

# ---- Pico inline clang-tidy (arm-none-eabi, opt-in via RENDERER_PICO_TIDY) ----
# Defines pico_tidy_enable(); the subsystem libs call it on BOTH platforms (no-op on
# host / normal pico builds), so the include is unconditional — outside the sdl/pico
# branches below.
include(cmake/PicoTidy.cmake)

# ---- Host dependencies (SDL3 + ImGui + GoogleTest via FetchContent) ------
if(PLATFORM STREQUAL "sdl")
  include(cmake/Dependencies.cmake)
  # clang-tidy target needs the host compile DB; host-only.
  include(cmake/StaticAnalysis.cmake)
endif()

add_subdirectory(src)

# Unit tests (enable_testing()/PRE_TEST already set above so the Orthodoxy canary
# registered during the cmake/Orthodoxy.cmake include).
if(BUILD_TESTS AND PLATFORM STREQUAL "sdl")
  add_subdirectory(tests)
endif()
