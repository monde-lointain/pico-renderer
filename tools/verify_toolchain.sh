#!/usr/bin/env bash
# Fail loudly if any tool the workflow gates depend on is missing.
set -uo pipefail
fail=0
need() { command -v "$1" >/dev/null 2>&1 || { echo "MISSING: $1 ($2)"; fail=1; }; }
need clang-22         "host compiler (CMakePresets host-asan pins clang-22)"
need clang++-22       "host compiler"
need clang-tidy-22    "host + pico clang-tidy"
need arm-none-eabi-gcc "pico cross-compiler"
need picotool         "device flash + reboot-to-BOOTSEL"
need ccache           "shared object cache across worktrees"
need cmake            "build system (>=3.28)"
need ninja            "pico generator"
need jq               "hook JSON parsing"
if [ -z "${PICO_SDK_PATH:-}" ] && [ "${PICO_SDK_FETCH_FROM_GIT:-}" != "ON" ]; then
  echo "MISSING: PICO_SDK_PATH (or PICO_SDK_FETCH_FROM_GIT=ON)"; fail=1
fi
ls /dev/ttyACM* >/dev/null 2>&1 || echo "WARN: no /dev/ttyACM* yet (device not plugged in / BOOTSEL)"
id -nG | tr ' ' '\n' | grep -qx dialout || echo "WARN: user not in 'dialout' group (serial perms)"
[ "$fail" -eq 0 ] && echo "TOOLCHAIN OK" || { echo "TOOLCHAIN INCOMPLETE"; exit 1; }
