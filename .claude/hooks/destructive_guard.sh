#!/usr/bin/env bash
# .claude/hooks/destructive_guard.sh — PreToolUse(Bash). exit 2 = block.
cmd="$(jq -r '.tool_input.command // ""')"
deny_re='git[[:space:]]+push([[:space:]].*)?[[:space:]]--force|--force-with-lease|git[[:space:]]+reset[[:space:]]+--hard|git[[:space:]]+branch[[:space:]]+-D|git[[:space:]]+clean[[:space:]]+-[a-z]*f[a-z]*d|git[[:space:]]+push[[:space:]].*:[^[:space:]]*$|rebase[[:space:]]+.*--force|filter-branch'
if echo "$cmd" | grep -Eq 'rm[[:space:]]+-[a-z]*r[a-z]*f' && ! echo "$cmd" | grep -Eq 'build-|/build|\.deps-cache'; then
  echo "BLOCKED: rm -rf outside a build dir" >&2; exit 2
fi
if echo "$cmd" | grep -Eq "$deny_re"; then
  echo "BLOCKED destructive op: $cmd" >&2; exit 2
fi
exit 0
