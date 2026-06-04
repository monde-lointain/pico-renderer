# Subsystem wiring. Each subsystem owns its own CMakeLists.txt below; this file
# only wires them and builds the final per-platform executable.

add_subdirectory(game)
add_subdirectory(gfx)
add_subdirectory(app)
add_subdirectory(platform)

# Renderer modules (Stream A pre-registration; feature branches add only their own
# sources behind these already-registered subdirs — never edit this file again).
add_subdirectory(prof)  # T5: granular profiler (early — no module deps; others link it)
add_subdirectory(rdr)
add_subdirectory(fixed)
add_subdirectory(arena)
add_subdirectory(cmd)
add_subdirectory(geom)
add_subdirectory(clip)
add_subdirectory(raster)
add_subdirectory(aa)
add_subdirectory(tex)
add_subdirectory(shade)
add_subdirectory(blend)
add_subdirectory(sched)
add_subdirectory(blit2d)  # Wave-D D.5: 2D sky background blits (NEW module)
add_subdirectory(demo)

# Final executable, one entry point per platform.
if(PLATFORM STREQUAL "picosystem")
  add_executable(renderer main_pico.cc)
  target_link_libraries(renderer PRIVATE
    renderer::app renderer::game renderer::gfx renderer::platform_pico)
  # Use the PicoSystem memory map (12 MiB user FLASH + 4 MiB FAT region), as the
  # Pimoroni PicoSystem SDK examples do via pico_set_linker_script.
  pico_set_linker_script(renderer
    ${CMAKE_CURRENT_SOURCE_DIR}/platform/memmap_picosystem.ld)
  pico_enable_stdio_usb(renderer 1)
  pico_enable_stdio_uart(renderer 0)
  pico_add_extra_outputs(renderer)
  pico_tidy_enable(renderer)
else()
  add_executable(renderer main_sdl.cc)
  target_link_libraries(renderer PRIVATE
    renderer::app renderer::game renderer::gfx renderer::platform_sdl project_warnings)
endif()

target_include_directories(renderer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
