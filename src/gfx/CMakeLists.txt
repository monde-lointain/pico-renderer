# Stream C: gfx STATIC library.

# ---- asset generation custom command --------------------------------------
# Regenerate assets_gen.{h,cc} whenever any PNG in assets/ changes.
file(GLOB ASSET_PNGS CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/assets/*.png")

set(ASSETS_GEN_H  "${CMAKE_CURRENT_SOURCE_DIR}/assets_gen.h")
set(ASSETS_GEN_CC "${CMAKE_CURRENT_SOURCE_DIR}/assets_gen.cc")

add_custom_command(
  OUTPUT  ${ASSETS_GEN_H} ${ASSETS_GEN_CC}
  COMMAND ${CMAKE_COMMAND} -E env
      python3 ${PROJECT_SOURCE_DIR}/tools/gen_assets.py
              ${PROJECT_SOURCE_DIR}/assets
              ${ASSETS_GEN_H}
              ${ASSETS_GEN_CC}
  DEPENDS ${ASSET_PNGS} ${PROJECT_SOURCE_DIR}/tools/gen_assets.py
  COMMENT "Regenerating assets_gen.h / assets_gen.cc from assets/*.png"
  VERBATIM
)

add_custom_target(gen_assets DEPENDS ${ASSETS_GEN_H} ${ASSETS_GEN_CC})

# ---- gfx static library ---------------------------------------------------
add_library(gfx STATIC
    assets_gen.cc
    blit.cc
    font.cc
)
add_library(renderer::gfx ALIAS gfx)

add_dependencies(gfx gen_assets)

target_include_directories(gfx PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(gfx PRIVATE project_warnings)
# PUBLIC so test binaries linking this lib inherit -fsanitize (no-op when off).
target_link_libraries(gfx PUBLIC renderer::sanitizers)
orthodoxy_enforce(gfx)
pico_tidy_enable(gfx)
# assets_gen.cc is generated, not authored; exclude it from inline (pico) clang-tidy.
# The host `tidy` target already excludes it by name (cmake/StaticAnalysis.cmake);
# SKIP_LINTING is the CXX_CLANG_TIDY equivalent (CMake 3.27+).
set_source_files_properties(${ASSETS_GEN_CC} PROPERTIES SKIP_LINTING ON)
