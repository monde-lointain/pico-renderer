#!/usr/bin/env bash
# .claude/hooks/task_schema.sh — TaskCreated. exit 2 = reject.
t="$(cat)"
# TW-01: the Task tool exposes caller fields under .task.metadata.*, not at
# .task.* directly, so the original .task.$k read rejected EVERY create. Accept
# either location (metadata first) so tracking via the Task tool actually works.
for k in files_owned branch verification; do
  v="$(echo "$t" | jq -r ".task.metadata.$k // .task.$k // empty")"
  [ -n "$v" ] || { echo "TASK REJECTED: missing .task.metadata.$k (or .task.$k) — set it, or track the stream in WORKFLOW.md" >&2; exit 2; }
done
exit 0
