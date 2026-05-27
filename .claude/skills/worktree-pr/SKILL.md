---
name: worktree-pr
description: Use to run the central-repo/PR workflow over git worktrees — create the stream's worktree/branch off main, keep it current, open a PR, and merge after review + green CI.
---

# Worktree + PR Workflow

`main` is truth. One worktree/branch per stream.

## CWD DISCIPLINE — read first if dispatched as a subagent (W1-01)
If you were spawned via the Agent tool with `isolation:worktree`, **the harness already put you
in your own worktree** — do NOT `git worktree add` and do NOT `cd` to an absolute project path.
Doing so abandons your isolated worktree and dumps your work into the *shared main tree*, where
concurrent streams collide and your branch ends up stacked on another stream's (this happened on
the first real dispatch — see WORKFLOW.md W1-01). Instead:
- Work entirely from your starting `$PWD`; use **relative paths**, never `cd /abs/project/path`.
- Before your first commit, assert you're in your worktree, not the shared tree:
  `git rev-parse --show-toplevel` must be your worktree dir, and `git branch --show-current` must
  be your `impl/<mod>` branch — NOT `main`, not the main repo.
- Your diff vs `main` must contain **only your owned globs** (`.claude/ownership.json`); the Lead
  runs `tools/verify_stream_branch.sh <branch> <owner>` and rejects any out-of-lane / stacked branch.

## Start a stream (manual worktree — only when NOT dispatched-with-isolation)
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
