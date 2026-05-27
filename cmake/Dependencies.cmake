# Host (sdl) dependencies pulled via FetchContent from git (portable: builds on
# any machine with a network + SDL's system build deps).
#
# Offline / local-checkout override (per dep, CMake-native):
#   -DFETCHCONTENT_SOURCE_DIR_SDL3=/path/to/SDL
#   -DFETCHCONTENT_SOURCE_DIR_IMGUI=/path/to/imgui
# To share one clone across several games, set -DFETCHCONTENT_BASE_DIR=/path.

# GIT_TAG values are pinned to commit SHAs (not moving tags) for reproducible
# builds; the tag the SHA corresponds to is in the trailing comment. Bump
# deliberately.
include(FetchContent)

# ---- SDL3 (static, no tests/examples) ------------------------------------
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
FetchContent_Declare(
  SDL3
  GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
  GIT_TAG d9d5536704d585616d4db3c8ba3c4ff6fc2757e1  # release-3.4.8
  # No GIT_SHALLOW: shallow fetch can't resolve an arbitrary commit SHA.
  SYSTEM  # treat SDL3 headers as system so they don't trip our -Wall -Wextra -Wpedantic
)
FetchContent_MakeAvailable(SDL3)

# ---- Dear ImGui (no upstream CMake; hand-rolled target + SDL3 backends) ---
# ImGui ships no CMakeLists, so fetch the source then build it ourselves.
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG 8936b58fe26e8c3da834b8f60b06511d537b4c63  # v1.92.8
  # No GIT_SHALLOW: shallow fetch can't resolve an arbitrary commit SHA.
)
# ImGui ships no CMakeLists, so MakeAvailable only populates (sets imgui_SOURCE_DIR);
# the hand-rolled target below compiles it. Replaces the deprecated manual
# FetchContent_Populate pattern (single-arg form removed under CMP0169, CMake 3.30+).
# If a future imgui adds a CMakeLists, MakeAvailable would add_subdirectory it and
# collide with the `imgui` target below — revisit on version bump.
FetchContent_MakeAvailable(imgui)
add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
)
target_include_directories(imgui PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui PUBLIC SDL3::SDL3)

# ---- GoogleTest ----------------------------------------------------------
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG 52eb8108c5bdec04579160ae17225d66034bd723  # v1.17.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
