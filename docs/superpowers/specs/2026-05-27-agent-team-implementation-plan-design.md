# Design: Agent-Team Parallel Implementation Plan for the PicoSystem Renderer

## Context

The renderer design spec (`docs/superpowers/specs/2026-05-26-command-buffer-renderer-design.md`)
defines *what* to build. This spec defines *how* a **Claude Code agent team** builds it: the team
topology, the custom agents / skills / hooks, the parallel execution model, and the verification
ladder. It targets the experimental Agent Teams feature (lead + teammates, shared task list, direct
messaging; `CLAUDE_CODE_EXPERIMENTAL_AGENT_TEAMS=1`) and follows the dispatchable-stream structure of
`parallelize-plan.md`, the team guidance in `AGENT_TEAMS.md`, the prompt practices in
`PROMPT_GUIDELINES.md`, and the embedded-TDD strategy in `~/books/technology/tdd-summary`.

Base: the project starts from `../picosystem-template` (C++20, CMake ≥3.28). The template already
supplies the embedded-TDD spine — dual-targeting (host `sdl` preset = SDL3+ImGui+GoogleTest = Stage-1
microcycle; `pico` preset = arm-none-eabi `.uf2` = Stage-2 compiler-compat), a module-per-dir layout
(`src/{platform,app,game,gfx}` with mirrored `tests/`), Orthodox C++ enforcement (Orthodoxy clang
plugin), sanitizers, clang-tidy (host + pico), and the `platform.h` adapter split. The renderer adds
its own module dirs under the same conventions.

### Resolved parameters (locked)

- **Isolation model: git worktrees + a central-repo / PR workflow.** One canonical repo with a `main`
  integration branch is the single source of truth. Each teammate works in **its own git worktree on
  its own feature branch**, regularly pulls `main` to stay current, and lands work via a **reviewed
  PR-style merge to `main`**. This emulates a central remote with contributors pulling and submitting
  PRs — and gives true source isolation (a teammate's in-progress, non-compiling work is invisible to
  others until merged). This supersedes the earlier shared-working-tree choice.
- **Team shape:** subsystem owners + standing cross-cutting roles (Lead + 5 teammates); review,
  integration, perf, and on-target are roles invoked on demand, not extra standing teammates.
- **Plan scope:** durable workflow infra + a fully detailed **foundation wave** + a repeatable wave
  template; each wave ends in a retrospective + next-wave planning stream.
- **On-device verification:** hardware available; **agent-driven** via `picotool` flash + USB-CDC
  serial readback (`KEY=VALUE\n`) parsed and threshold-asserted. Visual checks (AA/tearing/banding)
  remain human-confirmed.
- **Runtime:** tmux split panes; **all teammates on Opus**; `fixed` lands "immediate" (owners start
  test-first against its frozen header rather than waiting for its impl to merge); serial protocol
  `KEY=VALUE\n` over USB-CDC stdio.

## ⚠ Execution-Model Amendment (post-A10 validation, 2026-05-27) — SUPERSEDES the agent-team vehicle

The A10 smoke test (see `WORKFLOW.md`) found that **Claude Code agent-team *members* run in the lead's
working tree — they do NOT get per-teammate git-worktree isolation** (a spawned team member committed
straight to `main`). The worktree/PR model above is therefore **not achievable via agent teams.** A
**subagent with `isolation:worktree` DOES** get its own worktree+branch, and **hooks fire for both**.

**This document's "agent team / teammates / shared task list / SendMessage" framing is superseded by a
subagent-driven model** for all *implementation* work. What changes vs. what stands:

- **Execution vehicle → `superpowers:subagent-driven-development` + `dispatching-parallel-agents` +
  `using-git-worktrees`.** The Lead holds the dependency graph and **dispatches a fresh
  subagent-in-a-worktree per stream** (the `renderer-module-owner`/`infra-tooling` agent defs become
  *dispatch roles*, not standing teammates), reviews the returned branch, merges to `main`, and
  dispatches the next unblocked streams. Read "teammate Tn" throughout this doc as "the subagent
  dispatched for stream X."
- **Barriers → lead-sequenced** (the Lead dispatches B.0/ε first, then β/γ/δ, etc.) rather than encoded
  in an agent-team shared task list.
- **Hook scope (measured):** `PreToolUse` (destructive guard) + `PostToolUse` (format) **fire in
  subagent worktrees** ✓. The team-event hooks `TaskCreated`/`TaskCompleted`/`TeammateIdle` **do NOT
  fire** for subagents → their gates move to the **Lead's pre-merge gate**: module `ctest -L <mod>` +
  `make format-patch` + Orthodoxy + `renderer-reviewer` + `ci_main.sh` green, before merging a stream.
- **Stands unchanged:** the worktree-per-stream + reviewed-merge-to-`main` topology (now via subagent
  isolation); `.claude/ownership.json` (the reviewer/lead enforce lanes at merge); the contract freeze;
  the C1/C2/C3 integration split; the verification ladder; the wave template + retro.

Everything below is read through this amendment. The H1–H3 "launch the team" tasks in the plan are
replaced by the subagent-dispatch loop.

## Targets / Non-Goals

- **Targets:** a dispatch-ready workflow — agents, skills, hooks, the worktree/PR protocol, team
  bootstrap; the Contract-Freeze stream; the complete foundation-wave stream topology (project init →
  `fixed` → `cmd`/`arena` → `geom`/`clip` → `raster` → basic `tex`/`shade`/`blend` → dual-core
  `sched`: a spinning textured triangle on host **and** device); and a repeatable wave template whose
  terminal stream is retro + replan.
- **Non-goals:** detailing feature waves W2–W5 up front (they are detailed just-in-time each wave);
  re-deciding any renderer design decision (that spec is the source of truth); a literal GitHub remote
  is optional, not required (the worktree model gives the PR workflow locally — see Unresolved Q1).

## Workflow Architecture

Interface-first, dual-target TDD, executed in **waves** by a standing team. Work lands on `main`
through reviewed PR-merges; one **hardware spike precedes the first contract freeze** so struct
layouts are sized from measurement, not guessed.

```
S0 BOOTSTRAP (template -> renderer, canonical repo + main)   [serialized, Lead+T5]
  toolchain inventory + hard-verify; worktree/PR protocol; CI-on-main; team+worktree smoke test
        |
        v
S0.5 HARDWARE SPIKE (throwaway firmware on a branch)         [serialized, T5+T2]
  measure free RAM / mul-div + fill throughput / scanout timing on device.
  Records the sort-middle-vs-fallback decision and the sizes Stream A will freeze.
        |
        v
Stream A: CONTRACT FREEZE (one PR, lands all headers on main)  [barrier-gated]
  all module headers + shared types in src/rdr/types.h; stub .cc; host + arm compile green.
  Every feature branch is cut from this commit.
        |   (no implementation branch starts before A is merged to main)
        v
Wave fan-out: one worktree+branch per stream, parallel behind frozen headers   [R8 file-partitioned]
  each: red-green-refactor on host; pull main regularly; PR -> review -> merge to main
        |   (incremental integration as modules land, NOT one big-bang fan-in)
        v
C1 SINGLE-CORE INTEGRATION (host + device frame path, no sched)   [integrator]
  modules wired through rdr; transform->clip->raster->shade->blend->fb; full-frame golden vs oracle
        |
        v
C2 CONCURRENCY INTEGRATION (dual-core sched + scanline-race + on-device)   [T4 + integrator]
  Stage-2 arm compile + on-target Phase-0 re-confirm; tag wave-base
        |
        v
C3 THROUGHPUT PROBE (flat-triangle flood on the real pipeline)   [perf-profiler]
  measure achievable tri/s on device vs the 100K target -- BEFORE investing W2-W5 in features
        |
        v
Retro + plan next wave                          [Lead + all]  -> next contract delta-freeze
```

Every code stream runs the **embedded-TDD ladder**: Stage-1 host microcycle (`make test`, every
change, in the teammate's own worktree) → Stage-2 arm-none-eabi compile-compat → Stage-4 on-target
serial run. Stage-1 is a hard per-task gate. **Stage-2 runs continuously on `main` (CI after every
merge) plus a nightly arm-compile-all**, not only at the barrier — this matches Grenning's guidance
to recompile for the target "whenever you use a new header, feature, or library call" and avoids
batching dual-target incompatibilities into one painful fan-in. Stage-4 gates the integration barrier
and the perf gate. This is Grenning's cycle: the bulk of code is written and proven off-target on the
host; the target is exercised continuously to catch dual-target incompatibilities (different compiler
bugs, type sizes, byte order, runtime libs) early, contained behind the `platform.h` adapter.

### Why the hardware spike precedes Stream A

The renderer spec orders Phase 0 *first* ("validate real free RAM on-device before locking sizes")
and decides the sort-middle vs immediate-forward+8-bit-Z fork on measured cost. Freezing the compact
`TransformedVertex`, tile bin, and rasterizer architecture in Stream A *before* measuring would risk
building the wrong architecture and discovering it only at integration. S0.5 is a throwaway-firmware
spike on its own branch (discarded after, never merged to `main`). Its measurements differ in
cost/confidence: **free RAM and scanout timing are cheap and high-confidence**; a faithful
transform:raster *ratio* would need a real transform path *and* rasterizer, so S0.5 instead measures
**proxies** — fixed-point mul/div throughput, memory-fill bandwidth, and a micro-rasterizer
benchmark — enough to choose the architecture without building it. Stream A then freezes layouts that
fit the measured budget; C2 re-confirms the real ratio under the integrated pipeline.

### Why integration is split into C1 (single-core) and C2 (concurrency)

A single big-bang fan-in of every module *plus* the dual-core machinery is the integration anti-pattern
TDD/CI warns against: wiring bugs (interface mismatch, façade glue) would land tangled with concurrency
bugs (ordering, spinlocks, the scanline-race watermark), all at once. **C1 integrates the modules on a
single-core frame path** (`transform → clip → raster → shade → blend → framebuffer`) and proves it with
a **full-frame golden image vs. the float oracle** on host plus a static frame on device — a real
integration checkpoint before any concurrency exists. **C2 then adds `sched` (dual-core), scanline-race,
and the on-target re-confirm.** This also isolates the plan's hardest-to-test risk: the host (SDL/x86)
**cannot faithfully reproduce RP2040 dual-core memory ordering, spinlock semantics, or DMA/scanout
timing**, so `sched` is the one stream where Stage-1 host TDD is weakest. It is validated two ways — a
**host 2-worker simulation** of the queue/bin-merge/watermark logic (per the renderer spec) *and*
**prioritized on-target device time** for the real ordering/timing — and it is deliberately confined to
C2 rather than smeared across the wave. Per-PR review is necessarily **lane-scoped** (the reviewer sees
one branch diff against the contract), so cross-module *semantic* assumptions are caught here at
integration, not at merge — another reason to integrate incrementally (C1 before C2) rather than trust
a single fan-in.

## Team Roster & Ownership

Each teammate owns a set of modules — `src/<mod>/` **plus** the mirrored `tests/<mod>/` — and works
in its own worktree/branch. **Ownership now serves merge-conflict avoidance (R8), not hook
enforcement:** because branches are isolated and PRs are reviewed before merge, partitioning by file
keeps PRs from conflicting rather than preventing silent overwrites. The Lead is the `main`
maintainer who merges PRs.

| Member | Owns | Wave-1 focus |
|---|---|---|
| **Lead** | `src/rdr/` (`types.h`/`config.h`/façade), top `CMakeLists.txt`, `.claude/` config, `main` merge rights | contracts, integration, perf gate, PR merges |
| **T1 Math+Geom** | `fixed/`, `geom/`, `clip/` | `fixed` → `geom`/`clip` |
| **T2 Raster** | `raster/`, `aa/` | flat raster + Z |
| **T3 Shading** | `tex/`, `shade/`, `blend/` | point-`tex` + modulate + opaque |
| **T4 Cmd+Mem+Platform** | `cmd/`, `arena/`, `sched/`, `platform/` | command buffer, arena, multicore glue, ST7789 565 fork + scanline-race DMA |
| **T5 Infra** | `tests/harness/`, `tools/` (assets), `.claude/skills`+`hooks`, on-target runner, CI | oracle + golden + ImGui + asset tool + serial runner |

**Orthodoxy carve-out for the ImGui inspector.** T5's host inspector interfaces ImGui — classes,
namespaces, references, overloads — which violate Orthodox C++ wholesale. Per the CLAUDE.md pragmatism
clause, **Orthodoxy is scoped to renderer modules and the inspector is exempt** (own target, excluded
from the Orthodoxy-enforced set, with narrow per-line suppressions where ImGui leaks across a
boundary). Otherwise the TaskCompleted Orthodoxy gate fights T5 on every edit.

**Load balancing within a wave (a worktree-model tension).** Owner load is uneven — `raster`+`aa` (T2)
and `cmd`+`arena`+`sched` (T4) are far heavier than `arena` or `blend` alone — and worktree isolation
makes it awkward to hand a stuck/idle owner's work across branches. Mitigations: size heavy modules as
**multiple sub-streams** (each its own branch) so they can be reassigned or picked up by an idle owner
via a helper worktree (coordinated with the module owner); the Lead rebalances at the daily check-in
rather than relying on self-claim across lanes.

`.claude/ownership.json` records each teammate's owned globs as the conflict-avoidance manifest and
the source the `TaskCreated` schema check validates against. It is Lead-owned; ownership changes are
deliberate, low-frequency, and land via the contract-delta PR. There is **no `session_id → teammate`
table and no PreToolUse ownership block** — the worktree/branch boundary and PR review do that job.

### Shared files are the merge-conflict hotspots — serialize them

With branches, contention shows up as merge conflicts, concentrated in a few shared files:
`src/rdr/types.h`, the top and `src/` `CMakeLists.txt`, `.claude/*`, and `settings.json`. Mitigation:
**Stream A pre-registers every foundation-wave module subdir** (`add_subdirectory(<mod>)`) in
`src/CMakeLists.txt` and lands all shared types in `src/rdr/types.h`, so feature branches never touch
the parent CMake list or the shared header — they only add their own module's sources behind the
already-registered subdir. Later waves' new modules are pre-registered in that wave's contract-delta
PR (Lead). Shared-type changes happen only through contract-delta PRs, serialized through the Lead.

## Custom Agents (`.claude/agents/`, subagent defs reusable as teammates)

Each agent body is appended to the teammate's system prompt (it does not replace it); the `skills`/
`mcpServers` frontmatter is ignored for teammates (AGENT_TEAMS.md), so the body explicitly names the
skills to use and the project skills live in `.claude/skills/` (project scope, auto-loaded). Bodies
cite the project CLAUDE.md (Orthodox C++, refactoring, Modern CMake) and the renderer spec.

**Standing teammates (one worktree each):**
- **renderer-module-owner** — T1–T4: Orthodox C++ + embedded-TDD discipline; contract-first;
  host-first red-green-refactor; "do you have a test for that?" rigor.
- **infra-tooling** — T5: host harness, asset converters, hook/skill authoring, serial test-runner, CI.

**Roles invoked on demand (subagents the Lead/owner spawns; not extra panes):**
- **renderer-reviewer** (Opus) — reviews each PR's branch diff before merge (Orthodoxy / correctness /
  contract-conformance); coverage-first finding mode (report all findings with confidence+severity),
  per PROMPT_GUIDELINES code-review guidance.
- **integrator** — at the barrier: confirm all wave branches merged, run Stage-2/on-target on `main`,
  tag `wave-base`.
- **perf-profiler** — on-target cycle counters; perf-gate levers from the renderer spec.
- **on-target-runner** — `picotool` flash + serial parse + threshold assert.

Spawn prompts follow PROMPT_GUIDELINES: explicit role, scope, Files-Owned, branch name, verification
commands, and success criteria stated up front to maximize autonomy and minimize mid-task user turns.

## Custom Skills (`.claude/skills/`)

- **contract-first** — define/freeze a module header (POD structs + `module_verb` signatures + error
  codes) before any impl; gate that the header compiles and a stub `.cc` links.
- **orthodox-tdd-cycle** (rigid) — red→green→refactor bound to `make test`/`make format`/`make tidy`,
  Orthodox C++ idioms, refactor-only-on-green, copy-don't-cut extraction.
- **fixed-point** — Q-format reference: Q16.16 / Q12.4 / 1-w formats; no-`UMULL` multiply discipline
  (16×16 partials, `MULS`); SIO hardware divider; floats only in setup/asset tools.
- **golden-image-test** — author/run float-oracle + golden-image comparisons on host (bit-identical
  fixed-point parity); inject-an-error-to-confirm-the-test-is-wired discipline.
- **renderer-module-scaffold** — generate `src/<mod>/{<mod>.h,<mod>.cc,CMakeLists.txt}` + `tests/<mod>/`
  matching template conventions (project-prefixed target + `Renderer::<Mod>` alias, `project_warnings`,
  sanitizers link, `gtest_discover_tests`). Assumes the subdir is already registered (see Stream A).
- **worktree-pr** — create the stream's worktree/branch off `main`, keep it current (`git merge main`),
  open the PR (branch diff + checklist), and merge after review; standardizes the central-repo flow.
- **on-target-probe** — build `.uf2` → flash → read serial → parse `KEY=VALUE` → assert thresholds;
  emit the measurement protocol for human visual checks.
- **wave-retro** — structured retrospective + next-wave contract-delta + stream-topology draft;
  triages per-teammate workflow notes into shipped agent/skill/hook changes.

## Custom Hooks (`settings.json` — quality gates, not isolation)

Worktrees + PR review provide isolation, so hooks are pure quality gates:

| Hook | Trigger | Action |
|---|---|---|
| **PreToolUse** (Bash) | destructive git/shell ops | block (exit 2) `push --force`, hard reset of shared refs, `branch -D`, `git clean -fdx`, history rewrites, `rm -rf` outside the worktree's build dir. **Safety guard for 6 autonomous agents** (the worktree model means a stray reset/force-push can destroy others' work); per the "balance autonomy and safety" prompt guidance. |
| **PostToolUse** (Edit/Write on `*.cc`/`*.h`) | after write | `clang-format --dry-run` on the changed file (cheap, standalone). Orthodoxy is **not** run here — it is a clang *plugin* needing the compile DB, so it runs in the build gate, not per-edit. |
| **TaskCreated** | task add | reject (exit 2) unless the task carries Files-Owned + branch + Verification (enforces the dispatchable-unit schema) |
| **TaskCompleted** | mark complete | **hard gate:** `make test` (host, in this worktree) green + `make format-patch` clean + Orthodoxy clean (it runs inside the host build); else exit 2 |
| **TeammateIdle** | about to idle | if owned tests are red or assigned work is open → exit 2 (keep working); periodically prompt for a `WORKFLOW.md` note. **Circuit-breaker:** after **N failed green attempts or M minutes on the same red test**, stop looping and message the Lead/human instead — no infinite token-burning loop on an unsolvable problem. |
| **CI-on-main** (not a CC hook — a git/CI job) | push/merge to `main` | host tests + arm-none-eabi compile (Stage-2) + `tidy`; a red `main` blocks further merges |

**Toolchain precondition.** Several gates depend on tools the template marks optional or pins to
specific versions. **S0 hard-verifies** the Orthodoxy plugin (if absent it silently no-ops and every
Orthodoxy gate becomes a false pass) **and confirms an Orthodoxy violation *errors* — fails the host
build — rather than merely warning** (else the TaskCompleted "Orthodoxy clean" check must parse
warnings, not the build exit code); plus `clang-22`/`clang-tidy-22`, `arm-none-eabi-gcc`, `pico-sdk`,
`picotool`, and the serial device/permissions. S0 fails the bootstrap if any are missing.

## Worktrees & PR Protocol (central-repo emulation)

- **One canonical repo, `main` is truth.** All worktrees share its object store; `main` is the
  integration branch. CI runs on `main` after every merge; `main` is kept green.
- **One worktree + branch per stream** (`impl/<mod>`), created off the current `main` via the
  `worktree-pr` skill. Each worktree has its own `build-host/` — build isolation is automatic, no
  per-teammate build-dir scheme needed. A teammate's in-progress source is invisible to others until
  merged, so a broken edit in one lane cannot break another lane's build.
- **Stay current.** Teammates `git merge main` into their branch regularly (at least each time a
  dependency merges, e.g. when `fixed` or the oracle lands), so divergence stays small. Frozen headers
  on `main` keep interface drift near zero.
- **PR = reviewed merge to `main`.** When a stream is green (Stage-1 + format + Orthodoxy), the owner
  opens a PR (branch diff + checklist). `renderer-reviewer` reviews; review gates the merge — the
  standard checkpoint the shared-dir model lacked. This is per-stream review (resolves the old "review
  cadence" question). Review is necessarily **lane-scoped** (one branch diff vs. the contract), so it
  catches Orthodoxy/contract-conformance/lane-local bugs; cross-module *semantic* bugs surface at
  C1/C2 integration, which is why integration is incremental.
- **Delegated merge to avoid the Lead becoming the bottleneck.** If the Lead alone merges every PR,
  five near-simultaneous PRs queue on one actor and erase the parallelism. So **owners self-merge their
  own PR once `renderer-reviewer` approves and CI-on-main is green**; the Lead reserves direct `main`
  authorship for **contract-delta PRs and the C2 barrier** only. The Lead is still `main`'s maintainer
  (owns conflict-resolution policy and the `wave-base` tag) but is not a per-PR serialization point.
- **Failure isolation is real.** A failing/abandoned branch blocks nothing; `main` stays green;
  rollback is dropping a branch or reverting a merge, not resetting everyone's tree. (Concurrent ref
  updates across worktrees can transiently fail on `packed-refs.lock`; the `worktree-pr` skill retries.
  `main` is checked out only in the Lead's worktree; others reach its commits via `git merge main`.)
- **Dependency build cost.** Each worktree's first `cmake` would otherwise re-fetch and rebuild SDL3 +
  ImGui via FetchContent — six times over. S0 sets a shared `FETCHCONTENT_BASE_DIR` (or a prebuilt SDL
  via `find_package` + `FIND_PACKAGE_ARGS`) so the host deps are fetched/built once and reused across
  worktrees.
- **Worktree/branch lifecycle.** Owners keep a **stable worktree across waves** and cut a **new branch
  per stream** off the current `main`; the retro **prunes merged branches and stale worktrees** (`git
  worktree prune`, delete merged branches) so 5 waves × ~6 streams don't accumulate. Build artifacts
  are per-worktree (cleaned with the worktree); the shared `FETCHCONTENT_BASE_DIR` persists.
- **Local vs remote.** The model works entirely locally (branches in one repo). If real `gh` PRs are
  wanted (richer review UI, hosted CI), push branches to a GitHub remote — optional, see Unresolved Q1.

## Continuous-Improvement Loop

Each teammate keeps a `WORKFLOW.md` in its own worktree, appending friction + a concrete suggestion
(skill/hook/agent tweak); the TeammateIdle hook nudges. At each retro the **wave-retro** skill makes
the Lead collate every teammate's `WORKFLOW.md` (gathered from their branches) and ship agent/skill/
hook updates as part of the next contract-delta PR. This gives the "team always suggesting efficiency/
quality improvements" mandate a real mechanism, not exhortation.

**Binding semantics — the workflow config is itself version-controlled and branched.** `.claude/`
(agents/skills/hooks), `settings.json`, and `.claude/ownership.json` live in the repo and load
per-session from each worktree's *branch* tree. Two consequences: (a) a teammate on a stale branch
runs the **old** config until it `git merge main`; (b) session-loaded parts (skills, agent bodies,
CLAUDE.md) bind at **spawn time**, so even after merging, a live teammate may not pick up an improved
skill without a re-spawn (hooks, read per-invocation, do update on merge). Therefore workflow
improvements are **next-wave-spawn** semantics, not live: `.claude/`+`settings.json` change only via
Lead/T5 PRs to `main`, and the improved workflow binds when the next wave's team is spawned. Urgent
mid-wave hook fixes require re-spawning the affected teammate.

**Workflow KPIs (make "more efficient / higher quality" measurable).** The retro tracks a few cheap
metrics so the improvement loop is falsifiable, not just a suggestion box: **PR cycle time**, **CI-on-`main`
red frequency**, **rework-after-review rate**, **on-target gate pass rate**, and **contract-delta count
per wave** (see below). A metric moving the wrong way is itself a retro agenda item.

## Stream A — Contract Freeze (keystone barrier)

**Goal:** land every module's interface on `main` so feature branches all cut from a stable contract.
**Scope:** headers + shared types + CMake subdir registration; no behavior.
**Mechanics:** one PR by the Lead, with owners contributing their own module's `<mod>.h` (each on a
short-lived branch merged into the freeze PR, or committed directly by the Lead from owners' drafts —
the freeze is collaborative but lands as one atomic `main` commit). After it merges, every B-stream
branches from it.
**Cross-module structs live in `src/rdr/types.h` (Lead-owned):** `TransformedVertex`, the tile bin,
arena handles, render-state block, `Command` tagged union, `Vtx` input union, tex/material/light/fog
descriptors. Module headers declare **only that module's own `module_verb` signatures** and include
`rdr/types.h`. This resolves "who owns the shared structs" — the answer is the shared header, not any
module lane.
**Also freezes:** fixed-point formats; sampler/combiner/blend signatures; error-code convention
(0 = ok, errno-like otherwise). **Sizes (`TransformedVertex`, tile bin, arenas) are set by the S0.5
measurements**, not guessed. **Pre-registers** all foundation module subdirs in **both
`src/CMakeLists.txt` and `tests/CMakeLists.txt`** (both are shared conflict hotspots), so feature
branches add only their own module's sources/tests behind already-registered subdirs.
**Verification:** all headers compile; stub `.cc` link; host test skeletons + Orthodoxy canary green;
arm-none-eabi compiles; CI green on `main`.
**Parallelism:** Barrier-Gated. **No implementation branch starts until A is merged to `main`.**
**Re-surface triggers:** any owner finds a frozen signature can't express required behavior →
contract-delta PR before implementing around it; S0.5 numbers force a layout that doesn't fit → Lead
re-freezes before fan-out.
**Contract-churn budget (interface-first's failure mode).** S0.5 de-risks struct *sizing* but not the
*shape* of signatures/fields, so some drift is expected. But **> 2 contract-delta PRs in a single
wave** is the signal that the freeze was premature: pause fan-out and re-examine the contract as a
whole rather than keep patching it (each delta forces every in-flight branch to `git merge main` +
adapt + re-test). Tracked as a workflow KPI.

## Foundation Wave Topology (parallelize-plan schema)

Abbreviated here; the implementation plan expands each into the full per-stream schema (Goal / Scope /
Files Owned / Files Forbidden / Dependencies / Parallelism / Verification / Deliverables / Failure
Isolation / Re-Surface Triggers). **Task granularity:** a *stream* is the dispatchable unit and maps
to one branch + one PR; the team task list carries stream-level tasks with dependencies (encoding the
barriers). The many red-green-refactor micro-steps within a stream are tracked as commits in-lane (the
TDD test list evolves, per Grenning), **not** as separate team tasks — so the TaskCreated schema stays
meaningful and the dependency graph stays legible.

- **S0 bootstrap** (Serialized, Lead+T5): toolchain inventory + hard-verify (incl. Orthodoxy
  errors-not-warns); template → `renderer`; canonical repo + `main`; worktree/PR protocol +
  `worktree-pr` skill; **shared `FETCHCONTENT_BASE_DIR`** so SDL3+ImGui build once across worktrees;
  CI-on-main; agents/skills/hooks; **team+worktree smoke test** (spawn a throwaway teammate in a
  worktree, branch, commit, self-merge after approval — confirm agent-teams-with-worktrees mechanics).
  Blocks all. Verify: host + arm build green, template demo runs.
- **S0.5 hardware spike** (Serialized, T5+T2): throwaway-branch firmware measuring free RAM /
  mul-div + fill throughput / micro-rasterizer + scanout timing; records the sort-middle-vs-fallback
  decision. Branch discarded after. Feeds Stream A's sizes.
- **A contract freeze** (Barrier-Gated, one PR to `main`) — above.
- **B.0 `fixed`** (T1; dep A): Q-format math + multiply discipline + SIO divider; host golden tests.
  Fan-out point. Owners of β/γ/δ branch **immediately** — but "immediate" means scaffolding +
  compile/structure tests against the frozen `fixed.h`; their **numeric golden tests cannot go green
  until B.0 merges and the oracle (ε) merges.** Prioritize B.0 (small, on the critical path); β/γ/δ
  `git merge main` when it lands.
- **B.1-α `cmd`+`arena`** (T4) · **β `geom`+`clip`** (T1) · **γ `raster` flat+Z** (T2) ·
  **δ `tex`-point+`shade`-modulate+`blend`-opaque** (T3): Fully Parallel, each its own branch,
  test-first on host; a failing branch blocks nothing.
- **B.1-ε host harness** (T5): float oracle + golden-image framework + ImGui inspector. **On the
  correctness critical path** — β/γ/δ's golden tests depend on the oracle, so ε merges early alongside
  B.0. · **B.1-ζ minimal asset tool** (T5): mesh + texture passthrough; genuinely independent. ·
  **B.1-η on-target serial runner** (T5): `picotool` flash + `KEY=VALUE` parse + assert; firmware links
  the **picotool reset stub + watchdog** (see On-Device); required before the C gates.
- **C1 single-core integration** (integrator; dep B.0/α/β/γ/δ/ε merged): wire modules through `rdr` on
  a single-core frame path + the **demo app** (below) → static then spinning textured triangle on host
  **and** device. **Automated exit criteria (not "looks right"):** a host **full-frame golden image
  matches the float oracle** within tolerance; an on-target **frame renders + a frame-time sanity number**
  is read over serial; no Stage-2 regression. The integration checkpoint *before* concurrency.
- **C2 concurrency integration** (T4 + integrator; dep C1): dual-core `sched` + scanline-race + frame
  exec. `sched` validated by a **host 2-worker simulation** (queue/bin-merge/watermark logic) **and**
  prioritized on-target runs. Gate: Stage-2 arm compile + on-target **Phase-0 re-confirm** of the S0.5
  numbers under the real pipeline; tag `wave-base`. Re-surface: integrated transform:raster ratio or
  free-RAM contradicts the S0.5 decision → re-open the architecture fork.
- **C3 throughput probe** (perf-profiler; dep C2): the project's headline claim (3000 tris @ 30 FPS /
  100K tri/s) is *aggressive* and gated on texture-bandwidth — and otherwise wouldn't be measured until
  W5, five waves deep. Flood the real single-core+dual-core pipeline with flat triangles, measure
  achievable tri/s on device, and sanity-check against the 100K target **before** investing W2–W5 in
  features. Re-surface: throughput is far under target → re-open architecture/feature scope *now*, not
  at W5, when rework is cheap. (Integrates the riskiest claim early.)
- **Demo app** (`src/demo/`, owned by the integrator): a minimal host+device program that builds a
  command buffer (one textured, lit, spinning triangle) and drives the renderer; consumes a texture from
  B.1-ζ. It is the harness for C1/C2's frame goldens — not a renderer module, so it sits outside T1–T5's
  lanes. The template's `src/app`/`src/game` are reference-only, not extended by the renderer.
- **D retro + plan W2** (Lead+all): run `wave-retro`; collate per-worktree `WORKFLOW.md`; ship workflow
  improvements; draft W2 contract-delta + topology.

## Repeatable Wave Template

contract delta-freeze PR (extend `rdr/types.h` + register new subdirs; ship workflow improvements) →
owner fan-out, one branch per stream behind the frozen deltas → PR-review-merge each to `main` →
incremental integration (single-core checkpoint before any concurrency, then the device/concurrency
gate) → **retro + replan**. (No S0/S0.5 repeat: the bootstrap and hardware spike run once; later waves
inherit a proven model, toolchain, and baseline.)

Sketched waves (detailed just-in-time):
- **W2** transparency (per-tile sort, two-pass) + AA (analytic coverage + edge-gated resolve).
- **W3** color combiner + PRIM/ENV + alpha compare/cutout + fog + ordered RGB dither.
- **W4** tex formats (IA/I, CI+TLUT) + wrap/mirror/clamp + 3-point filter + per-tri mip + lighting
  (N directional + ambient) + specular/env texgen + volume cull + LOD branch + decal-Z.
- **W5** full asset pipeline (swizzle/mip/palette) + demo scene + profiling + perf gate (~3000 tris
  @ 30 FPS) + tuning (tile size, divide cadence, AA footprint, hot-code/textures in SRAM).

## Verification Ladder (TDD stages mapped to streams)

- **Stage 1 (host microcycle):** every code stream; `make test` + sanitizers in the teammate's own
  worktree; hard TaskCompleted gate.
- **Stage 2 (compiler-compat):** `pico` preset arm-none-eabi build + `tidy-pico`; **continuous on
  `main` (CI per merge) + nightly arm-compile-all**, and re-run at the integration barrier.
- **Stage 3/4 (on-target):** `on-target-probe` over USB-CDC serial at C1 (static frame + frame-time
  sanity), C2 (Phase-0 re-confirm), **C3 (throughput vs 100K target)**, and the W5 perf gate; thresholds
  asserted by the runner. Serialized on the single device.
- **Stage 5 (acceptance):** human visual confirmation of AA/tearing/banding on hardware; demo scene
  frame-time assertion (≤33.3 ms) at W5.

### On-Device Mechanics (the "agent-driven" premise + its failure mode)

`on-target-runner` flashes by rebooting the running pico into BOOTSEL via `picotool reboot -f -u`,
which **requires the firmware to link picotool's reset/USB-stdio interface and not be hung.** During
bring-up the firmware *will* crash/loop, and a hung device can't be reset by picotool → it falls back
to a **physical BOOTSEL button press = human in the loop**, exactly when iterating fastest.
Mitigations: every firmware build links the reset stub and arms a **hardware watchdog** (auto-reboots
a hung frame into a known state); the runner waits on USB re-enumeration (poll for the CDC device
path) with a timeout, and on timeout emits a clear "press BOOTSEL" prompt rather than hanging. Device
path/permissions (`dialout`) are verified at S0.

## Risks & Mitigations

- **Merge conflicts in shared files.** The cost of branches replaces shared-tree overwrite races.
  Mitigation: R8 file-partition keeps module code conflict-free; Stream A pre-registers subdirs and
  lands all shared structs in `rdr/types.h`; shared-type/CMake changes only via serialized
  contract-delta PRs through the Lead; teammates `git merge main` frequently to keep deltas small.
- **Integration lag / stale branches.** A long-lived branch drifts from `main`. Mitigation: frozen
  headers minimize interface drift; merge `main` in on every dependency landing; keep streams small
  (one branch ≈ one PR ≈ a few days).
- **Orthodoxy plugin silently absent → gates become false passes.** Mitigation: S0 hard-verifies the
  plugin (and `clang-22`/`tidy-22`/ARM/`pico-sdk`/`picotool`); bootstrap fails if missing.
- **Building the wrong architecture before measuring.** Mitigation: **S0.5 hardware spike precedes
  Stream A**; A freezes sizes from measured proxies; C2 re-confirms under the real pipeline.
- **Big-bang integration.** Wiring all modules + concurrency at one fan-in mixes wiring bugs with
  ordering bugs. Mitigation: **split into C1 (single-core, full-frame golden vs oracle) then C2
  (dual-core + scanline-race + device)** — a real checkpoint before concurrency.
- **`sched`/multicore is not faithfully host-testable.** Host can't reproduce RP2040 memory ordering,
  spinlocks, or DMA/scanout timing — the one stream where Stage-1 is weakest, and it's the integration
  stream. Mitigation: a host **2-worker simulation** for queue/merge/watermark logic + **prioritized
  on-target time**; confined to C2 so the risk isn't smeared across the wave.
- **Lead as merge bottleneck / SPOF.** Mitigation: **owners self-merge after reviewer approval + green
  CI-on-main**; the Lead reserves direct `main` authorship for contract-deltas and the C2 barrier.
- **Workflow config is branched → improvements don't bind live.** Mitigation: treat workflow changes as
  **next-wave-spawn** semantics (Lead/T5 PRs to `main`; bind at next team spawn); re-spawn a teammate
  for urgent mid-wave hook fixes (see Continuous-Improvement Loop).
- **6× dependency builds across worktrees.** Mitigation: shared `FETCHCONTENT_BASE_DIR` set at S0 so
  SDL3+ImGui build once.
- **"Immediate" is partly illusory.** Numeric golden tests for β/γ/δ need real `fixed` (B.0) **and**
  the oracle (B.1-ε) merged to `main`; both are prioritized. Immediate work on the dependent branches
  = scaffolding + compile/structure tests only.
- **Agent-driven flashing reverts to human on a hung device.** Mitigation: reset stub + watchdog on
  every firmware; runner times out with a "press BOOTSEL" prompt (see On-Device Mechanics).
- **Agent-teams + worktrees is a novel combination.** Each teammate must run in its own worktree cwd
  and commit to its branch; the lead merges. Mitigation: the S0 **team+worktree smoke test** proves
  the mechanics before real work; `using-git-worktrees` skill standardizes setup.
- **Agent-team durability over a multi-hour/day wave.** Experimental feature: lead fixed for the
  team's lifetime, one team at a time, resume limits; a dead lead loses teammates (branches + task
  list survive in git). Mitigation: short waves; all state lives in git branches + the task list +
  per-worktree `WORKFLOW.md`, so a fresh team resumes from disk; human re-spawns teammates and points
  each at its existing branch/worktree. **Residual risk — accepted.**
- **Supervision bandwidth for 6 concurrent Opus panes.** Hooks catch syntactic/test failures, not
  semantic wrong-direction work. Mitigation: per-PR review before merge; the human steers; short
  streams bound drift. **Residual — accepted.**
- **Single hardware serializes on-target tests.** Accepted; on-target runs are barrier/perf-gate only,
  not per-stream, so contention is rare.
- **On-target serial flakiness.** Mitigation: `KEY=VALUE\n` line-parseable + re-runnable; runner
  retries + reports raw lines on parse failure; visual checks stay human.
- **Opus-for-all token cost.** Accepted for quality; wave sizing (5–6 streams/wave) bounds concurrency.

## Resolved Decisions

- **Isolation = git worktrees + central-repo/PR workflow** (supersedes shared working tree). `main` is
  truth; one worktree/branch per stream; reviewed PR-merge to `main`; CI keeps `main` green.
- One de-risking spike (S0.5 hardware measurement) precedes Stream A; S0 bootstrap includes a
  team+worktree smoke test (no separate hooks-identity GO/NO-GO — the worktree model doesn't need it).
- Cross-module structs live in `src/rdr/types.h` (Lead); module headers declare only their own verbs.
- Stream A pre-registers all module subdirs and lands shared types, so feature branches never edit the
  parent CMake list or shared header (conflict avoidance).
- Hooks are quality gates only (format per-edit; test+Orthodoxy at TaskCompleted via the build;
  CI-on-main for Stage-2); no PreToolUse ownership block.
- Review is **per-PR** before merge (renderer-reviewer); **owners self-merge after approval + green CI**;
  the Lead reserves direct `main` authorship for contract-deltas and the C2 barrier (not a per-PR SPOF).
- Stage-2 arm compile is continuous (CI-on-main + nightly), not barrier-only.
- **Integration is incremental: C1 (single-core, full-frame golden vs oracle) → C2 (dual-core +
  scanline-race + device) → C3 (throughput probe vs the 100K target).** `sched` is validated by a host
  2-worker simulation + prioritized device time, not host TDD alone. **C3 integrates the riskiest claim
  (performance) early**, before W2–W5 features are built on the architecture.
- **Safety + bounding for the autonomous loop:** a PreToolUse(Bash) **destructive-op guard** (no
  force-push/hard-reset/branch-delete/`rm -rf`) and a TeammateIdle **circuit-breaker** (escalate to
  human after N failed attempts / M minutes, no infinite loop).
- **Contract-churn budget:** > 2 contract-deltas in a wave ⇒ re-examine the freeze, don't keep patching.
- Orthodoxy is **scoped to renderer modules**; the ImGui inspector is exempt (pragmatism carve-out).
- Heavy modules (`raster`/`sched`) sized as **sub-streams** for reassignment; Lead rebalances at check-in.
- Worktrees stable across waves (new branch per stream); retro prunes merged branches/worktrees.
- Workflow KPIs tracked at each retro (PR cycle time, CI-red rate, rework-after-review, on-target pass
  rate, contract-delta count).
- Foundation-wave done = **automated** (host full-frame golden + on-target frame-time + recorded
  architecture decision), not a subjective visual check.
- Demo app lives in `src/demo/` (integrator-owned), outside T1–T5 lanes; template `app`/`game` unused.
- Workflow config (`.claude/`, `settings.json`) is branched: improvements bind at **next-wave spawn**;
  shared SDL3+ImGui build once via `FETCHCONTENT_BASE_DIR`.
- Standing teammates: Lead + T1–T5; reviewer/integrator/perf/on-target are on-demand roles, not panes.
- Stream-level tasks in the team task list (one per branch/PR); TDD micro-steps are in-lane commits.
- Embedded-TDD ladder: Stage-1 per-task hard gate; Stage-2 continuous; Stage-4 at barrier; Stage-5
  human at W5.
- On-device gates agent-driven via `picotool reboot` + reset-stub/watchdog firmware + USB-CDC serial
  `KEY=VALUE\n`; visual checks human; human-BOOTSEL fallback on a hung device.
- tmux split panes; all teammates Opus; `.claude/ownership.json` (conflict manifest); `fixed`
  "immediate"; `KEY=VALUE`.
- Each wave ends in a retro + next-wave planning stream; per-worktree `WORKFLOW.md` + wave-retro drive
  continuous improvement.

## Unresolved Questions

1. **Local branches vs. a real GitHub remote.** The worktree model gives the PR workflow locally
   (branches + review + merge). A GitHub remote adds real `gh` PR review UI and hosted CI at the cost
   of setup + pushing. Which do we use? Decide at S0. *Affects the CI and `worktree-pr` design.*
2. **CI runner for `main`.** Local git hook / `make` target vs a GitHub Actions workflow (only if Q1
   chooses a remote). Decide at S0 alongside Q1.
3. **BOOTSEL automation** — confirm `picotool reboot -f -u` works on this unit without a physical
   button (firmware reset-interface linked), so the agent-driven loop holds. Verify in S0.5.
4. **Serial transport detail** — confirm RP2040 USB-CDC stdio (`pico_enable_stdio_usb`) is the channel
   the runner reads; reconcile with the audio-IRQ on core 0.
5. **Golden-image storage** — commit PNG goldens vs generate-and-cache; affects repo size + harness
   diff path. Decide in B.1-ε.
6. **Contract-freeze PR mechanics** — owners draft headers on short branches merged into one freeze PR,
   vs the Lead authoring all headers from owners' specs. Decide at the start of Stream A.
