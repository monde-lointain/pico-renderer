#!/usr/bin/env bash
# .claude/hooks/task_schema.sh — TaskCreated. exit 2 = reject.
t="$(cat)"
for k in files_owned branch verification; do
  v="$(echo "$t" | jq -r ".task.$k // empty")"
  [ -n "$v" ] || { echo "TASK REJECTED: missing .task.$k" >&2; exit 2; }
done
exit 0
