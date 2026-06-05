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
#
# D2-01 false-green guard (house doctrine): a process exiting 0 is NOT proof of a
# tidy pass — a wrapping $? can capture a preceding build step, or a backgrounded
# run be inferred PASS from completion. Assert BOTH on the FINISHED log:
#   (1) the tidy command's OWN exit is 0   (captured directly, not via the pipe), AND
#   (2) grep -c ': error:' == 0            (belt-and-braces: catches a false-green exit).
# `set -o pipefail` is off for this one pipe so $? reflects clang-tidy, not tee.
tidy_log="$(mktemp)"
set +o pipefail
cmake --build build-host --target tidy 2>&1 | tee "$tidy_log"
tidy_rc=${PIPESTATUS[0]}
set -o pipefail
tidy_err=$(grep -c ': error:' "$tidy_log" 2>/dev/null || true); tidy_err=${tidy_err:-0}
if [ "$tidy_rc" -ne 0 ] || [ "$tidy_err" -ne 0 ]; then
  echo "TIDY FAIL (D2-01): exit=$tidy_rc, ': error:' lines=$tidy_err on the FINISHED log"
  rm -f "$tidy_log"
  exit 1
fi
rm -f "$tidy_log"
cmake --preset pico
cmake --build --preset pico -j"$(nproc)"
echo "CI-MAIN GREEN"
