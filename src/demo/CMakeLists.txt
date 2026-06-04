# Demo app (integrator-owned, C1). Ports the gldemo flat/shaded scene through
# the rdr façade. NOT orthodoxy_enforce'd (app-level carve-out, like src/app).
#
# The scene math/geometry is a platform-agnostic library (Renderer::DemoScene)
# so the ACTUAL demo scene is host-testable (tests/demo) — the black-screen
# regression escaped because the demo's build path was never exercised on host.
# main.cc is the thin per-platform entry (loop + present + telemetry).

add_library(Renderer_demo_scene STATIC demo_scene.cc)
add_library(Renderer::DemoScene ALIAS Renderer_demo_scene)
target_include_directories(Renderer_demo_scene PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(Renderer_demo_scene
  PUBLIC renderer::sanitizers
  PRIVATE Renderer::Cmd Renderer::Fixed renderer::gfx)
pico_tidy_enable(Renderer_demo_scene)

if(PLATFORM STREQUAL "picosystem")
  add_executable(renderer_demo main.cc)
  target_link_libraries(renderer_demo PRIVATE
    Renderer::rdr Renderer::DemoScene Renderer::Aa renderer::gfx
    renderer::platform_pico)
  target_include_directories(renderer_demo PRIVATE ${CMAKE_SOURCE_DIR}/src)
  pico_set_linker_script(renderer_demo
    ${CMAKE_SOURCE_DIR}/src/platform/memmap_picosystem.ld)
  pico_enable_stdio_usb(renderer_demo 1)
  pico_enable_stdio_uart(renderer_demo 0)
  pico_add_extra_outputs(renderer_demo)
  pico_tidy_enable(renderer_demo)
else()
  add_executable(renderer_demo main.cc)
  target_link_libraries(renderer_demo PRIVATE
    Renderer::rdr Renderer::DemoScene Renderer::Aa renderer::gfx
    renderer::platform_sdl project_warnings)
  target_include_directories(renderer_demo PRIVATE ${CMAKE_SOURCE_DIR}/src)
endif()
