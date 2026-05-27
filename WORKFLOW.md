# WORKFLOW.md — workflow retrospective + improvement log

Living log of friction + improvements, per the continuous-improvement loop. The Lead
collates this at each `wave-retro`. Entries are grounded in actual execution, not hypotheticals.

## Retro: Foundation phase (S0 → Stream A → S0.5) — 2026-05-27

Executed solo by the Lead (no teammates yet). Both targets stayed green throughout; the
contract is frozen on `main`; S0.5 measured 138 KiB free → sort-middle confirmed.

### Applied (Tier 1 — caused real pain)
1. **Firmware must not malloc-probe RAM and must arm a watchdog.** The S0.5 spike used
   malloc-until-fail; pico `malloc` *panics* on OOM, and with no watchdog the panicked
   firmware wedged USB → `picotool` couldn't software-reset → manual BOOTSEL needed.
   → `on-target-probe` skill now mandates **linker-symbol RAM measurement** + **watchdog +
   reset stub on ALL firmware (spikes included)**; added a firmware checklist.
2. **CDC-tty serial access is an upfront S0 prerequisite.** `usermod -aG dialout` doesn't
   bind mid-session; the CDC tty needs its own udev rule (`60-picotool.rules` covers only the
   USB vendor interface). Cost ~3 round-trips when hit at S0.5.
   → Plan A1 now sets the `2e8a`/`plugdev` udev rule in S0 and `verify_toolchain.sh` asserts
   tty readability.
3. **Throwaway spikes get their own worktree.** Branch-switching in the main tree aborted on
   uncommitted spike changes; I didn't check and committed S0.5 results to the wrong branch
   (recovered via cherry-pick). → `worktree-pr` skill: spikes use a dedicated worktree; always
   verify `git branch --show-current` after checkout.

### Applied (Tier 2 — friction)
4. **Compiler/Orthodoxy alignment.** `host` preset uses plain `clang` (18) while presets pin
   clang-22 and `.orthodoxy.yml` is clang-22 schema; Orthodoxy loaded the `/18` plugin.
   → `verify_toolchain.sh` asserts the Orthodoxy plugin version matches the host compiler;
   plan notes the host-preset clang-22 alignment option.
5. **Rename fragility.** `grep -rlZ | xargs -0 sed` mis-split (partial rename). → shipped a
   tested `tools/rename_project.sh` (while-read loop); plan A2 uses it.
6. **Spike benchmark quality.** `MICRORAST` was dead-code-eliminated; everything ran at stock
   clock, not 250 MHz. → probe guidance mandates anti-DCE (volatile/asm barriers) + setting the
   target clock; C3 must run at 250 MHz with real geometry.

### Logged (Tier 3 — minor / confirmations)
7. `settings.json`/`.claude` must exist before the session that relies on hooks (mid-session
   creation may not bind until restart) — confirms the spec's "config binds at next spawn".
8. `.gitignore` append concatenated (no trailing newline) — use newline-safe edits.
9. Per-module scaffolding as one task each (H3) — confirmed; reinforce for the team.

### Keep (worked well)
- Shared `FETCHCONTENT_BASE_DIR` (SDL3 built once).
- The `probe-spec` loop before execution (6 rounds caught platform-ownership, the C1/C2 split,
  the C3 perf probe, autonomous-loop safety) — high ROI.
- Contract-freeze-first + both-targets-green-every-step — zero integration surprises.

### Workflow KPIs (foundation phase)
- Contract-deltas this phase: 0 (within the >2 budget).
- CI-on-main red after merge: 0 (merged green, 61/61).
- On-target gate pass: S0.5 measured after 1 firmware fix + 1 manual BOOTSEL (watchdog gap).

## A10 smoke test -> execution-model pivot (2026-05-27)

Ran the A10 validation (one throwaway agent) before the Wave-1 launch. It caught a model-breaking
assumption — exactly why we validate first.

**Confirmed working:** agent spawning works here; project config loads for spawned agents;
**PreToolUse/PostToolUse hooks fire for spawned agents** (the destructive guard intercepted a forced
branch-delete in both a team member and a subagent).

**Model-breaker:** an **agent-TEAM member runs in the lead's working tree** (it committed straight to
`main`) — no per-teammate worktree isolation. A **subagent with `isolation:worktree` DOES** get its
own worktree+branch. Teams give a shared task list + messaging but no isolation; subagents give
isolation but are one-shot.

**Pivot (approved):** Part D moves from the agent-teams vehicle (H1–H3, shared task list, SendMessage)
to **subagent-driven-in-worktrees** (`superpowers:subagent-driven-development` +
`dispatching-parallel-agents` + `using-git-worktrees`):
- The Lead holds the dependency graph and **dispatches a fresh subagent-in-a-worktree per stream**,
  reviews the returned branch (`renderer-reviewer`), merges to `main`, dispatches the next unblocked
  streams. Barriers are **lead-sequenced**, not task-list-encoded.
- **Hook scope:** PreToolUse (destructive guard) + PostToolUse (format) fire per-edit in subagent
  worktrees. The team-event hooks (`TaskCreated`/`TaskCompleted`/`TeammateIdle`) do **NOT** fire for
  subagents; their gates move to the **lead's pre-merge gate** (module `ctest -L <mod>` + format +
  Orthodoxy + reviewer + `ci_main.sh` green) before merging each stream's branch.
- Modules are independent post-contract-freeze, so the lost messaging/shared-task-list doesn't matter.

**Bonus finding:** the destructive-guard greps the whole Bash command string, so a heredoc containing a
forced-branch-delete *as documentation text* false-positives. Mitigation: author docs with the
Write/Edit tools (not bash heredocs); longer term, narrow the guard to the leading command token.
