add_library(Renderer_raster)
add_library(Renderer::Raster ALIAS Renderer_raster)
target_sources(Renderer_raster PRIVATE raster.cc)
target_include_directories(Renderer_raster PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(Renderer_raster
    PUBLIC renderer::sanitizers
    # R.1: raster_one composes the texture sampler + color combiner + the
    # rgb565() packer (gfx/framebuffer.h). None is part of raster.h's interface
    # -> PRIVATE; CMake still propagates the static-lib link requirement to the
    # final executable. rgb565 is a header-only static inline (no link symbol),
    # but link renderer::gfx explicitly so the dependency is declared (no
    # cycle: gfx does not depend on raster).
    # R.2: the XLU sweep composes blend_pixel_alpha (Renderer::Blend) for the
    # texel-alpha alpha-over; also PRIVATE (not in raster.h's interface).
    PRIVATE Renderer::Tex Renderer::Shade Renderer::Blend renderer::gfx)
orthodoxy_enforce(Renderer_raster)
pico_tidy_enable(Renderer_raster)
