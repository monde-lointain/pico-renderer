#!/usr/bin/env bash
# Rename the imported template's project identifiers. Robust (while-read, not xargs).
# Usage: tools/rename_project.sh [FROM_lower FROM_UPPER TO_lower TO_UPPER]
set -euo pipefail
FL="${1:-picosystem_template}"; FU="${2:-PICOSYSTEM_TEMPLATE}"
TL="${3:-renderer}";            TU="${4:-RENDERER}"
grep -rl --exclude-dir=.git --exclude-dir=docs --exclude-dir=.deps-cache \
  --exclude-dir=build-host --exclude-dir=build-pico -e "$FL" -e "$FU" . | while read -r f; do
    sed -i -e "s/$FL/$TL/g" -e "s/$FU/$TU/g" "$f"
done
left=$(grep -rl --exclude-dir=.git --exclude-dir=docs -e "$FL" -e "$FU" . | wc -l)
[ "$left" -eq 0 ] && echo "rename OK (0 remaining outside docs)" || { echo "rename INCOMPLETE: $left files"; exit 1; }
