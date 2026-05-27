#!/usr/bin/env bash
# .claude/hooks/idle_gate.sh — TeammateIdle. exit 2 = keep working (with feedback).
ATT=.git_idle_attempts; n=$(cat "$ATT" 2>/dev/null || echo 0)
# Blocked on an unmerged dependency? Idling is correct — don't loop.
if [ -n "${BLOCKED_ON:-}" ] && ! git merge-base --is-ancestor "origin/${BLOCKED_ON}" HEAD 2>/dev/null \
   && ! git rev-parse --verify --quiet "${BLOCKED_ON}" >/dev/null; then
  echo "Blocked on ${BLOCKED_ON} (unmerged). Idling OK — Lead will unblock." >&2; rm -f "$ATT"; exit 0
fi
SEL=(); [ -n "${MODULE:-}" ] && SEL=(-L "$MODULE")
if ctest --preset host "${SEL[@]}" >/dev/null 2>&1; then rm -f "$ATT"; exit 0; fi
n=$((n+1)); echo "$n" > "$ATT"
if [ "$n" -ge 5 ]; then
  echo "CIRCUIT-BREAKER: ${MODULE:-owned} tests red after $n attempts — escalate to Lead/human." >&2
  rm -f "$ATT"; exit 0
fi
echo "${MODULE:-owned} tests red (attempt $n/5). Keep working; if stuck, message the Lead." >&2; exit 2
