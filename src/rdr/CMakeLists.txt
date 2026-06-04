add_library(Renderer_rdr)
add_library(Renderer::rdr ALIAS Renderer_rdr)
target_sources(Renderer_rdr PRIVATE rdr.cc)
target_include_directories(Renderer_rdr PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(Renderer_rdr
    PUBLIC renderer::sanitizers
    PRIVATE Renderer::Arena Renderer::Geom Renderer::Sched Renderer::Blit2d)
orthodoxy_enforce(Renderer_rdr)
pico_tidy_enable(Renderer_rdr)
