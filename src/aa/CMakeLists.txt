add_library(Renderer_aa)
add_library(Renderer::Aa ALIAS Renderer_aa)
target_sources(Renderer_aa PRIVATE aa.cc)
target_include_directories(Renderer_aa PUBLIC ${CMAKE_SOURCE_DIR}/src)
# R.3-AA: aa_resolve_tile composes rgb565() (gfx/framebuffer.h) for the per-
# channel offset blend. rgb565 is a header-only static inline (no link symbol),
# but declare the dependency explicitly (no cycle: gfx does not depend on aa).
# PRIVATE — not part of aa.h's interface.
target_link_libraries(Renderer_aa
    PUBLIC renderer::sanitizers
    PRIVATE renderer::gfx)
orthodoxy_enforce(Renderer_aa)
pico_tidy_enable(Renderer_aa)
