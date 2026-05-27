add_library(Renderer_sched)
add_library(Renderer::Sched ALIAS Renderer_sched)

# sched.cc is the platform-agnostic seam (like platform/scanrace.cc): it calls
# sched_dispatch_tiles, whose definition is substituted at link time. The two
# dispatch bodies define the SAME symbol, so exactly ONE is compiled per target
# (mirrors src/platform's platform_host vs platform_pico split):
#   - dispatch_host.cc : serial, worker 0 (zbuf[0]). Host + every test + SDL.
#   - dispatch_pico.cc : dual-core (core0->zbuf[0], core1->zbuf[1]). Firmware.
target_sources(Renderer_sched PRIVATE sched.cc)
target_include_directories(Renderer_sched PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(Renderer_sched
    PUBLIC renderer::sanitizers
    PRIVATE Renderer::Geom Renderer::Raster)

if(PLATFORM STREQUAL "picosystem")
  target_sources(Renderer_sched PRIVATE dispatch_pico.cc)
  # Dual-core dispatch needs the multicore launch/FIFO and a hardware spinlock.
  # PUBLIC so the demo firmware (renderer_demo), which links Renderer::Sched
  # transitively, picks up pico_multicore + hardware_sync.
  target_link_libraries(Renderer_sched PUBLIC pico_multicore hardware_sync)
else()
  target_sources(Renderer_sched PRIVATE dispatch_host.cc)
endif()

orthodoxy_enforce(Renderer_sched)
pico_tidy_enable(Renderer_sched)
