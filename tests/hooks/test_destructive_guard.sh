#!/usr/bin/env bash
set -u
H=.claude/hooks/destructive_guard.sh
run() { echo "{\"tool_input\":{\"command\":\"$1\"}}" | "$H" >/dev/null 2>&1; echo $?; }
fail=0
for c in "git push --force origin main" "git reset --hard HEAD~3" "git branch -D impl/tex" \
         "git clean -fdx" "rm -rf src/"; do
  [ "$(run "$c")" = "2" ] || { echo "NOT BLOCKED: $c"; fail=1; }
done
for c in "git commit -m x" "git merge main" "make test" "rm -rf build-host/CMakeFiles"; do
  [ "$(run "$c")" = "0" ] || { echo "WRONGLY BLOCKED: $c"; fail=1; }
done
[ "$fail" -eq 0 ] && echo PASS || echo FAIL
