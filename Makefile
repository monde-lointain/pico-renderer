# Convenience shim over CMake/CMakePresets for picosystem-template.
# Style follows cpp/cmake-template's Makefile (VERBOSE/Q toggle, variable-driven
# config, configured-build sentinel), adapted to this project's two presets:
#   host -> build-host  (SDL3 + ImGui + tests)
#   pico -> build-pico  (RP2040 .uf2 firmware)

# Set VERBOSE=1 to echo the underlying commands.
VERBOSE ?= 0
ifeq ($(VERBOSE),1)
export Q :=
else
export Q := @
endif

PRESET ?= host
BUILD_DIR := build-$(PRESET)
CONFIGURED := $(BUILD_DIR)/CMakeCache.txt

# Pico firmware artifact (produced by the pico preset).
UF2 := build-pico/src/renderer.uf2

JOBS ?= $(shell nproc)

# Optional configure-time overrides.
BUILD_TYPE ?=
OPTIONS ?=
CONFIGURE_OPTS := $(if $(BUILD_TYPE),-DCMAKE_BUILD_TYPE=$(BUILD_TYPE)) $(OPTIONS)

# Files clang-format touches: all engine/game/test sources minus the generated table.
FORMAT_FILES := $(shell find src tests \( -name '*.cc' -o -name '*.h' \) -not -name 'assets_gen.*')

.DEFAULT_GOAL := help

.PHONY: all build test run reconfig clean distclean help
.PHONY: format format-patch tidy tidy-pico assets sanitize flash

all: build

# ---- core (preset-aware) -------------------------------------------------
build: | $(CONFIGURED)
	$(Q)cmake --build --preset $(PRESET) -j$(JOBS)

# Tests are host-only; never configure the pico tree just to skip.
test:
ifeq ($(PRESET),pico)
	@echo "test: skipped (no test preset for the pico build; tests are host-only)"
else
	$(Q)$(MAKE) --no-print-directory build
	$(Q)ctest --preset host
endif

# Build the host demo and launch it. Needs a display (not headless/SSH).
run:
	$(Q)$(MAKE) --no-print-directory build PRESET=host
	$(Q)./build-host/src/renderer

# Configure when a build dir has no cache yet. Pattern rule so both build-host and
# build-pico are covered by one recipe (tidy depends on build-host's cache directly).
build-%/CMakeCache.txt:
	$(Q)cmake --preset $* $(CONFIGURE_OPTS)

reconfig:
	$(Q)rm -rf $(BUILD_DIR)
	$(Q)cmake --preset $(PRESET) $(CONFIGURE_OPTS)

clean:
	$(Q)if [ -d $(BUILD_DIR) ]; then cmake --build $(BUILD_DIR) --target clean; fi

distclean:
	$(Q)rm -rf build-*

# ---- tooling -------------------------------------------------------------
# format / format-patch / assets run directly (no CMake configure, so no SDL3 clone).
format:
	$(Q)clang-format -i $(FORMAT_FILES)

format-patch:
	$(Q)clang-format --dry-run --Werror $(FORMAT_FILES)

# clang-tidy is host-only and needs a built host tree (SDL3 generates headers at
# build time) plus its compile DB. Always operate on build-host regardless of PRESET.
tidy: | build-host/CMakeCache.txt
	$(Q)cmake --build build-host -j$(JOBS)
	$(Q)cmake --build build-host --target tidy

# clang-tidy over the pico build (arm-none-eabi), run inline by CMake during the
# build. Needs the ARM toolchain + pico-sdk + clang-tidy. Findings fail the build.
# Separate build dir (build-pico-tidy) keeps the .uf2 build (build-pico) clean.
tidy-pico: | build-pico-tidy/CMakeCache.txt
	$(Q)cmake --build build-pico-tidy -j$(JOBS)

assets:
	$(Q)python3 tools/gen_assets.py assets src/gfx/assets_gen.h src/gfx/assets_gen.cc

# Build the pico firmware and flash it. picotool needs the RP2040 in BOOTSEL (USB);
# without picotool, fall back to a copy-to-mount hint.
flash:
	$(Q)$(MAKE) --no-print-directory build PRESET=pico
	$(Q)if command -v picotool >/dev/null 2>&1; then \
	  picotool load -x $(UF2); \
	else \
	  echo "picotool not found; copy $(UF2) to the mounted RPI-RP2 drive."; \
	fi

# ---- sanitizers ----------------------------------------------------------
# Build the ASan/UBSan tree (clang-22, Debug) and run the tests under the survey
# env (recoverable: collects all findings instead of aborting on the first).
# container/odr-overflow off — uninstrumented gtest/SDL cross the instrumentation
# boundary and would trip bogus reports. LSan suppressions cover third-party/
# static leaks so PRE_TEST gtest discovery can enumerate.
SANITIZE_ENV := \
  ASAN_OPTIONS=detect_leaks=1:abort_on_error=0:detect_container_overflow=0:detect_odr_violation=0 \
  UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=0:report_error_type=1 \
  LSAN_OPTIONS=suppressions=$(CURDIR)/tools/lsan.supp \
  ASAN_SYMBOLIZER_PATH=$(shell command -v llvm-symbolizer-22 llvm-symbolizer 2>/dev/null | head -1)

sanitize: | build-asan/CMakeCache.txt
	$(Q)cmake --build build-asan -j$(JOBS)
	$(Q)$(SANITIZE_ENV) ctest --preset host-asan --output-on-failure

build-asan/CMakeCache.txt:
	$(Q)cmake --preset host-asan $(CONFIGURE_OPTS)

# ---- help ----------------------------------------------------------------
help:
	@echo "picosystem-template - make targets"
	@echo ""
	@echo "Variables:"
	@echo "  PRESET=host|pico   Target preset (default: host). pico builds the .uf2."
	@echo "  BUILD_TYPE=<type>  Override the preset's CMAKE_BUILD_TYPE (e.g. Release)."
	@echo "  OPTIONS=<-D...>    Extra cmake configure flags."
	@echo "  JOBS=<n>           Parallel build jobs (default: nproc)."
	@echo "  VERBOSE=1          Echo underlying commands."
	@echo ""
	@echo "Core:"
	@echo "  build         Configure (if needed) and build the selected preset (default goal: help)"
	@echo "  run           Build host and launch the demo (needs a display)"
	@echo "  test          Build host preset and run ctest (host-only)"
	@echo "  reconfig      Wipe and reconfigure the selected build dir"
	@echo "  clean         Run the build system's clean target"
	@echo "  distclean     Remove all build-* directories"
	@echo ""
	@echo "Tooling:"
	@echo "  format        clang-format -i over src/ + tests/ (excludes generated assets)"
	@echo "  format-patch  clang-format dry-run check (fails on any diff)"
	@echo "  tidy          Build host, then gated clang-tidy (findings fail)"
	@echo "  tidy-pico     Build pico with inline clang-tidy (arm-none-eabi; findings fail)"
	@echo "  sanitize      Build clang-22 ASan/UBSan tree, run tests (survey env)"
	@echo "  assets        Regenerate src/gfx/assets_gen.{h,cc} from assets/*.png"
	@echo "  flash         Build pico and flash the .uf2 via picotool (RP2040 in BOOTSEL)"
	@echo ""
	@echo "Note: the pico build needs the arm-none-eabi toolchain + ninja + pico-sdk"
	@echo "      (set PICO_SDK_PATH, or PICO_SDK_FETCH_FROM_GIT=ON);"
	@echo "      'make tidy-pico' additionally needs clang-tidy-22 on PATH."
	@echo "      'make sanitize' needs clang-22 / clang++-22 on PATH."
