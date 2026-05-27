#!/usr/bin/env bash
set -u
H=.claude/hooks/task_schema.sh
ok='{"task":{"files_owned":["src/raster/**"],"branch":"impl/raster","verification":"make test"}}'
bad='{"task":{"branch":"impl/raster"}}'
a="$(echo "$ok"  | "$H" >/dev/null 2>&1; echo $?)"
b="$(echo "$bad" | "$H" >/dev/null 2>&1; echo $?)"
[ "$a" = "0" ] && [ "$b" = "2" ] && echo PASS || echo "FAIL (ok=$a bad=$b)"
