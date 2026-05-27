# tests/harness — host test harness for Stream B.1-epsilon.
#
# Three parts:
#   1. Float oracle (oracle.{h,cc})       — slow float reference math.
#   2. Golden-image framework (golden.*,  — render-to-buffer + PNG compare.
#      png_io.*)
#   3. ImGui frame inspector (inspector.*)— SDL3/ImGui window (build-only in CI).
#
# ORTHODOXY CARVE-OUT: this directory is NOT orthodoxy_enforced. It interfaces
# ImGui/SDL3 (inspector) and is the float oracle, both of which intentionally
# sit outside the Orthodox C++ module discipline (see CLAUDE.md "Pragmatism").
# No orthodoxy_enforce()/pico_tidy_enable() calls here, by design. The module
# libs in src/*/ remain enforced.

# ---- harness support library (oracle + golden + png; no ImGui) -------------
add_library(harness_core STATIC
    oracle.cc
    golden.cc
    png_io.cc
)
add_library(renderer::harness ALIAS harness_core)
target_include_directories(harness_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src        # rdr/types.h, rdr/config.h, gfx/framebuffer.h
)
# Where committed golden PNGs live (regression anchors).
target_compile_definitions(harness_core PUBLIC
    HARNESS_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")
target_link_libraries(harness_core PRIVATE project_warnings)

# ---- harness self-tests ----------------------------------------------------
add_executable(harness_test
    png_io_test.cc
    oracle_test.cc
    golden_test.cc
)
target_link_libraries(harness_test PRIVATE renderer::harness gtest_main)
gtest_discover_tests(harness_test PROPERTIES LABELS harness)

# ---- ImGui frame inspector (build/link target; manual run) -----------------
# Links SDL3 + ImGui (the carve-out). In headless CI we build+link, not run.
add_executable(harness_inspector
    inspector.cc
    inspector_main.cc
)
target_include_directories(harness_inspector PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src        # gfx/framebuffer.h
)
target_link_libraries(harness_inspector PRIVATE SDL3::SDL3 imgui)
