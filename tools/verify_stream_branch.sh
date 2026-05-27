#!/usr/bin/env bash
# tools/verify_stream_branch.sh — Lead pre-merge guard (W1-01 executable guard).
#
# Asserts a returned stream branch is lane-scoped to its owner's globs. This is the
# executable check that closes WORKFLOW.md finding W1-01: when worktree isolation is
# defeated and agents run concurrently in the shared main tree, a branch ends up
# STACKED on another stream's branch. A stacked branch leaks the other lane's files
# into its `diff vs main`, so a lane-scope assertion catches it — and also catches any
# plain cross-lane write. Run by the Lead in the dispatch-loop pre-merge gate, before
# review/merge of every returned stream branch.
#
# Usage: tools/verify_stream_branch.sh <branch> <owner> [base]
#   <owner> = a key in .claude/ownership.json (T1..T5, Lead)
#   [base]  = comparison base (default: main)
set -euo pipefail

BRANCH="${1:?usage: verify_stream_branch.sh <branch> <owner> [base]}"
OWNER="${2:?usage: verify_stream_branch.sh <branch> <owner> [base]}"
BASE="${3:-main}"
ROOT="$(git rev-parse --show-toplevel)"

# Owned globs for OWNER from the ownership manifest.
GLOBS=$(python3 - "$ROOT/.claude/ownership.json" "$OWNER" <<'PY'
import json, sys
owners = json.load(open(sys.argv[1]))["owners"]
key = sys.argv[2]
if key not in owners:
    sys.stderr.write("unknown owner: %s (have: %s)\n" % (key, ", ".join(owners)))
    sys.exit(2)
print("\n".join(owners[key]))
PY
)

# Shared artifacts every stream may touch by mandate (the per-wave retro log).
ALLOW=$'WORKFLOW.md'

FILES=$(git diff --name-only "$BASE..$BRANCH")
if [ -z "$FILES" ]; then
  echo "verify: no changes on $BRANCH vs $BASE (already merged, or empty)"; exit 1
fi

bad=0
while IFS= read -r f; do
  [ -z "$f" ] && continue
  ok=0
  while IFS= read -r a; do [ "$f" = "$a" ] && ok=1; done <<<"$ALLOW"
  while IFS= read -r g; do
    [ -z "$g" ] && continue
    case "$g" in
      */\*\*) prefix="${g%\*\*}"; case "$f" in "$prefix"*) ok=1 ;; esac ;;
      *)      [ "$f" = "$g" ] && ok=1 ;;
    esac
  done <<<"$GLOBS"
  if [ "$ok" -eq 0 ]; then echo "OUT-OF-LANE: $f"; bad=1; fi
done <<<"$FILES"

if [ "$bad" -ne 0 ]; then
  echo "STREAM-BRANCH FAIL: $BRANCH touches files outside $OWNER's lane."
  echo "  -> stacked-on-another-stream (worktree isolation defeated) or cross-lane write. See WORKFLOW.md W1-01."
  exit 1
fi
echo "STREAM-BRANCH OK: $BRANCH lane-scoped to $OWNER ($(echo "$FILES" | grep -c .) files)"
