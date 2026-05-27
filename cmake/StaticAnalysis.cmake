# clang-tidy target (host build only). Based on cpp-template's StaticAnalysis.cmake,
# adapted for this project: headers live under src/ (no include/), and pico-only and
# generated sources are excluded since they are not in the host compile DB.

# Prefer the versioned binary (CI installs clang-tidy-22; no unversioned symlink
# is guaranteed on the runner) and keep the .clang-tidy checks consistent with
# the clang-22 toolchain the Orthodoxy/CI jobs use.
find_program(CLANG_TIDY_EXECUTABLE NAMES clang-tidy-22 clang-tidy)
if(CLANG_TIDY_EXECUTABLE)
  # Lint only the game code (game/gfx/app), matching the .clang-tidy
  # HeaderFilterRegex and the "platform/ and tests are intentionally exempt"
  # policy (see README). platform_*.cc / main_*.cc are the SDL/bare-metal entry
  # points (engine code you don't touch); the generated assets_gen.cc is skipped.
  file(GLOB_RECURSE TIDY_SOURCE_FILES ${PROJECT_SOURCE_DIR}/src/*.cc)
  list(FILTER TIDY_SOURCE_FILES INCLUDE REGEX "/src/(game|gfx|app)/")
  list(FILTER TIDY_SOURCE_FILES EXCLUDE REGEX "assets_gen\\.cc$")

  # The orthodoxy plugin (-fplugin) in the compile DB is built for a different
  # LLVM and crashes clang-tidy, so first regenerate a sanitized tidy-only DB.
  # No-op when the plugin is absent (e.g. CI's tidy job), since the strip regex
  # then matches nothing.
  add_custom_target(tidy
    COMMAND ${CMAKE_COMMAND}
      -DTIDY_DB_IN=${PROJECT_BINARY_DIR}/compile_commands.json
      -DTIDY_DB_OUT_DIR=${PROJECT_BINARY_DIR}/tidy
      -P ${PROJECT_SOURCE_DIR}/cmake/StripOrthodoxyPlugin.cmake
    COMMAND ${CLANG_TIDY_EXECUTABLE}
      -p ${PROJECT_BINARY_DIR}/tidy
      --warnings-as-errors=*
      ${TIDY_SOURCE_FILES}
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Running clang-tidy (orthodoxy plugin stripped from a tidy-only compile DB)"
  )
endif()
