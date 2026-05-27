# Demo app (integrator-owned, C1). Ports the gldemo flat/shaded scene through
# the rdr façade. NOT orthodoxy_enforce'd (app-level carve-out, like src/app).
#
# Builds on BOTH platforms: a host executable (renderer_demo, bounded frames for
# the host smoke) and the device .uf2 (renderer_demo, free-running with USB-CDC
# telemetry). The renderer libraries are platform-agnostic; only the linked
# platform HAL + main entry differ.

if(PLATFORM STREQUAL "picosystem")
  add_executable(renderer_demo main.cc)
  target_link_libraries(renderer_demo PRIVATE
    Renderer::rdr Renderer::Cmd Renderer::Fixed renderer::gfx
    renderer::platform_pico)
  target_include_directories(renderer_demo PRIVATE ${CMAKE_SOURCE_DIR}/src)
  # PicoSystem memory map + USB-CDC stdio for the frame_ms= telemetry.
  pico_set_linker_script(renderer_demo
    ${CMAKE_SOURCE_DIR}/src/platform/memmap_picosystem.ld)
  pico_enable_stdio_usb(renderer_demo 1)
  pico_enable_stdio_uart(renderer_demo 0)
  pico_add_extra_outputs(renderer_demo)
  pico_tidy_enable(renderer_demo)
else()
  add_executable(renderer_demo main.cc)
  target_link_libraries(renderer_demo PRIVATE
    Renderer::rdr Renderer::Cmd Renderer::Fixed renderer::gfx
    renderer::platform_sdl project_warnings)
  target_include_directories(renderer_demo PRIVATE ${CMAKE_SOURCE_DIR}/src)
endif()
