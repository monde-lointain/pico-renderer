# Orthodox C++ enforcement via the Orthodoxy clang plugin.
# OPTIONAL: only engages on a clang host build with the plugin installed and a
# matching major version. A plain gcc build (or missing plugin) silently skips,
# so the project still builds. Apply orthodoxy_enforce(<target>) to the
# platform-independent libraries (game/gfx/app) only — NOT tests, platform_sdl,
# or any third-party target.

# RENDERER_REQUIRE_ORTHODOXY: enforcement-active assertion. Default
# OFF so a normal local build (no plugin installed) configures cleanly. CI pins
# it ON for the Linux/clang job so a silently-inactive plugin FAILS configure
# loudly instead of letting unenforced code slip through unnoticed.
option(RENDERER_REQUIRE_ORTHODOXY
  "Fail configure if the Orthodoxy plugin is not active (CI enforcement canary)"
  OFF)

set(ORTHODOXY_ACTIVE OFF)

if(PLATFORM STREQUAL "sdl" AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  find_package(orthodoxy CONFIG QUIET OPTIONAL_COMPONENTS plugin)
  if(orthodoxy_plugin_FOUND OR TARGET orthodoxy::plugin)
    set(ORTHODOXY_ACTIVE ON)
    message(STATUS "Orthodoxy plugin found: enforcement enabled on src/{game,gfx,app}")
  else()
    message(STATUS "Orthodoxy plugin not found: enforcement skipped (build still valid)")
  endif()
endif()

# Enforcement-active assertion. When RENDERER_REQUIRE_ORTHODOXY is ON
# we MUST have a live plugin; otherwise the "enforcement" is a silent no-op.
if(RENDERER_REQUIRE_ORTHODOXY AND NOT ORTHODOXY_ACTIVE)
  message(FATAL_ERROR
    "RENDERER_REQUIRE_ORTHODOXY=ON but the Orthodoxy plugin is not active.\n"
    "Requirements: PLATFORM=sdl, a Clang C++ compiler (got "
    "'${CMAKE_CXX_COMPILER_ID}'), and the orthodoxy plugin installed where "
    "find_package(orthodoxy) can locate it (see docs/setup-llvm.md). "
    "Enforcement would otherwise be a silent no-op.")
endif()

function(orthodoxy_enforce target)
  if(ORTHODOXY_ACTIVE)
    target_link_libraries(${target} PRIVATE orthodoxy::plugin)
  endif()
endfunction()

# Enforcement-active canary: a target containing a DELIBERATE Orthodoxy
# violation that MUST fail to compile when the plugin is live. CI builds it
# expecting failure ("this should not compile"). Only engaged when the plugin
# is active, so local non-plugin builds skip it and configure/build cleanly.
# EXISTS-guarded so the project configures even without the canary dir.
if(ORTHODOXY_ACTIVE
   AND EXISTS ${CMAKE_CURRENT_LIST_DIR}/../tests/orthodoxy_canary/CMakeLists.txt)
  add_subdirectory(
    ${CMAKE_CURRENT_LIST_DIR}/../tests/orthodoxy_canary
    ${PROJECT_BINARY_DIR}/tests/orthodoxy_canary)
endif()
