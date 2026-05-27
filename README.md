# PicoSystem game template

A starting point for building games for the [Pimoroni PicoSystem](https://shop.pimoroni.com/products/picosystem)
(RP2040 handheld, 240×240 ST7789 LCD, 8 buttons, mono beeper). The same code runs on:

- **Host (SDL3)** — fast iterate-and-test loop on your PC, with an ImGui upscale menu.
- **Device (PicoSystem)** — a flashable `.uf2` for the real hardware.

It ships a tiny placeholder game (a title screen, then a square you move with the D-pad) that exercises the
whole engine. Replace that with your own game.

## Layout

```
src/
  platform/   ENGINE  thin hardware abstraction (HAL): one interface, two backends
              platform.h           the 7-function contract (input, present, millis, seed, log, audio)
              platform_sdl.cc      host backend (SDL3 + ImGui)
              platform_pico.cc     device backend (bare-metal RP2040: ST7789 via PIO/DMA, PWM audio)
  gfx/        ENGINE  software renderer: framebuffer, sprites, blitter, font, asset table
  game/       YOURS   game logic. Ships only rng (seedable xorshift32); add your rules here.
  app/        YOURS   the presenter: app_init / app_tick / app_take_cue (see "the contract" below)
  main_sdl.cc / main_pico.cc   per-platform entry points (own the 30fps loop)
tests/        host-only GoogleTest suites (game / gfx / app + a smoke test)
tools/        gen_assets.py: PNG -> RGB565 + 1-bpp mask C source
assets/       *.png; the build regenerates src/gfx/assets_gen.{h,cc} when these change
cmake/        Dependencies (SDL3/ImGui/GoogleTest), Orthodoxy plugin, pico-sdk import
```

**Engine vs your game:** `platform/` and `gfx/` are the reusable engine — leave them. `game/` and `app/`
are where your game lives.

## Build & run

Requires CMake ≥ 3.28. Configure presets live in `CMakePresets.json`.

```sh
# Host (SDL3 + tests). First configure clones SDL3 from git — slow (minutes), one-time.
cmake --preset host
cmake --build --preset host
ctest --preset host
./build-host/src/renderer        # run the demo

# Device (.uf2 for PicoSystem). Needs the arm-none-eabi toolchain + ninja + pico-sdk
# (point PICO_SDK_PATH at your checkout, or set PICO_SDK_FETCH_FROM_GIT=ON).
cmake --preset pico
cmake --build --preset pico
# -> build-pico/src/renderer.uf2  (drag onto the PicoSystem in BOOTSEL mode)
```

Or use the `Makefile` shim (run `make help` for the full list):

```sh
make run                # build host + launch the demo
make test               # build host + ctest
make flash              # build pico + flash the .uf2 via picotool (RP2040 in BOOTSEL)
make build PRESET=pico  # just build the firmware
make format / make tidy / make sanitize   # see "Quality gates" below
make tidy-pico          # clang-tidy over the pico (arm-none-eabi) build
```

### Dependencies
SDL3, Dear ImGui, and GoogleTest are fetched from git via `FetchContent` (see `cmake/Dependencies.cmake`),
so a host build works on any machine with a network and SDL's system build deps (X11/Wayland/ALSA dev
headers on Linux). Two tips:

- **Offline / reuse a local checkout:** `cmake --preset host -DFETCHCONTENT_SOURCE_DIR_SDL3=/path/to/SDL`
  (same for `..._IMGUI`).
- **Share one SDL clone across several games:** `-DFETCHCONTENT_BASE_DIR=/some/shared/dir`.

pico-sdk is located via `PICO_SDK_PATH` (env or cache), with a `PICO_SDK_FETCH_FROM_GIT=ON` fallback if
neither is set (resolved by `cmake/pico_sdk_import.cmake`, which errors if the SDK can't be found). The pico build targets `PICO_BOARD=pimoroni_picosystem` and uses the
PicoSystem memory map (`src/platform/memmap_picosystem.ld`, 12 MiB user FLASH + 4 MiB FAT region) via
`pico_set_linker_script`.

## The app contract

Your game is just three functions (`src/app/app.h`); the platform owns the loop and calls them. The App
never calls `plat_*`, so it is fully unit-testable without hardware (see `tests/app/`).

```c
void     app_init(App* a, uint32_t seed);                       // once at startup
void     app_tick(App* a, const struct Input* in,               // each frame (~30fps)
                  uint32_t now_ms, struct Framebuffer* fb);      //   read input, update, render into fb
enum Cue app_take_cue(App* a);                                  // pull one audio cue per frame
```

`Input` carries `held` / `pressed` button bitmasks (`BTN_UP/DOWN/LEFT/RIGHT/A/B/X/Y`). Render with the gfx
primitives — `frect`/`rect`/`hline`/`vline`, `text(...)`, and `blit_mask(fb, sprite_get(SPRITE_*), x, y, ...)`.

## Starting a new game

1. **Rename the project.** The CMake project, library namespace, executable, and window title all use the
   token `renderer`. Replace it:
   - `CMakeLists.txt` — the two `project(renderer ...)` lines
   - `src/*/CMakeLists.txt`, `src/CMakeLists.txt`, `tests/*/CMakeLists.txt` — the `renderer::*`
     aliases and link references
   - `src/platform/platform_sdl.cc` — the window title string
   (The directory may use a hyphen, e.g. `picosystem-template`; the CMake project uses an underscore because
   `::`-namespaced CMake target names can't contain hyphens.)
2. **Write your game.** Replace `src/app/app.cc` (and `app.h`'s `App` struct / `Cue` values) with your logic,
   and put game rules under `src/game/`. Keep the `app_init`/`app_tick`/`app_take_cue` contract.
3. **Add art.** Drop `*.png` files into `assets/`; each becomes a `SPRITE_<NAME>` id automatically (the build
   reruns `tools/gen_assets.py`). `pico8_font.png` is required by the font renderer; render sprites with
   `blit_mask(fb, sprite_get(SPRITE_<NAME>), x, y, ...)`.
4. **Test on host, then flash.** Iterate with the `host` preset + `ctest`; build the `pico` preset for the
   device.

## Quality gates

- **Format** — `make format` (clang-format `-i`); `make format-patch` checks without writing (fails on diff).
- **clang-tidy** — `make tidy` runs clang-tidy gated (`--warnings-as-errors=*`) over `src/{game,gfx,app}`.
- **clang-tidy (pico)** — `make tidy-pico` runs clang-tidy inline over the pico (arm-none-eabi) build, so
  device-only paths (`platform_pico.cc`/`main_pico.cc`) and shared code under RP2040 macros are linted as
  compiled for the device — which `make tidy` (host compile DB) can't see. Needs the arm-none-eabi toolchain,
  pico-sdk, and `clang-tidy-22` on PATH.
- **Sanitizers** — `make sanitize` builds a clang-22 ASan+UBSan tree (`host-asan` preset) and runs the tests
  under them. Catches runtime UB (out-of-bounds, misaligned loads, signed overflow) that static analysis
  can't see and that can diverge x86 → arm. Needs `clang-22`/`clang++-22` on PATH.
- **CI** (`.github/workflows/`) — `ci.yml` builds + tests on Linux/clang, Windows/MSVC, macOS/AppleClang,
  cross-compiles the firmware for ARM, and runs gated clang-tidy on both the host and the pico
  (arm-none-eabi) build. `sanitize.yml` runs the ASan/UBSan suite nightly. (CI runs only once you add a
  GitHub remote.)

## Coding standard & Orthodoxy

The engine and game code follow **Orthodox C++** (a C-like subset; see `CLAUDE.md`). On a host build with a
matching clang + the `orthodoxy` plugin installed, this is *enforced* on `src/{game,gfx,app}` at compile time;
otherwise it is silently skipped and the build is still valid. Note the plugin must match your clang's major
version — the default `host` preset uses plain `clang`, so enforcement only runs if a plugin is installed for
that version. `platform/` and tests are intentionally exempt.

To set up the plugin and make enforcement *required* (so a silently-inactive plugin fails loudly), see
[`docs/setup-llvm.md`](docs/setup-llvm.md). Configure with `-DRENDERER_REQUIRE_ORTHODOXY=ON` to
assert the plugin is live; the `renderer_orthodoxy_canary` test (a deliberate violation that *must*
fail to compile) then proves enforcement actually rejects bad code. CI's Linux job pins this on.
