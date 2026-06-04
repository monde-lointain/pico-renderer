add_library(Renderer_rdr)
add_library(Renderer::rdr ALIAS Renderer_rdr)
target_sources(Renderer_rdr PRIVATE rdr.cc)
target_include_directories(Renderer_rdr PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(Renderer_rdr
    PUBLIC renderer::sanitizers
    # T5: PROF_CLEAR_BLIT + PROF_RASTER_DISPATCH blocks live INSIDE rdr_end_frame
    # (the fb clear + 2D blits + sched_rasterize are one call — not bracketable
    # from main.cc). prof.h is .cc-only -> PRIVATE; PROFILER=0 -> macros vanish.
    PRIVATE Renderer::Arena Renderer::Geom Renderer::Sched Renderer::Blit2d
            Renderer::Prof)
orthodoxy_enforce(Renderer_rdr)
pico_tidy_enable(Renderer_rdr)
