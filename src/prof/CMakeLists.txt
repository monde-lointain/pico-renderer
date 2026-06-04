# Granular intra-frame profiler (T5). CARVE-OUT module: NOT orthodoxy_enforce'd
# — the RAII profile_block ctor/dtor (prof.h) are a user-sanctioned exception to
# Orthodox C++. Off by default; renderer::prof_config carries PROFILER=1 only on
# the pico-prof / -DRENDERER_PROFILER=ON build, and is linked PUBLIC so a SINGLE
# source defines PROFILER for every instrumented consumer (raster/rdr/sched/demo).
add_library(Renderer_prof)
add_library(Renderer::Prof ALIAS Renderer_prof)
target_sources(Renderer_prof PRIVATE prof.cc)
target_include_directories(Renderer_prof PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(Renderer_prof
    PUBLIC renderer::sanitizers renderer::prof_config)

if(PLATFORM STREQUAL "picosystem")
  # Device timers: time_us_64 (pico_time), systick_hw (hardware_structs),
  # clock_get_hz (hardware_clocks). PUBLIC so PICO_ON_DEVICE (carried by
  # pico_platform under these) reaches EVERY consumer's compilation -> all
  # firmware TUs agree on PROF_NUM_CORES==2 (no g_prof array-size skew).
  target_sources(Renderer_prof PRIVATE prof_pico.cc)
  target_link_libraries(Renderer_prof PUBLIC pico_time hardware_structs hardware_clocks)
else()
  target_sources(Renderer_prof PRIVATE prof_host.cc)
endif()

pico_tidy_enable(Renderer_prof)
