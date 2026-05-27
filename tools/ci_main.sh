#!/usr/bin/env bash
# tools/ci_main.sh — run on every merge to main. Keeps main green.
set -euo pipefail
for t in tests/hooks/test_*.sh; do echo "[$t]"; bash "$t" | grep -q '^PASS$' || { echo "HOOK TEST FAIL: $t"; exit 1; }; done
cmake --preset host -DRENDERER_REQUIRE_ORTHODOXY=ON
cmake --build --preset host -j"$(nproc)"
! cmake --build build-host --target orthodoxy_canary 2>/dev/null && echo "orthodoxy canary correctly fails to compile"
ctest --preset host --output-on-failure
# clang-tidy over ALL authored src + tests (warnings-as-errors). Was previously
# unenforced and scoped to template files only, giving false-clean reviews (W1-08).
cmake --build build-host --target tidy
cmake --preset pico
cmake --build --preset pico -j"$(nproc)"
echo "CI-MAIN GREEN"
