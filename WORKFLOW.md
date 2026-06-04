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
| W1-06 | repo-global `make format-patch` red on pre-existing skeleton/contract files, failing a lane that touched none | The frozen contract headers (`types.h`/`config.h`) were hand-formatted, not clang-format-clean | Low | reformatted the whole tree clean (frozen contract reflow OK'd — nothing implemented against it yet) | Lead | **verified** — repo-global `make format-patch` exit 0 |
| **W1-07** | `clang-tidy --fix` corrupted vendored `imgui.h`/SDL backends AND renamed ImGui/SDL **API call-sites** in `platform_sdl.cc`/`main_sdl.cc` (`ImGui::CreateContext`→`create_context`) | `--fix` writes fixes to **every** file a fix touches, ignoring `HeaderFilterRegex` (which only filters *display*); identifier-naming renames a decl + all its uses, incl. third-party | **High (broke build)** | recovery: restored vendored headers from FetchContent git, reverted glue files; rule logged: **never run `--fix` broadly** — restrict to files that include no third-party headers, or use `-export-fixes` + filter to our tree | Lead | mitigated (recovered; rule documented here) |
| **W1-08** | tidy gave **false-clean reviews** — b0-fixed's review said "clang-tidy: no findings" but `fixed.cc` was never linted | `StaticAnalysis.cmake` scoped tidy to template files only (`game\|gfx\|app`); `.clang-tidy` `HeaderFilterRegex` excluded the renderer modules + tests | **High (hollow gate)** | expanded scope to all `src/`+`tests/` (excl. generated/canary/inspector/pico-only); fixed unanchored `HeaderFilterRegex` matching `*-src/` via `.deps-cache/` exclude; **wired `make tidy` into `ci_main.sh`**; cleared ~529 findings | Lead/T5 | **verified** — `make tidy` exit 0; in `ci_main.sh` |

## Cycle KPIs — first dispatch (B.0 `fixed` + B.1-ε harness), 2026-05-27
- **Review-catch rate: 1 caught / 0 escaped.** The gate's reviewer (UBSan) caught a real `fx_to_int(INT32_MIN)` UB blocker the author's 17 tests missed → fixed → 23 tests, ASan/UBSan clean.
- **Dispatch-rework rate: 1 of 2** (B.0 needed one re-dispatch for the blocker + edge tests).
- **CI-red-after-merge: 0** (both merged green via `ci_main.sh`).
- **Contract-deltas: 0** (frozen `rdr/types.h` untouched; `Vec4fx` kept module-local).
- **Verdict:** validating-before-fan-out + the pre-merge gate earned their keep on cycle one. The
  proof-of-loop (dispatch→gate→review→fix→re-gate→merge) is confirmed end-to-end.

## Wave-1 parallel fan-out (B.1-α/β/γ/δ/ζ/η/θ) — 2026-05-27

After cycle 1, ran the W1-01 **isolation probe** (dispatch one subagent, confirm `git worktree list`
shows a populated worktree on its own branch, not collapsed into the main tree). It PASSED — the
harness pre-creates the worktree at spawn — so W1-01's serial-until-proven hold was cleared and **all
7 remaining streams were dispatched in parallel**, each in its own worktree (per the dispatch graph,
β/γ/δ code against the frozen headers, so all are unblocked once B.0+ε are merged). All 7 returned
green + lane-scoped (`verify_stream_branch.sh` re-run by the Lead on every branch: 0 stacking, 0
cross-lane). All 7 reviewed by `renderer-reviewer` → **APPROVE, 0 blockers**; the one real correctness
coupling (δ `tex` 4444 decode vs ζ asset 4444 emit) was verified consistent (R-high/A-low). Merged 7
branches to `main`; `ci_main.sh` integration gate went red once on clang-tidy, fixed by the Lead, then
green (182 host tests; arm builds).

### Backlog (tracked) — `ID · symptom · root-cause · sev · → artifact · owner · status`

| ID | Symptom | Root cause | Sev | → Artifact | Owner | Status |
|----|---------|-----------|-----|-----------|-------|--------|
| **W1-09** | In a worktree subagent the **`Write`/`Edit` tools don't persist** — report success, but the worktree stays clean and the write lands in the **shared main tree** | Edit/Write path resolution ignores the agent's `$PWD` worktree (resolves to the parent/main tree); `Bash` heredoc/`tee` respect cwd | **High** | dispatch-prompt template + `worktree-pr` skill: **mandate authoring files via `Bash`, not Edit/Write**, until the harness CWD bug is fixed; gate already cleans the main-tree leak via `git merge`'s dirty-tree refusal | Lead | shipped (mandate); deliverables were never corrupted (commits via Bash landed in-worktree) — **verify the harness fix before W2** |
| **W1-10** | 124 clang-tidy errors rode into the merge (const-correctness, uppercase-suffix, isolate-decl, narrowing) — caught only at the integration `ci_main` | The **per-stream gate omitted clang-tidy** (ran ctest+format+Orthodoxy only); W1-08 wired tidy into `ci_main` but not into the stream landing-gate | **High** | add lane-scoped `make tidy` (or `clang-tidy -p build-host/tidy <owned files>`) to the dispatch-prompt gate + the `worktree-pr` landing checklist; `ci_main` is the backstop (it fired) | Lead | shipped to docs; **executable**: ci_main enforces at integration |
| **W1-11** | `clang-tidy --fix` introduced a **compile error** in `clip.cc` (const-correctness made the pointee const, breaking a `dst` assignment) | `--fix` applies fix-its without whole-program type-checking; reinforces W1-07 | Med | rule: after any scoped `--fix`, **re-run tidy + a build** before trusting it; never broad `--fix` (explicit file list only, then verify) | Lead | verified (re-run caught it; repaired with `* const`) |
| **W1-12** | Local `ci_main` was green but remote CI was red on 2 jobs it can't replicate: **windows-msvc** (`arena_test.cc` used GCC `__attribute__((aligned))` → MSVC C2146) and **pico-arm tidy** (`platform_pico.cc` `runway` non-const — host tidy excludes `_pico.cc`) | `ci_main` covers host(clang)+pico-*build* only — not MSVC/AppleClang, nor pico-*tidy* | Med | code: use portable `alignas`/standard C++, never compiler extensions, in cross-target code; **process: a wave is not "landed" until the remote CI matrix is green** (push is part of the gate, not a victory lap) — remote CI is the cross-platform backstop and it fired | Lead | verified (fixed `alignas` + `const`; pico-tidy reproduced+green locally; re-pushed) |

### C1 integration handoff (cross-module contract items — not workflow bugs; for the C1 plan)
- **β `geom_run` ⇄ `Frame`:** frozen `Frame{fb,width,height}` lacks the TVtx pool / tile bins / matrix+viewport+material state / clip scratch → `geom_run` is a stub; C1 must extend `Frame` (or add a `GeomCtx` the entry takes). Module-local POD (`GeomOut`/`MtxStack`/`Viewport`) shaped for C1 to own+pass.
- **γ `raster_tile` signature** (changed in-lane, T2-owned, NOT frozen): `raster_tile(int, const TileBin*, const TVtx* pool, uint16_t* fb, uint16_t* zbuf)` — added `pool`, dropped `const` on fb/zbuf. `sched`/`rdr`/AA callers adopt at C1.
- **γ depth encoding** (unpinned): `clamp(inv_w_q16>>4, 0..0xFFFF)`, clear=0=farthest — δ/blend translucent (z-test-no-write/sort) MUST use the identical encoding. Pin in a shared note.
- **ζ↔δ RGBA4444 order:** R-high/A-low (`R[15:12]G[11:8]B[7:4]A[3:0]`) — agreed by both lanes; **pin it in the spec/`config.h`** (config.h notes 4444's low nibble as coverage — reconcile alpha-vs-coverage there).
- **ζ `Vtx` size:** real POD size is **14 B**; the `types.h` comment says 16 B — fix the comment + add `static_assert(sizeof(struct Vtx)==14)` (Stream-A/contract touch).
- **ζ UV:** stored S10.5 of raw [0,1], NOT pre-scaled by tex size — the sampler/transform multiplies by tex dim. Confirm whose job at C1.
- **δ alpha dropped end-to-end** (tex discards 4444 alpha; `shade` forces keep=1): the spec'd C1 demo's **alpha-cutout billboard cannot work** — C1 must descope it or plumb alpha through tex→shade.
- **θ watermark producer unwired:** `s_committed_rows` only advanced by `plat_present_watermark`, which nothing calls → device takes the full-frame fallback; the scanline-race chase/stall path is unexercised on HW until the scheduler (C1/C2) drives it. Add a stall **deadline** (busy-wait has no escape today).
- **α `cb_walk`** has no stream-length bound (relies on a terminating `CMD_END`) — the geom front-end must END-terminate every DL; **arena backing buffers must be 8B-aligned** on target (`arena_init` aligns offsets, not `base`).

#### C2 / downstream latents surfaced at C1 integration (review follow-up, 2026-05-27)
These are cross-module semantic items the C1 thin slice deferred — none block C1, all must be pinned before the named downstream consumer.
- **#1 vertex-window base convention (C2):** DRAW_TRIS indices are ABSOLUTE into the current LOAD_VERTS window in C1 (base==0 only; a nonzero `load_verts.base` is rejected at LOAD_VERTS so a windowed stream cannot silently misindex). Absolute-vs-base-relative MUST be pinned before any multi-LOAD_VERTS / windowed stream lands.
- **#2 `TriRef.material` (C2, blocks blend/tex):** C1 writes `0` (placeholder). The frozen `TriRef.material` field MUST carry a real render-state version/intern id before the blend/tex streams read it (they key material lookups off it). C1 previously passed `combiner.mode` — wrong (a combiner enum, not a state id); fixed to 0.
- **#3 `tris_total` counts post-clip fan triangles (C2/telemetry):** the counter increments per binned fan triangle, not per source triangle, so a guard-band-clipped source tri over-counts across the band. Either document this or split a separate source-tri counter before the telemetry/`FRAME_US` numbers are treated as source-poly counts.
- **#4 `det_sign` from `modelview[mv_top]` only (C2):** backface winding folds in only the modelview determinant sign — a mirror placed in the view/projection slot will NOT flip winding. Pin the "no mirror in the projection slot" precondition, or compute the sign from the composed MVP, before any mirrored-projection scene.
- **#6 transform range budget (downstream/any scene):** the integer Q16.16 transform path requires |coord| < 2^15 through MVP (`fx_mul` wraps out of domain). The demo uses SCENE_SCALE 0.06 to stay in budget; any new scene must respect it. Consider a debug range-assert in `geom_transform_clip`/`mtx_mvp`.

#### C2 integration result + new latents (dual-core back-end raster, merged 3638c7c, 2026-05-27)
Scope: back-end raster only (FE single-core; no scanline-race). `sched_rasterize` frozen → `sched_dispatch_tiles` link-time split (host serial / pico dual-core). Bit-identical invariant host-proven (`tests/sched`: serial==2-worker==frozen-seam CRC; shared-zbuf teeth). Device: `workers=2`, determinism 25/25 frames across 2 cold boots, visual-confirmed. Latents #1–#4 above all UNTOUCHED (FE/geom concerns, out of back-end-raster scope) — still open for their consumers.
- **C2-A — dual-core gives NO speedup at low tri count (overhead-bound):** demo = 6 accepted tris / 16 tiles → ~52ms vs C1 ~49ms (core1 wake/FIFO-join/spinlock > the parallel win; bottleneck is serial geom + present). **Not a defect** — the parallel raster's payoff lives at ~3000 tris. → **Artifact: the dual-core speedup MUST be measured at the C3 throughput probe / W5 perf gate under a real load**, not claimed from the thin demo. A heavier on-device scene is the vehicle. Sev: Med (perf claim unproven). Status: tracked → C3.
- **C2-B — multicore correctness can't be golden-tested on the single-thread host:** mitigated by (a) `tests/sched` host model of order-independence + per-worker-zbuf isolation, and (b) on-device run-to-run `fb_crc` determinism (25/25 across boots). Rule for future multicore streams: pair a host order-independence model with an on-device reproducibility capture; neither alone is sufficient.
- **C2-C (nit) — demo per-frame full-FB `fb_crc32` is telemetry-only:** computed AFTER the `frame_ms` window (doesn't inflate frame_ms) but adds wall-clock. Gate behind a build flag for a production demo. Sev: Low.

### Batch fast-follows (to wave-retro)
- β `clip` lerps packed-565 rgba as a scalar → carry-bleed across R/G/B; harmless for off-screen guard-band clip, **must be per-channel before near-plane clipping lands** (later wave).
- β `geom_modelview_det_sign` overflows `fx_mul` at uniform scale ≳180 → flips det sign → inverts cull. Use a 64-bit product or document the operand-range bound.
- θ per-band DMA re-arming vs the continuous-stream 14.75 ms budget — confirm 30 fps at C2.
- η `main()`/exit-code wiring untested (core is); `read_until_sentinel` tested but production re-implements it inline (drift risk); key-charset asymmetry parse vs check-spec.
- δ `tex_sample` has no non-pow2 guard (documented pow2-only) — add a debug assert.

## Cycle KPIs — second dispatch (Wave-1 parallel fan-out, 7 streams), 2026-05-27
- **Review-catch rate: 0 escaped blockers** (7/7 APPROVE; reviewers independently rebuilt/retested per W1-09, confirmed the 4444 coupling, and surfaced the latents above as tracked items rather than blockers).
- **Dispatch-rework rate: 0 of 7** (no stream needed re-dispatch; the only post-merge fix was the Lead clang-tidy pass — a gate-scope gap, W1-10, not a stream defect).
- **CI-red-after-merge: caught-before-push** — `ci_main` went red on tidy at integration and was fixed before any push; main never pushed red.
- **Contract-deltas (frozen `rdr/types.h`): 0** — every module change stayed in module-owned headers; the `Frame`/`Vtx` items are deferred C1 deltas, not made in Wave-1.
- **Verdict:** parallel fan-out works behind the lane-scope guard + per-stream review + the `ci_main` integration backstop. The two process gaps (W1-09 tool bug, W1-10 tidy-not-in-stream-gate) are the cycle's real lessons; both now have an artifact.

## Terrain-port wave — S0 spike investigation half (2026-06-02)

Plan: `~/.claude/plans/design-a-modern-command-buffer-fizzy-candy.md` (approved; faithful N64 `terrain` port, profiling-first, 1:1 aspect, 2D-blit sky, scripted+free-fly camera). Read-only N64-source findings (resolve plan Q1/Q2/Q5/Q15/Q22):

- **Q22 / P3-3 RESOLVED (R-lane order HOLDS).** `src/main/scenery.c`: tree combine `gDPSetCombineLERP(TEXEL0,0,SHADE,0, …, TEXEL0,…)` → color=TEXEL0×SHADE, **alpha=TEXEL0** (texel's own). Opaque pass `G_RM_AA_ZB_TEX_EDGE` (cutout=coverage from texel alpha), XLU pass `G_RM_AA_ZB_XLU_SURF`. Blend-alpha = **texel alpha**, NOT a coverage-derived continuous alpha → **R.2 (XLU) does NOT depend on R.3-AA**; keep order R.1→R.3-fog→R.2→R.3-AA→R.4. AA only softens silhouettes atop.
- **Q15 / N3 RESOLVED (endian risk localized).** Grid/scenery = typed `Vtx`/`u16` C initializers → parse VALUES, **no swap**. `g_terrain_tex` = `u16` hex array (5551 values) → decode 5551, no swap. **Only the panorama TLUT** is "big-endian u16 in a u8 blob" (`src/assets/panorama.c` comment) → **swap just the TLUT** (256×u16). CI8 image = u8 indices, no swap.
- **Q5 RESOLVED.** `src/main/camera.c` is float (s_az_rad/elev, yaw/pitch, FOVY). → **bake the scripted path's keyframe V*P matrices offline to Q16.16** (deterministic, zero device float, stable fb_crc); free-fly = separate non-profiled mode.
- **Q2 strengthened.** `terrain.bs1` = 4.3 MB "BST1 v145" command-stream capture (potential ground truth). `test/` ships `sky_golden.inc` + `fixtures/{cam,scenery,sky_bg,sky_emit,terrain}_*.h` + host tests → real reference data, not just inspection. **angrylion-rdp-plus (RDP emulator) + terrain.bs1 → candidate bit-reference oracle** (see refs below).
- **Q1 candidate pre-scale ≈ 0.005.** Grid X 0..4096 (512 step), Z to ~16 K (33 rows), Y −308..−456; UV S10.5 to 32767; camera 7330, far 32007. β-latent det-overflow cliff ~180 → 0.005 puts working coords ~80–160. **On-target spike confirms no-overflow + visible.**

**N64 reference repos (provided 2026-06-02) — map to streams:**
- `~/development/repos/angrylion-rdp-plus/` (GPL RDP emulator) → **acceptance-bar oracle (Q2)**: run `terrain.bs1` → ground-truth frames to diff (within tol, accounting for our dropped VI-gamma P4-4); also the AA-coverage reference (R.3-AA), as the plan's licensing note anticipated.
- `~/development/repos/f3dex2/` (microcode) → exact gSPVertex/matrix + clip + combine semantics for the front-end + 2-cycle combine (R.4 / geom).
- `~/development/repos/libultra_modern/` + `~/development/repos/n64sdkmod/packages/libnusys/` → GBI macro/render-mode definitions (G_RM_AA_ZB_*, gDPSetCombineLERP muxes) for faithful tex/shade/blend (D.2/D.3/R.*).

**S0 on-target spike — COMPLETE (2026-06-02, branch `spike/terrain-s0`, throwaway/discarded, base 73ffed0 untouched). LOCKED:**
- **Pre-scale = 0.005** (C1 clear). OVERFLOW=0 at 0.003/0.005/0.008; device fixed-point matches float MVP within 3.4e-5; worst product |A·B|≈11 (cliff 2^15), matrix-build dominant term ~40. **Converter MUST bake MVP column-major as `m[i*4+j]=VP[i][j]`** (the transpose; the other direction is a silent wrong-w bug the spike hit+fixed).
- **C2 REFRAMED — gate is bilinear ARITHMETIC, not flash bandwidth.** Flash bilinear **198 cyc/px** vs SRAM 197.3 (**0.36% diff**, XIP hit **99.99%**: CTR_HIT 5,538,882/CTR_ACC 5,539,710; note ctr_acc=0x10 not 0x08). Per-tile working set (~306 lines) fits the 2048-line cache → bandwidth free at per-tile scale. **Non-pow2 `%` wrap = +47 cyc/px → use POW2 mask-wrap (pow2 atlas/tiles) → ~151 cyc/px floor.** FPS (dual-core 16.5 Mcyc/frame): 57k px→22.6ms/44fps; 72k→28.5ms/35fps; 86k→34.1ms/29fps. **30 FPS plausible (tight only at worst-case full terrain fill).** Arithmetic levers (point/affine/DDA + pow2) are the path; **recompress/shrink does NOT help** (kills Q4). Remaining bandwidth unknown = cross-tile thrash across the 128 tiles → atlas-vs-per-tile (Q3/Q20).
- **5551→565 decode validated:** 0x538b→0x5385 (device==baked==hand-calc; layout `RRRRRGGGGGBBBBBA`).
- **Q2/Q26 CLOSED (negative):** `terrain.bs1` = BST1 v145 asset bundle (not a cmd/frame capture); `test/` goldens = RDP cmd-words (self-validation), NOT console pixels. **No rendered-frame ground truth exists** → acceptance bar stays inspection + own float-oracle (I1), capped by dropped VI-gamma (P4-4). angrylion = CODE reference only (combiner/AA math), not frame-diff.
- **N1:** front terrain row projects to NDC y≈1.09 → scripted path stays off the near plane for T0; **near-clip stays deferred.**
- Host gotcha: device re-enumerates ttyACM0↔1 across resets → reader must glob newest `/dev/ttyACM*` + reopen-on-error. FREE_RAM (spike, no FB/pools) 251,240 B.

## Terrain-wave friction backlog (2026-06-02)

Format: ID · symptom · root-cause · sev · → artifact · owner · status. Closed-loop rule: no item `verified` until something executable catches its regression.

| ID | Symptom | Root cause | Sev | → Artifact | Owner | Status |
|----|---------|-----------|-----|-----------|-------|--------|
| **TW-03** | On-target serial reader latched a stale/dead `ttyACM0`; truncated captures when the device re-enumerated mid-stream | `wait_for_tty` returned `sorted()[0]` (oldest); `read_lines_from_tty` opened once, no reopen | Med | `tools/run_on_target.py`: `newest_tty()` (mtime) + reopen-on-error loop; **guard:** 3 `newest_tty` unit tests inject `_glob`/`_mtime` | Lead | **verified** — `run_on_target_test.py` 20/20 |
| **TW-05** | Silent wrong-w MVP bug (the spike hit it): camera/asset MVP baked transposed | renderer reads column-major `m[k*4+row]`; the row-vector→flat bake convention `m[i*4+j]=vp[i][j]` was unwritten | High | `tools/asset_convert.py`: `bake_mvp_q16_16` + ref transforms; **guard:** `asset_tool_test` asserts bake→renderer-model == row-vec ref, transpose diverges (teeth) | Lead | **verified** — `asset_tool_test.py` 18/18; D.1/D.7 MUST bake via this helper |
| **TW-01** | Every `TaskCreate` rejected — can't use the Task tool for session tracking | `task_schema.sh` reads `.task.{files_owned,branch,verification}`; tool only exposes `metadata`, which nests at `.task.metadata.*` | Low | `.claude/hooks/task_schema.sh` read `.task.metadata.*`, OR reject-msg says "track in WORKFLOW.md" | Lead | open |
| **TW-02** | Throwaway spike branches (`spike/terrain-s0`, `spike/hw-probe`) accumulate — `-d` refuses unmerged, guard blocks `-D` (2nd occurrence) | `destructive_guard` greps `-D`; no sanctioned spike-discard path | Low | `tools/discard_spike.sh` (or `spike/*` allowlist in the guard) | Lead | open |
| **TW-04** | Perf-probe validity is tribal (S0 got it right unprompted; prompt's `CTR_ACC=0x08` was wrong, real 0x10) | No codified probe-validity checklist | Med | fold checklist into `on-target-probe` skill / `perf-profiler` agent: Release/`-O3`/`NDEBUG`, hot code in SRAM, `volatile` sink vs DCE, confirm `SYSCLK`, SDK struct for HW regs (not hardcoded offsets) | infra | open |
| **TW-06** | Spike findings die with the discarded branch unless they reach the consuming stream | No spike→consumer handoff rule | Med | dispatch-template rule: consuming stream's prompt cites the gating spike's locked outputs + carries them as a guard (S0→D.1/D.2 carry pre-scale 0.005, transpose, 5551, pow2) | Lead | open |
| **TW-07** | `frame.h`/`types.h` wanted by multiple streams (D.4/D.5/R.3-AA) → contention | Shared layout headers have no ownership rule | Med | `worktree-pr` skill: layout headers are Lead-owned micro-barriers (Δ/Φ), never stream-owned; land at quiesced points | Lead | shipped (Φ done this wave) |
| **TW-08** | "0 contract-deltas" KPI no longer fits (wave spends Δ3/D1/D2/D4) | KPI was a foundation-phase invariant | Low | KPI evolves → "deltas budgeted + barrier-scheduled + each justified" (see below) | Lead | shipped (KPI note) |
| **TW-09** | Two "tile" meanings (N64 mesh-cell vs 60×60 screen-tile) → subagent cross-talk | No glossary in dispatch prompts | Low | dispatch-template glossary line: "mesh-cell" vs "screen-tile" | Lead | open |
| **TW-10** | Plan lives in `~/.claude/plans/` → not in repo history/CI | Plans authored outside the tree | Low | commit significant plans into `docs/` | Lead | open |
| **TW-11** | `memcmp`-dedup of a mixed-size POD (`RenderState`) trips `bugprone-suspicious-memory-comparison` (padding has no unique object representation) | Value-compare of a struct with internal/tail padding | Low | D.4 keeps `memcmp` (sound under the renderer-wide invariant: every RenderState born from `memset`-0 + field-assign, `SET_MATERIAL` full-copies a zeroed origin → padding uniformly 0) + a single-line `NOLINTNEXTLINE` per site; **guard:** dedup tests assert value-equal-but-distinct-buffer → same id, so a real padding divergence fails green | D.4 | verified — `geom_test` 18/18 |

## Wave-D D.4 note — material interning (2026-06-03)
- **Dedup = linear VALUE compare (`memcmp`) over `frame->rstate_table`, NOT pointer identity.** Pointer identity is unusable: `geom_visit` copies `SET_MATERIAL` state into the single `f->rstate` buffer, so every intern sees the same pointer. Linear scan is ample (cap 16, scene ≈6 distinct states); no hash needed.
- **Lazy intern at draw time, cached + dirty-flagged** (`GeomCtx.cur_material_id`/`material_dirty`, set by the 4 rstate-mutating cmds). Re-interns once per state change, not per triangle → no per-tri scan; interns only states that emit geometry.
- **Overflow = drop-with-count clamp** (`geom_material_overflow_count()`), mirroring the bin/cmd convention: full table → untouched, counter bumped, returns last valid id (in-range, never corrupts a TriRef).
- **Cap concern for the real scene:** frame.h pins ≈6 distinct RenderStates; cap 16 comfortable. The 128 terrain mesh-cells SHARE one RenderState, differing only by texture pointer — that per-cell texture is a SEPARATE axis (later concern), deliberately NOT interned as 128 materials. If a future scene's distinct-RenderState count nears 16, RE-SURFACE (raise `RDR_MAX_MATERIALS` — a frame.h/Lead delta).
- **Reset ownership:** `geom_run` calls `geom_material_reset(f)` at frame start (geom owns interning) rather than depend on `rdr_begin_frame` (forbidden file) to clear `rstate_count` — self-contained + testable without an rdr.cc change.

### KPI evolution (foundation → feature waves)
- **Contract-deltas: was "0"; now "budgeted + barrier-scheduled + justified."** Terrain wave budget = Δ3 (done), D1/D2/D4 (planned); each lands at a quiesced Lead barrier with a one-line justification. Drift (an un-budgeted delta) is the regression signal.
- **Probe-before-execute ROI (log):** 4 `/probe-spec` rounds caught C1 (transform overflow), the C2 bandwidth→arithmetic reframe, the atlas/material-cap conflict, the present-tax — all pre-execution; S0-before-asset-pipeline reframed C2. "Measure the load-bearing unknown first" earns its keep again.

## Wave-D D.2 — tex formats + bilinear (2026-06-03, branch `waveD/tex-bilinear`)
- **Shipped:** all 9 formats decode to RGBA8 (RGBA565/4444/5551, IA8/IA4, I8/I4, CI4/CI8+TLUT); `tex_sample_rgba` (RGBA8) + `tex_sample` (565, now a thin wrapper over the RGBA8 core); `tex_validate` rejects bilinear-on-CI + CI-without-TLUT + unknown-format; N64 3-tap triangular bilerp (NOT 2×2 box). 25 tex tests (7 prior + 18 new) green; oracle cross-check 9 fmt × 3 wrap × 100-coord sweep + 3-pt corner/triangle checks.
- **3-point ambiguity (re-surface trigger) RESOLVED from refs, no escalation:** angrylion `tex.c texture_pipeline_cycle` + `tmem.c fetch_texel_quadro` settle it — true 3-tap: `upper=(sfrac+tfrac)&0x20` picks lower (t0,t1,t2) vs upper (t3,t2,t1) diagonal triangle; frac is **5-bit** (0x20=1.0); `+0x10>>5` rounds. Quad layout t0=(s,t) t1=(s+1,t) t2=(s,t+1) t3=(s+1,t+1). All integer → ARMv6-M MULS-safe (frac 0..31 × diff ±255 fits int32, no UMULL).
- **5551 decode (S0 trigger) within tol:** matches S0's locked `RRRRRGGGGGBBBBBA`; 5-bit→8-bit uses the angrylion replicated-table value `(v<<3)|(v>>2)` (tmem.c:2063), 1-bit alpha→0xff/0x00. Bit-identical to oracle (tol 0), no drift.
- **TLUT format decision (no types.h field exists):** TLUT entries decoded as **RGBA5551** (the N64 CI default + the Δ3 "panorama TLUT entries" note). If a future asset needs IA16 TLUTs, that's a types.h `tlut_format` field = Lead re-surface.
- **R.1 handoff (caller of tex_sample):** (1) call `tex_validate(&tex)` at SET_MATERIAL; RDR_EINVAL = bad material (don't sample). (2) Coords are **Q16.16 texel space**; R.1 must convert its u*inv_w / v*inv_w (S10.5 in `Vtx`) to Q16.16 texel units before calling. (3) `tex_sample` drops alpha (565); use `tex_sample_rgba` if the combiner needs texel alpha (TEX_EDGE cutout / XLU per R-lane Q22). (4) CI is point-only even if `filter==THREE_POINT` slips through (sampler force-falls-back; validate() is the real gate). (5) **pow2 dims required** for WRAP/MIRROR (mask-wrap; S0 non-pow2 = +47 cyc/px) — no runtime guard yet (carries fast-follow δ from C1 backlog).
- **Friction:** `make tidy` shim returns the *build* step's exit, not the `tidy` target's — a green-looking exit 0 masked real `readability-math-missing-parentheses` + `readability-uppercase-literal-suffix` findings (`a + b*c` mixed precedence; lowercase `0x..u`). Caught only by grepping the log for `error:`. → guard below.

| ID | Symptom | Root cause | Sev | → Artifact | Owner | Status |
|----|---------|-----------|-----|-----------|-------|--------|
| **D2-01** | `make tidy` reported exit 0 while the `tidy` target had real findings (false-green) | a wrapping `$?` capture saw the preceding successful `cmake --build` step, not the failing `--target tidy` step | Med | `make tidy`: chain build→target so the recipe's own exit reflects the target; CI greps the tidy log for `error:` as belt-and-braces | infra | open |
## Wave-D D.3 friction (blend + fog lerp) — 2026-06-03

| ID | Symptom | Root cause | Sev | → Artifact | Owner | Status |
|----|---------|-----------|-----|-----------|-------|--------|
| **D3-1** | Spec's frozen `blend_pixel(mode, src, dst)` has NO source-alpha arg, but alpha-over needs one (XLU = texel alpha) | Foundation header was opaque-only; spec sketch never carried alpha | Low | added `blend_pixel_alpha(mode, src, src_alpha, dst)` in the stream-owned blend.h (no types.h delta needed — BlendMode lives in blend.h); kept `blend_pixel` for the opaque path | D.3 | resolved in-stream |
| **D3-2** | `make tidy` flags the harness float oracle (`lrintf` long→float narrowing) even though harness is orthodoxy carve-out | tidy target's `-warnings-as-errors` covers all user TUs incl. harness; carve-out only skips orthodoxy_enforce, not clang-tidy | Med | keep oracle clamp value as `long` (clamp in int domain, cast once at store); **guard:** none codified — dispatch note: harness TUs still pass `make tidy`, write them tidy-clean | D.3 | open (note) |
| **D3-3** | `make tidy` flags test-file `kSamples565` (wants `K_SAMPLES565`) — naming rule applies to tests too | GlobalConstantCase=UPPER_CASE enforced project-wide; tests aren't exempt | Low | follow `K_…` for file-scope `static const` in tests (matches K_ROW565/K_COPY_PIXELS); dispatch prompts could state this once | D.3 | open (note) |
| **D3-4** | XLU sequencing: does the translucent pass blend on coverage or texel alpha? (R.2 vs R.3 ordering) | Ambiguity between coverage-AA and combiner alpha | Med | confirmed from N64 src: tree combiner alpha = TEXEL0 → XLU blends on **texel alpha, not coverage** → **R.2 does NOT depend on R.3**; blend_pixel_alpha takes a source-alpha arg, agnostic to its origin | D.3 | resolved (finding) |
## Wave-D D.5 (blit2d — NEW 2D sky background) — 2026-06-03 (branch `waveD/blit2d`)

- **Sampling = approach (b)** (self-contained CI8+RGBA5551-TLUT + I8 decode), NOT tex_sample. `tex.h` on main is RGBA565/4444-only (no CI/I/3-point) — reusing it was impossible today. Kept the decode to two tiny leaf fns (`blit2d_decode_ci8/_i8`) with a marked **TODO(T3): swap to tex_sample once it grows CI8/I8 (point filter).** T3 coupling = one-line swap per decode call site in `blit2d.cc`; signature parity needed: `tex_sample(TexDesc*, fx16_16 u, fx16_16 v, lod)` would have to honor `TEXFMT_CI8`+`tlut` and `TEXFMT_I8` and return RGB565 (CI) / intensity (I) — currently it doesn't, so do NOT block D.5 on D.2.
- **Blit cyc/px estimate (host-reasoned, NOT yet on-silicon — re-surface trigger if T3 measures worse):** panorama per px = CI8 index fetch (1 flash byte) + `% src_w` (non-pow2 ⇒ the +47-cyc tax S0/TW-04 flagged) + 5551→565 decode (~6 ALU ops). Rough order ~**40-60 cyc/px** at point sampling (no bilinear) — heavier than a plain memcpy but far under the bilinear 198 cyc/px floor. Full-frame 240×240 = 57,600 px ⇒ ~**2.3-3.5 Mcyc ≈ 0.18-0.27 ms @ 13 Mcyc-half-frame budget** → matches the ~0.23 ms re-estimate. **Lever for T3 if tight: make src_w a power of two** so the horizontal wrap is a mask (`& (src_w-1)`) not a `%` — drops the +47-cyc tax (same lesson as the texture wrap in C2). Clouds is cheaper (no flash TLUT indirection; I8 byte + 3× alpha-over MAC).
- **1:1 horizon mapping (T3 hand-off):** seam-freedom is delegated to ONE helper, `blit2d_horizon_row_1to1(src_row, src_h, dst_h)` = rounded vertical rescale. **No frame.h/rdr.cc change was needed** (re-surface trigger NOT hit): the 3D pass and the blit must both query this same helper with the same args so they agree on the horizon row. At the shipping 240-tall source → 240 panel it is identity; the helper exists so a 320×240-authored source (or a sub-band `dst_h`) still lands the horizon on the row the terrain expects. The blit itself does NOT enforce the seam — T3 wires the shared horizon row into both the Blit2dRect list and the 3D viewport; the `SeamFreeAt1to1Panel` test guards the helper's contract.
- **Friction:** the parent `src/CMakeLists.txt` says "never edit this file again" but a genuinely NEW module (blit2d) is not in Stream-A's pre-registered subdir list — so a one-line `add_subdirectory` edit is unavoidable. Pre-register a small pool of spare module slots, OR make the rule explicitly "edit only to add your own NEW subdir line." Sev Low → dispatch-template / Stream-A scaffold note.
- **Friction (Med, gate hygiene): a backgrounded `make tidy` reported a FALSE-POSITIVE PASS.** First pass I launched `make tidy` as a background task whose stdout went to a harness file I never saw the exit code of; I inferred PASS from the *process exiting*. It was actually failing 50× `misc-const-correctness` + 40× `readability-braces-around-statements` + 25× `readability-math-missing-parentheses` + 13× `readability-isolate-declaration` on blit2d.cc + blit2d_test.cc the whole time. Review caught it. **Root cause: "tidy exited" ≠ "tidy passed" — only exit code 0 + zero `error:` lines is PASS.** Guard/lesson: ALWAYS capture `make tidy` to a file and assert BOTH `echo TIDY_EXIT=$?` == 0 AND `grep -c ': error:' log` == 0 before claiming the tidy gate; never infer from process completion. House tidy style for new modules (so red→green is one pass, not two): east-`const` every non-reassigned local, parenthesize mixed `*`/`/` against `+`/`-`, brace ALL `if`/`for` bodies, one declaration per statement (`readability-isolate-declaration`). → fold into `orthodox-tdd-cycle` / `renderer-module-scaffold` skill checklist.
## D.7 (scene + camera scaffold) — 2026-06-03 (branch `waveD/scene-camera`)

App-side of the T0 "real-density geometry + deterministic camera" milestone. Self-contained placeholder terrain (no dep on D.1's generated assets). No re-surface triggers hit.

- **placeholder→D.1 swap seam is clean.** All geometry is behind two boundary fns (`demo_terrain_geometry` / `demo_tree_geometry`); `demo_terrain_build` is geometry-agnostic (single LOAD_VERTS+DRAW_TRIS batch per mesh). T0 swaps the two fns for D.1's baked mesh; nothing else moves. **Watch at T0:** the placeholder authors int16 model coords directly in pre-scaled world units (modelview=identity) — D.1's mesh must arrive in the SAME pre-scaled frame (×0.005) or re-bake the model matrix in the seam.
- **near-plane margin (confirms N1):** scripted keyframe loop = orbit of the field center at the N64 pose's XZ radius (~40 u) / height |N64_EYE_Y|, all keyframes look at origin → whole field stays at positive view-z for the entire loop. Measured over 480 frames: accepted tris ∈ [505,547], **0 near-rejects**, perspective near=0.5/far=80 with field view-z ≈ [22,61]. True near-clip (Stream N) stays deferred for T0. **Margin to watch:** kf2 pulls radius/height to 0.7× for the near-terrain-fill worst case — if D.1's real mesh is taller (bigger +Y relief) the kf2 fill pose is the first to risk a near crossing; re-check after the swap.
- **pool-cap headroom (M2 not tripped):** 924 source tris (900 terrain + 24 tree) → worst-case 2772 tverts vs `RDR_MAX_TVERTS`=3000 (228 headroom; no per-vertex sharing in the thin slice, so tverts = tris×3). Density was sized DOWN from the literal 33×17=1024 specifically to keep this headroom. **At T0, D.1's full 1024-tri mesh would need 3072 tverts > 3000 → drops.** Either D.1 ships ≤1000 tris, or the pool grows (config delta, Lead), or geom gains vertex sharing. Flagged for the Lead, not blocking the placeholder.
- **determinism discipline that worked:** integer-only per-frame path (frame counter + int64-widened Q16.16 keyframe lerp + fixed-delta pow2 scroll); float confined to one-time setup (keyframe table + per-frame MVP bake, a pure fn of the integer pose). Guard: a bit-reproducibility test replays a 600-frame button trace twice and compares camera/scroll member-wise. Cheap, and it would catch any float creeping into the advance.
- **friction:** (1) `tidy` `bugprone-suspicious-memory-comparison` rejects `memcmp` over a padded POD (DemoCamera) — the determinism guard had to compare members explicitly; reasonable, but the "bit-reproducible" intent reads less directly. (2) host machine ran ~5 concurrent `make tidy` invocations across worktrees → tidy is slow + a naive `pgrep clang-tidy` wait matches OTHER worktrees' runs; scope the wait to the worktree path. (3) the demo-scene library now `#include`s `platform/platform.h` only for `enum Button` — resolves via the existing `src/` include root, no link dep, but it's a header coupling to keep an eye on (a tiny `input_buttons.h` enum-only header would decouple it if platform.h ever grows heavy includes).

### D.7 Δ — scripted camera baked OFFLINE to a committed Q16.16 table (Lead review, Q5/TW-05) — 2026-06-03
- **Why:** the original per-frame `double` `m_perspective`/`m_lookat`/`mat4_mul` bake made host↔device fb_crc only ~3.4e-5 approximate (on-device float ≠ host float), not bit-identical. Q5/TW-05 locked: bake offline → commit Q16.16 → both host and device read the SAME bytes.
- **What landed:** `src/demo/gen_camera_path.py` (I own it) READ-ONLY imports `bake_mvp_q16_16`/`to_q16_16` from `tools/asset_convert.py` (TW-05 helper; tools/ untouched), ports the keyframe math + integer interpolation to Python, bakes each of the 480 scripted frames' V·P, and emits the COMMITTED `src/demo/camera_path_gen.h` (`SCRIPTED_FRAME_COUNT 480`, `g_scripted_mvp[480][16]`, **57 KB**). Build does NOT run Python (committed data, like `assets_gen`). Scripted runtime = `g_scripted_mvp[frame % SCRIPTED_FRAME_COUNT]` cast to `Mat4fx*` — **zero float on the scripted path**; the keyframe trig + per-frame double mat-mul were deleted. FREE-FLY keeps its on-device float bake (sanctioned interactive exception, never fb_crc-compared) — cleanly branched in `demo_terrain_build`.
- **convention pin:** the Python builds V·P in the SAME column-major flat layout the C `Mat4fx.m[]` used (`m[col*4+row]`), reshapes `m[i*4+j]→vp[i][j]`, bakes via `bake_mvp_q16_16` (`flat[i*4+j]=q16(vp[i][j])`). The Lead's key insight made this clean: Python and C float math need NOT agree bit-for-bit — only the committed table is normative, and host==device both read it. Verified: scripted fb_crc reproduces identically across runs (frames 0/123/240/479).
- **generated-file gates:** `// clang-format off/on` markers make `format-patch` pass without a dir-local `.clang-format DisableFormat` (can't disable the whole `src/demo/` — it holds hand-authored files); `// NOLINTBEGIN/END` around the table suppresses generated-data tidy (naming/magic-number) without guessing the exact check. The `assets_gen` pattern hides generated DATA in a `.cc` excluded from the tidy file-list; a committed generated `.h` that a tidied `.cc` includes has no such escape, so in-file NOLINT is the right tool.
- **review MINORs fixed:** (a) `LOAD_VERTS.count` now driven by the count RETURNED from `demo_terrain_geometry`/`demo_tree_geometry` (stored in module state), not the `DEMO_TERRAIN_VERTS` macro — a T0 D.1-mesh swap with a different vert count can't silently mismatch the window. (b) `render_terrain`'s `(n-1)%CAP` END check → a plain `if (n<1||n>CAP) FAIL()` bound + direct `cmds[n-1].op==CMD_END` (the `if` is what the static analyzer trusts; gtest ASSERT it does not model — that nuance is why the original used `%CAP`).
- **friction:** `clang-analyzer-security.ArrayBound` does NOT treat gtest `ASSERT_*` as a control-flow bound, so `cmds[n-1]` after `ASSERT_LE(n,CAP)` still trips it; need a plain `if`-guard early-return to satisfy it. Worth a one-liner in the testing skill (analyzer-provable bounds use `if`, not `ASSERT`).

### D.7 ΔΔ — placeholder INPUT geometry moved to FLASH-CONST (integration-gate RP2040 RAM overflow) — 2026-06-03
- **Bug the HOST gate could not see:** host build + 261 ctests + tidy all PASS, but the merged-lanes PICO link `region RAM overflowed by 7772 B`. Target-SRAM-only; the host gate never links the arm-none-eabi RAM region. **Root cause (my lane):** the placeholder INPUT geometry was computed into static buffers at init, so it sat in `.bss` (RAM): `s_terrain_vtx` 6944 + `s_terrain_idx` 5400 + `s_tree_vtx` 672 + `s_tree_idx` 144 = **13,160 B** (nm `--print-size` sizes; the Lead's 7060/1660/988/844 were the symbol OFFSETs, not sizes). `g_scripted_mvp` was already correctly in flash.
- **Fix:** `src/demo/gen_debug_terrain.py` (owned) bakes the placeholder terrain+tree geometry (positions, per-cell debug colors, indices, winding) to a committed FLASH-CONST header `src/demo/debug_terrain_gen.h` (`g_debug_terrain_vtx/idx`, `g_debug_tree_vtx/idx`; **42 KB**). The device demo submits LOAD_VERTS/DRAW_TRIS pointers straight at the const arrays → input geometry leaves `.bss` entirely (nm AFTER: all four symbols are `r`/.rodata, zero `.bss`). The runtime `demo_terrain_geometry`/`demo_tree_geometry` FILL functions stay (host tests + the T0 D.1 swap seam use the fill API); only the device path changed.
- **Result:** pico `renderer_demo.elf` LINKS; ~3.3 KiB RAM margin on this lane (freed 13.2 KB vs the 7.77 KB overflow). The two `.bss` giants are renderer-owned, NOT mine: `s_frame` 127,080 B (pool+bins+16 KB arena+zbuf) + `s_present_fb` 115,200 B (240×240 framebuffer) = 242 KB of the 248 KB `.bss`. My demo's own remaining `.bss` is ~1 KB (matrices+cmd buf). **Headroom note for the Lead:** further RAM beyond my fix is a Frame/cap budget decision (arena size / `RDR_MAX_TVERTS` pool / framebuffer), outside the demo lane.
- **closed-loop guard:** `FlashConstGeometryMatchesFillApi` asserts the committed const arrays are byte-identical (field-wise for `Vtx` since its union has no unique object representation — memcmp would be UB/tidy-flagged; memcmp is fine for the plain `uint16` index arrays) to what the fill functions produce. Edit the fill math without regenerating → test fails. Keeps the committed flash geometry honest.
- **GATE EXTENDED (the host gate missed this):** D.7 now runs `cmake --preset pico && cmake --build --preset pico` and confirms `renderer_demo.elf` links (no `region RAM overflowed`) AFTER the host gate. **Lesson for the dispatch/worktree skill:** any lane that adds static buffers or owns the demo MUST run the pico link as a gate — the host gate is structurally blind to target SRAM. (PICO_SDK_PATH=~/development/repos/pico-sdk needed for the pico preset; not in env by default.)

## Wave-D RETRO (2026-06-03, landed `c65d24a`, remote 6-job CI green)

**KPIs**
- **Contract-delta count: 2** — Δ3 (`TEXFMT_RGBA5551`) + Φ (`frame.h rstate_table`), both pre-wave Lead micro-barriers. **ZERO stream-driven deltas**: every "need" resolved in a stream-owned header (`BlendMode`→blend.h, `Blit2dRect`→blit2d.h, TLUT→decoded-as-5551-no-field). ≤2 ⇒ **the freeze held; not premature.** Under the budgeted Δ3/D1/D2/D4 (D1/D2/D4 deferred to R-lane/T4).
- **CI-on-main red frequency: 0** — all 5 pushes green; the lone gate failure (the N7 RAM overflow) was caught by LOCAL integrated `ci_main` **before** push → never reddened main. The multi-layer gate (stream lane → review → integrated ci_main incl. pico link → remote) is the reason.
- **rework-after-review: 5/6 streams (D.4 clean), stratified:** 1 real correctness gap (D.2 untested upper-triangle 3-point branch — caught by review, fixed with fail-before/pass-after teeth); 2 doc-only (D.3 parity-comment latent trap, D.5 horizon doc); **2 Lead-decision reworks** (D.1 atlas, D.7 offline-bake — locked-decision *conformance*, not review defects); +1 integration-gate rework (D.7 RAM, caught by ci_main not review). The high rate is foundation-bar depth + 2 locked-decision deviations, not sloppy streams.
- **on-target gate pass rate: N/A** — no on-target probe this wave (host + pico-*build* only); pico **link** passed post-RAM-fix. T0 is the first on-target probe.
- **PR cycle (open→merge-ready):** ~15–65 min/stream concurrent; critical path D.7 (3 rounds: build→camera-rework→RAM-fix). 6-wide fan-out; serial Lead fan-in + per-merge union of WORKFLOW.md (`merge=union` driver added this wave eliminated the 5× conflict).

**Two findings the gate layers earned their keep on:** D.2's untested inverted-fraction branch (review) and D.7's target-SRAM overflow (integrated ci_main — host-blind). Both pre-merge, both now guarded.

**Triage → `.claude` changes (ship with Wave-E contract-delta PR; bind at next spawn unless LIVE):**
| Friction | Change | Target | When |
|---|---|---|---|
| TW-04 probe validity | checklist: Release/-O3/NDEBUG, hot-code-in-SRAM, `volatile` sink vs DCE, confirm SYSCLK, SDK structs for HW regs | `on-target-probe` skill + `perf-profiler` agent | next-spawn |
| D.7-ΔΔ pico-link blind spot | rule: any lane owning the demo / adding static buffers MUST run `cmake --build --preset pico` link as a gate (host gate is SRAM-blind); needs `PICO_SDK_PATH` | `renderer-module-owner` agent gate def | next-spawn |
| D2-01/D3-2 tidy false-green | `ci_main.sh` greps the tidy log for `error:` (belt-and-braces); dispatch note: harness TUs must pass `make tidy`, grep the log when backgrounding | ci_main.sh (LIVE) + dispatch template | live + next-spawn |
| TW-06 spike→consumer handoff | consuming stream's prompt cites the gating spike's locked outputs as guards | dispatch template / `worktree-pr` | next-spawn |
| D.5 new-module CMake | R8 carve-out: a genuinely-new module's single `add_subdirectory` line is the new owner's to add (no contention) | `worktree-pr` R8 rule | next-spawn |
| TW-09 glossary / D3-3 K_-naming | dispatch-template lines: "mesh-cell" vs "screen-tile"; file-scope `static const` in tests → `K_…` | dispatch template | next-spawn |
| TW-01 task_schema | hook reads `.task.metadata.*` OR reject-msg → "track in WORKFLOW.md" | `.claude/hooks/task_schema.sh` (LIVE) | live |
| TW-02 spike discard | `tools/discard_spike.sh` or `spike/*` guard allowlist | tools/ + guard | next-spawn |
| TW-10 plans in repo | DONE — Wave-E plan committed to `docs/superpowers/plans/` | — | live (this retro) |

**Next wave drafted:** `docs/superpowers/plans/2026-06-03-waveE-rlane-t0.md` (T0 on-target barrier + serialized R-lane R.1→R.4 + T1–T5; contract-delta = D1 `TVtx.fog` + Φ2 cov/blit scratch + `src/aa/` + `RDR_MAX_TVERTS`→2048-after-T0-indexed-swap).

## G1 — geom-share barrier (transform-once + vert sharing) — 2026-06-03 (branch `impl/geom-share`)

**Change:** verts now transformed ONCE at CMD_LOAD_VERTS time (matching N64 gSPVertex semantics) and pooled contiguously from `vbase`. DRAW_TRIS resolves pooled TVtx by index; all-inside tris go directly to `geom_bin_tri` (zero new tverts). Clip-path (guard-band crossing) still emits clip-fan tverts. Near-behind verts get a sentinel (inv_w=-1); tris referencing a sentinel drop.

**Pool before/after on the 2x2 quad grid test (SharingReusesPooledVerts):** 9 source verts, 8 tris.
- Before: per-tri emit → 3×8 = 24 tverts (old code; test fails).
- After: transform-once → 9 tverts (new code; test passes).

**Terrain relevance:** D.1's 1024-tri mesh needs ~3072 tverts with per-tri emit (over `RDR_MAX_TVERTS`=3000). With sharing: up to ~1152 unique tverts (depends on vert reuse factor), fits comfortably.

**Bit-identical:** all 267 host-suite fb_crc goldens pass unchanged. The fast path bins the SAME pooled TVtx values the old path would have re-projected identically; culling and bin order are unchanged; clipped tris go through the same geom_emit_tri path.

**Friction:**
- Test `ClipPathStillEmits` initially used all-collinear verts (y=0 for all three → degenerate after projection → tris_total=0); caught on RED check. Fixed by giving verts non-zero y spread.
- `misc-const-correctness` on `cnt` (geom.cc) and `xs`/`ys`/quad-index vars (geom_test.cc); `readability-math-missing-parentheses` on `r*3+c` expressions in test. All fixed before commit; tidy exit=0, zero `error:` lines.
- `make tidy` background-task false-green risk (D2-01/D3-2 lesson applied): ran via `cmake --build build-host --target tidy 2>&1; echo "EXIT:$?"` chained with a grep, confirmed exit=0 and zero `error:` before marking tidy clean.

## R.1 — textured fill + Gouraud + alpha-cutout (raster inner loop) — 2026-06-03 (branch `impl/raster-r1`)

**Change:** `raster_one` now branches per-material on `rstate_table[TriRef.material]`: no-texture → flat fast path (byte-identical, golden PNG unchanged); valid texture → perspective-correct UV + affine Gouraud SHADE + `shade_pixel` combiner + alpha-cutout discard. New last param `const RenderState* rstate_table` on `raster_tile`/`_noclear`; threaded through `drain.h` (`&f->rstate_table[0]`) + the 3 sched_test call-sites. `TriSetup` carries per-vtx `u_iw/v_iw/rgba`, swapped in lockstep on winding-normalize.

**UV→Q16.16 (verified vs oracle_sample_texel):** `u_q16 = (u_iw_p << 27) / inv_w_p` where `u_iw_p = Σw_i·u_iw_i / area2` (affine, same int64 num/area2 truncation as the inv_w divide → P3-5 host↔device bit-identical). POINT idx = `u_q16>>16`. Fused `<<27` keeps fractional texel bits for THREE_POINT.

**Δ4 verdict:** measured u_iw over the R.1 test tris = [0, 224]; aggressive terrain-worst probe (512 S10.5 ≈16 texels × inv_w 4.0) = 2048, all well within int16 (±32767). **No overflow risk at these scales — re-measure at T1 with real terrain UVs.** Did NOT widen types.h.

**Friction (with executable-guard ideas):**
- **Worktree branch vs shared-checkout confusion (cost ~3 round-trips).** First `git checkout -b impl/...` ran in the SHARED checkout (Edit then errored "edit the worktree copy"); the worktree's own branch was `worktree-agent-…`. The `destructive_guard` hook then blocked freeing the stray name on the shared tree. Resolution: `git branch -m` the WORKTREE branch to `impl/raster-r1`. → Guard idea: dispatch should `cd` agents into the worktree path AND pre-create `impl/<stream>` checked-out IN the worktree, or a `verify_worktree.sh` that asserts `git rev-parse --show-toplevel` == the agent's worktree before any edit.
- **Tidy is enforced on tests too** (carve-out is orthodoxy, NOT tidy): 47 errors from compact `int a=…, b=…;` decls + `lrintf`(long)→int narrowing + `uint8_t·float` implicit narrowing + `!(a&&b&&c)`. House tidy-style (one-decl-per-line, east-const, parens, no narrowing) must be written on the FIRST pass for test helpers, same as src. → The dispatch K_-naming note should also remind: tests obey readability-isolate-declaration + misc-const-correctness + bugprone-narrowing-conversions.
- **Float-oracle pitfall: high-frequency texture defeats a ±1-2 tolerance.** A per-texel-distinct texture (R=u·30) made a 1-texel fixed-vs-float boundary slip show as a 22-level diff. Fix: (a) low-frequency gradient texture for the tolerance test (1-texel slip ≤2), AND (b) split the assertion into an independent PERSPECTIVE check (float vs fixed texel index within 1 texel) + a COMBINER check that pre-quantizes the float expectation through the SAME 565 pack/unpack before comparing (residual = only the N64 integer-combiner rounding, ≤1 565-quantum = ≤9/RB, ≤5/G). Keep the high-frequency texture for the bit-EXACT self-check. → Skill `golden-image-test`/`fixed-point`: note that POINT-sampled tolerance tests need a low-frequency reference OR a same-index comparison; never compare a quantized output to an un-quantized float expectation.
- **`clang-analyzer-core.DivideZero`** flagged the test's `/inv_w` recovery — mirrored the impl's `inv_w<=0 → return 0` guard in the reference (also correct behavior).

## T1 — textured terrain + tree sprites (demo scene) — 2026-06-03 (branch `impl/t1-textured`)

**Change:** demo terrain renders TEXTURED. `demo_terrain/tree_material(RenderState*)` fillers = single source of truth; `demo_terrain_build` binds a distinct SET_MATERIAL per draw (atlas TEXEL0×ENV, light-sage ENV tint rgb565(216,232,200); tree0 sprite MODULATE, CULL_NONE). Tree verts gain sprite UVs (S10.5=texel×32, upright) + gray Gouraud gradient (top 206/bot 157); generator mirrored + regenerated. Raster untouched (R.1 path). Flat-fill elsewhere bit-identical.

**Δ4 VERDICT — FITS (peak 32611, headroom only 156):** real terrain UVs over the full scripted loop give peak |u·inv_w| = 32611 < 32767 → int16 `TVtx.u_iw` does NOT overflow at T1. BUT margin is thin (156). A nearer camera, larger pre-scale, or atlas >512 would FIRE Δ4 (widen TVtx.u_iw/v_iw int16→int32, frozen types.h, Lead). The `TexcoordTimesInvWFitsInt16` guard now pins this; if a future change trips it, escalate rather than weaken the bound.

**fb_crc golden rebake:** 0x9ab0a0f9 → 0xa59eb386 (intended: render changed flat→textured). Histogram 35 distinct hue buckets (≥8 threshold; light-sage ENV preserves atlas hue variety — not washed out).

**Friction (with executable-guard ideas):**
- **Bash `cd` into the SHARED checkout silently ran build/test/ctest against unmodified source (cost ~1 round-trip; same class as R.1's worktree confusion).** The agent cwd defaults to the worktree, but an explicit `cd /…/renderer` in a command jumps to the shared tree where my edits don't exist — tests "passed" with the OLD test names/count. Tell: ctest test COUNT and names didn't change. → Guard idea: a `make`/ctest wrapper (or the `verify_worktree.sh` proposed at R.1) that asserts `$PWD`/`git rev-parse --show-toplevel` == the agent worktree and refuses to build otherwise; never `cd` to an absolute repo root in a worktree agent.
- **ownership.json has no `tests/demo/**` owner key** → `verify_stream_branch.sh impl/t1-textured Lead` flags `tests/demo/{CMakeLists.txt,demo_scene_test.cc}` OUT-OF-LANE even though the T1 dispatch explicitly grants them. Not a cross-lane write — a manifest gap. → Fix: add `tests/demo/**` under `Lead` (or a `Demo` owner) in `.claude/ownership.json` so the pre-merge guard is green for legitimately-scoped demo-test edits.
- **Test needs geom+shade symbols not transitively exposed by `Renderer::rdr` (links them PRIVATE).** Added `Renderer::Geom Renderer::Shade` to the test target for the Δ4 probe (`geom_transform_clip`/`geom_project`/`vp_from_cmd`) + ENV proof (`shade_pixel`). Sanctioned by the dispatch ("only if you must add an include").
- **`out.u_iw` is already int16 → measuring it would MASK the overflow.** The Δ4 probe recomputes geom_project's `fx_to_int(fx_mul(fx_from_int(u),inv_w))` at FULL int32 width (the value BEFORE the int16 narrow) so an overflow is visible, not wrapped. (`FX_ONE` is a per-.cc macro, not exported → used `fx_from_int(1)`.)
