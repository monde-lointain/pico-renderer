#!/usr/bin/env bash
# .claude/hooks/task_complete_gate.sh — TaskCompleted. exit 2 = block completion.
# Runs in the teammate's own worktree (cwd). Module-scoped (ctest -L $MODULE).
set -uo pipefail
cmake --build --preset host -j"$(nproc)" >/tmp/tc_build.log 2>&1 || {
  echo "GATE FAIL: host build broken:" >&2; tail -20 /tmp/tc_build.log >&2; exit 2; }
if [ -n "${MODULE:-}" ]; then SEL=(-L "$MODULE"); else SEL=(); fi
if ! ctest --preset host "${SEL[@]}" --output-on-failure >/tmp/tc_test.log 2>&1; then
  echo "GATE FAIL: ${MODULE:-all} tests not green:" >&2; tail -20 /tmp/tc_test.log >&2; exit 2
fi
if ! make format-patch >/tmp/tc_fmt.log 2>&1; then
  echo "GATE FAIL: clang-format dirty (run 'make format')" >&2; exit 2
fi
exit 0
