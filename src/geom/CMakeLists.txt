add_library(Renderer_geom)
add_library(Renderer::Geom ALIAS Renderer_geom)
target_sources(Renderer_geom PRIVATE geom.cc)
target_include_directories(Renderer_geom PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(Renderer_geom
    PUBLIC renderer::sanitizers Renderer::Fixed Renderer::Clip Renderer::Cmd
           Renderer::Arena
    # CL-6: geom.cc calls tex_validate() at material intern (R.1 handoff).
    # PRIVATE: an implementation dependency — geom.h exposes no tex types beyond
    # what rdr/types.h already provides.
    PRIVATE Renderer::Tex)
orthodoxy_enforce(Renderer_geom)
pico_tidy_enable(Renderer_geom)
