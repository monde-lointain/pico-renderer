---
name: worktree-pr
description: Use to run the central-repo/PR workflow over git worktrees — create the stream's worktree/branch off main, keep it current, open a PR, and merge after review + green CI.
---

# Worktree + PR Workflow

`main` is truth. One worktree/branch per stream.

## Start a stream
```bash
git worktree add ../wt-<mod> -b impl/<mod> main
```
Work only in your owned globs (`.claude/ownership.json`).

## Stay current
`git merge main` into your branch whenever a dependency lands (e.g. when `impl/fixed` or `impl/harness` merges). Frozen headers keep drift small.

## Land the stream
1. Green: Stage-1 (`ctest -L <mod>`) + `make format-patch` clean + Orthodoxy clean.
2. Open a PR (branch diff vs `main` + checklist).
3. `renderer-reviewer` reviews (lane-scoped); address findings.
4. After approval + green `tools/ci_main.sh`: **self-merge** to `main` (owners self-merge; the Lead reserves `main` for contract-deltas + the C2 barrier).
5. Retry transient `packed-refs.lock` failures.

## Never
- Never force-push, hard-reset shared refs, or delete others' branches (the destructive-op guard blocks these). Operate only on your own `impl/*` branch.

## Throwaway spikes (hardware probes, experiments)
A spike gets its **own worktree** (`git worktree add ../wt-spike -b spike/<name> main`), never a
branch-switch in a tree with uncommitted changes — a `checkout` will silently abort and you can
commit to the wrong branch (happened in the foundation retro; see WORKFLOW.md). After ANY
`git checkout`/worktree op, verify `git branch --show-current` before committing. Record only the
results (e.g. measurements) back to the mainline; discard the spike worktree/branch.
