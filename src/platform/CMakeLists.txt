# src/platform/CMakeLists.txt — HAL targets for E.1 (platform_sdl) and F.1 (platform_pico).
# Each is a STATIC lib implementing platform.h; the executable links exactly one.

if(PLATFORM STREQUAL "picosystem")
  # ---- F.1 — PicoSystem bare-metal HAL ------------------------------------
  # Generate the PIO header from screen.pio before the library is compiled.
  # pico_generate_pio_header wires a custom command that runs pioasm and puts
  # screen.pio.h in the build tree; it also adds the generated directory to
  # the target's include path.
  add_library(platform_pico STATIC
    platform_pico.cc
  )
  add_library(renderer::platform_pico ALIAS platform_pico)
  pico_generate_pio_header(platform_pico
    ${CMAKE_CURRENT_LIST_DIR}/screen.pio
  )
  target_include_directories(platform_pico PUBLIC
    ${PROJECT_SOURCE_DIR}/src
  )
  target_link_libraries(platform_pico PUBLIC
    pico_stdlib
    hardware_pio
    hardware_spi
    hardware_dma
    hardware_pwm
    hardware_adc
    hardware_irq
    hardware_vreg
    hardware_clocks
    hardware_gpio
    pico_rand
  )
  # Apply -fno-exceptions/-fno-rtti to C++ source only (avoids cc1 warning on .c SDK files).
  # COMPILE_LANGUAGE generator expression scopes the flags to the target's C++ sources.
  target_compile_options(platform_pico PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti>
  )
  pico_tidy_enable(platform_pico)

else()
  # ---- E.1 — SDL3 host HAL ------------------------------------------------
  add_library(platform_sdl STATIC
    platform_sdl.cc
  )
  add_library(renderer::platform_sdl ALIAS platform_sdl)
  target_include_directories(platform_sdl PUBLIC
    ${PROJECT_SOURCE_DIR}/src
  )
  target_link_libraries(platform_sdl PUBLIC
    SDL3::SDL3
    imgui
  )
  # platform_sdl.cc is Orthodoxy-exempt (SDL/ImGui use modern C++ idioms).
  # Do NOT apply project_warnings or orthodoxy::plugin here.

endif()
