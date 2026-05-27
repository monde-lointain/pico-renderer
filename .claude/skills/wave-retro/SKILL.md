---
name: wave-retro
description: Use at the end of a wave — collate per-worktree WORKFLOW.md notes, compute the workflow KPIs, ship agent/skill/hook improvements as the next contract-delta, and draft the next wave's stream topology.
---

# Wave Retro

## Procedure
1. Collate every teammate's `WORKFLOW.md` (from their branches/worktrees) — friction + concrete suggestions.
2. Compute the workflow KPIs:
   - PR cycle time (open→merge)
   - CI-on-main red frequency
   - rework-after-review rate
   - on-target gate pass rate
   - **contract-delta count this wave** (>2 ⇒ the freeze was premature — flag it)
3. Triage suggestions into concrete `.claude/` changes (agents/skills/hooks). Ship them as part of the **next wave's contract-delta PR** (they bind at next team spawn, not live).
4. Prune merged branches + stale worktrees (`git worktree prune`).
5. Draft the next wave: contract-delta (extend `rdr/types.h` + register new subdirs) + the dispatchable stream topology. Save as `docs/superpowers/plans/<date>-wave<N>-*.md`.
