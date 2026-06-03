add_library(Renderer_raster)
add_library(Renderer::Raster ALIAS Renderer_raster)
target_sources(Renderer_raster PRIVATE raster.cc)
target_include_directories(Renderer_raster PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(Renderer_raster
    PUBLIC renderer::sanitizers
    # R.1: raster_one composes the texture sampler + color combiner. Neither is
    # part of raster.h's interface -> PRIVATE; CMake still propagates the static-
    # lib link requirement to the final executable.
    PRIVATE Renderer::Tex Renderer::Shade)
orthodoxy_enforce(Renderer_raster)
pico_tidy_enable(Renderer_raster)
