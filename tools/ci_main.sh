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
# Belt-and-braces (D2-01/D.5): the `tidy` target recipe can false-green when its
# $? captures the preceding build step, not clang-tidy — so capture the log and
# fail on ANY ': error:' line in addition to the command's own exit (pipefail
# already catches a propagated non-zero).
tidy_log="$(mktemp)"
cmake --build build-host --target tidy 2>&1 | tee "$tidy_log"
if grep -q ': error:' "$tidy_log"; then
  echo "TIDY FAIL: clang-tidy emitted ': error:' lines (false-green guard, D2-01)"
  rm -f "$tidy_log"
  exit 1
fi
rm -f "$tidy_log"
cmake --preset pico
cmake --build --preset pico -j"$(nproc)"
echo "CI-MAIN GREEN"
