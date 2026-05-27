---
name: renderer-module-scaffold
description: Use when creating a new renderer module — generate src/<mod>/{<mod>.h,<mod>.cc,CMakeLists.txt} and tests/<mod>/{<mod>_test.cc,CMakeLists.txt} matching the project's CMake conventions.
---

# Renderer Module Scaffold

Assumes the module's subdir is already registered in `src/CMakeLists.txt` and listed in `tests/CMakeLists.txt`'s EXISTS-guarded foreach (done in Stream A).

## Generate
- `src/<mod>/<mod>.h` — `#include "rdr/types.h"` + `module_verb` declarations (see `contract-first`).
- `src/<mod>/<mod>.cc` — stub bodies returning `RDR_OK`.
- `src/<mod>/CMakeLists.txt`:
  ```cmake
  add_library(Renderer_<Mod>)
  add_library(Renderer::<Mod> ALIAS Renderer_<Mod>)
  target_sources(Renderer_<Mod> PRIVATE <mod>.cc)
  target_include_directories(Renderer_<Mod> PUBLIC ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(Renderer_<Mod> PUBLIC renderer::sanitizers)
  orthodoxy_enforce(Renderer_<Mod>)        # modules ONLY — never harness/demo (ImGui carve-out)
  ```
- `tests/<mod>/<mod>_test.cc` — GoogleTest.
- `tests/<mod>/CMakeLists.txt`:
  ```cmake
  add_executable(<mod>_test <mod>_test.cc)
  target_link_libraries(<mod>_test PRIVATE Renderer::<Mod> gtest_main)
  gtest_discover_tests(<mod>_test PROPERTIES LABELS <mod>)   # -L <mod> selects it (per-task gate)
  ```

## Never
- Never `orthodoxy_enforce` `tests/harness/` (ImGui) or `src/demo/` — they're app/inspector-level (carve-out).
