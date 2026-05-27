#!/usr/bin/env bash
# .claude/hooks/format_check.sh — PostToolUse(Edit|Write). Advisory (exit 0 always).
f="$(jq -r '.tool_input.file_path // empty')"
case "$f" in *.cc|*.h) clang-format --dry-run --Werror "$f" 2>&1 | sed 's/^/[format] /' ;; esac
exit 0
