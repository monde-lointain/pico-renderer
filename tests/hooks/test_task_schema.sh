#!/usr/bin/env bash
set -u
H=.claude/hooks/task_schema.sh
ok='{"task":{"files_owned":["src/raster/**"],"branch":"impl/raster","verification":"make test"}}'
bad='{"task":{"branch":"impl/raster"}}'
# TW-01 regression: the Task tool nests caller fields under .task.metadata.* —
# a valid metadata-nested task MUST be accepted (was rejected before the fix).
meta='{"task":{"metadata":{"files_owned":["src/raster/**"],"branch":"impl/raster","verification":"make test"}}}'
a="$(echo "$ok"   | "$H" >/dev/null 2>&1; echo $?)"
b="$(echo "$bad"  | "$H" >/dev/null 2>&1; echo $?)"
c="$(echo "$meta" | "$H" >/dev/null 2>&1; echo $?)"
[ "$a" = "0" ] && [ "$b" = "2" ] && [ "$c" = "0" ] && echo PASS || echo "FAIL (ok=$a bad=$b meta=$c)"
