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

## Stream B.0 — `fixed` math (subagent-in-worktree, 2026-05-27)

Landed Q16.16 mul/div/conv + mat4 + transform + vec3 dot/normalize. 17 host golden tests vs float
oracle; arm-clean. Three small frictions worth surfacing:

### Tier 2 — friction
1. **`make format-patch` is repo-global, not lane-scoped.** It globs all of `src/`+`tests/`, so a
   pre-existing format violation in a *non-owned* skeleton (`tests/geom/geom_test.cc` etc.) makes the
   gate red for a lane that touched none of it. A subagent can't fix it without crossing ownership.
   → Suggest a lane-scoped variant (`make format-patch FILES=...` or gate per-owned-glob) so a stream's
   format gate reflects only its own files. I verified my owned files with a direct
   `clang-format --dry-run --Werror <owned files>` instead.
2. **`hardware/divider.h` isn't on the fixed lib's arm include path** (the target only carries
   `src/`), so an explicit `hw_divider_*` call fails to compile for pico without adding a pico-sdk dep
   to my CMakeLists. Resolved *better* by dropping the explicit include: the RP2040 AEABI maps C `/`
   onto the SIO divider (datasheet 2.3.1.5), so plain integer `/` already uses the hardware divider on
   device and is bit-identical to the host `/`. Net: simpler, no SDK coupling, same numerics.
   → Guidance idea: for `fixed`, prefer C `/` (AEABI→SIO) over the explicit `hw_divider_*` API unless
   an *overlapped/async* divide is actually needed; document this in the `fixed-point` skill.

### Tier 3 — confirmations
3. The no-UMULL discipline is verifiable post-build: `arm-none-eabi-objdump -d` on the lib object
   showed 0 UMULL/SMULL/MLA, 81 MULS — a cheap, objective gate. Worth adding to the `fixed` review
   checklist (assert no 64-bit-multiply opcodes in the arm object).

---

# How this log becomes improvement (the closed loop)

A journal of symptoms doesn't improve anything. From here, every finding is a **tracked backlog
item**, not prose, and no item is *closed* until something **executable** would catch its
regression (a hook / CI assert / validation step). The A10 pivot proved the cost of skipping this:
it concluded "subagents isolate" and shipped a *doc-only* change with no check — and that exact
assumption broke again on the first real dispatch (W1-01).

**Record format** (one row per finding): `ID · symptom · root-cause · severity · → artifact
(which agent def / skill / hook / CMake / plan section) · owner · status{open|shipped|verified|wontfix}`.

**Two cadences:**
- **Hot-fix** — finding blocks the next action → fix *before* the next dispatch.
- **Batch** — friction/nits → collected to the **wave-retro barrier** (Stream D + `wave-retro` skill),
  triaged, high-ROI ones shipped.

**KPIs (feedback signal).** Existing: contract-deltas, CI-red-after-merge, on-target-gate-pass.
Added: **review-catch rate** (blockers caught at gate vs escaped to `main`) and **dispatch-rework
rate** (streams needing re-dispatch).

## Wave-1 improvement backlog

| ID | Symptom | Root cause | Sev | → Artifact | Owner | Status |
|----|---------|-----------|-----|-----------|-------|--------|
| **W1-01** | `isolation:worktree` defeated: both subagents ran concurrently in the shared main tree; `infra/harness` ended up stacked on `impl/fixed` | Agents `cd` to the abs project path (and the `worktree-pr` skill told them to `git worktree add` manually) → abandon the harness worktree → share the main tree | **High (blocks fan-out)** | `worktree-pr` (CWD-discipline block); dispatch loop (serial-until-proven + spawn-prompt CWD mandate + gate runs the guard); **new `tools/verify_stream_branch.sh`** (lane-scope ⇒ catches stacking) | Lead | **verified** — guard reproduces the catch on `infra/harness` (flags foreign `src/fixed/*`, accepts `tests/harness/*`) |
| W1-02 | `geom` must map GBI `Vp_t`→`SET_VIEWPORT{x,y,w,h}` with the oracle's center/extent convention or every golden shifts | Oracle viewport interpretation not pinned in a shared note | Med | `tests/harness/oracle.h` note + geom integration | T5/T1 | open (batch) |
| W1-03 | `OTVtx` carries no clip-space Z; δ/blend need a depth value, no seam | Oracle struct omits NDC-Z (w-buffer is `inv_w`, but z-compare path unseeded) | Med | `tests/harness/oracle.{h,cc}` | T5 | open (batch) |
| W1-04 | `CommittedSceneRegression` soft-passes if the golden file is deleted (`GOLDEN_WROTE` passes) | Test asserts `!= FAIL` instead of `== PASS` for a committed anchor | Low | `tests/harness/golden_test.cc` (`EXPECT_EQ PASS` unless `GOLDEN_REGEN`) | T5 | open (batch) |
| W1-05 | Oracle rejects whole back-of-near vertices but can't *clip* a partially-behind tri | No near-plane intersection in the oracle yet | Low | `tests/harness/oracle.cc` (when `clip` stream lands) | T5/T1 | open (batch) |
| W1-06 | repo-global `make format-patch` is red on pre-existing skeleton files, failing a lane that touched none of them | The target globs all of `src/`+`tests/` | Low | `Makefile`/`tools` (lane-scoped variant) + gate uses lane-scoped `clang-format` meanwhile | T5 | open (batch) |

## Cycle KPIs — first dispatch (B.0 `fixed` + B.1-ε harness), 2026-05-27
- **Review-catch rate: 1 caught / 0 escaped.** The gate's reviewer (UBSan) caught a real `fx_to_int(INT32_MIN)` UB blocker the author's 17 tests missed → fixed → 23 tests, ASan/UBSan clean.
- **Dispatch-rework rate: 1 of 2** (B.0 needed one re-dispatch for the blocker + edge tests).
- **CI-red-after-merge: 0** (both merged green via `ci_main.sh`).
- **Contract-deltas: 0** (frozen `rdr/types.h` untouched; `Vec4fx` kept module-local).
- **Verdict:** validating-before-fan-out + the pre-merge gate earned their keep on cycle one. The
  proof-of-loop (dispatch→gate→review→fix→re-gate→merge) is confirmed end-to-end.
