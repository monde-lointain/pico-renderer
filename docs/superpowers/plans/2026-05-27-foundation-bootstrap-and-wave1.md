# Foundation Bootstrap, Contract Freeze & Wave-1 Dispatch — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up the renderer project from `../picosystem-template`, build the agent-team workflow infrastructure (agents/skills/hooks/ownership + CI + worktree-PR protocol), measure the hardware so struct layouts are sized not guessed, freeze the module contract, and hand the agent team a dispatch-ready Wave-1 stream topology.

**Architecture:** Interface-first, dual-target TDD (host SDL = Stage-1 microcycle; arm-none-eabi = Stage-2 compile-compat; on-target USB-CDC serial = Stage-4). Parallelism comes from git worktrees + a central-`main`/PR workflow: each teammate owns modules in its own worktree/branch and lands work via reviewed merge to `main`. This plan is the **sequential foundation** (Parts A–C, lead-executed) plus the **parallel dispatch topology** (Part D, team-executed). Full rationale: `docs/superpowers/specs/2026-05-27-agent-team-implementation-plan-design.md`. Renderer design: `docs/superpowers/specs/2026-05-26-command-buffer-renderer-design.md`.

**Tech Stack:** C++20, CMake ≥3.28, Orthodox C++ (Orthodoxy clang plugin), GoogleTest, SDL3 + ImGui (host, FetchContent), pico-sdk + arm-none-eabi-gcc + picotool (device), clang-22/clang-tidy-22, ccache, tmux + Claude Code Agent Teams (`CLAUDE_CODE_EXPERIMENTAL_AGENT_TEAMS=1`).

**Scope of this plan.** Parts A–C produce working, testable software on their own (both targets build green, Orthodoxy canary + smoke tests pass, hardware measurements recorded, contract frozen on `main`). Part D is the dispatch schema for the Wave-1 implementation streams (B.0 → C3); those streams are *executed by the agent team*, each teammate running its own emergent red-green-refactor loop via the `orthodox-tdd-cycle` skill — so they are specified as dispatchable units, not pre-written TDD walkthroughs (the per-module test list emerges during implementation; Grenning ch.4). Waves W2–W5 get their own just-in-time plans after the Wave-1 retro.

---

## File Structure

Created/modified by this plan (paths relative to repo root):

**Workflow infra (Lead/T5-owned):**
- `.claude/agents/{renderer-module-owner,infra-tooling,renderer-reviewer,integrator,perf-profiler,on-target-runner}.md`
- `.claude/skills/{contract-first,orthodox-tdd-cycle,fixed-point,golden-image-test,renderer-module-scaffold,worktree-pr,on-target-probe,wave-retro}/SKILL.md`
- `.claude/ownership.json` — module→owner glob manifest + TaskCreated schema source
- `.claude/settings.json` — hooks (PreToolUse-bash guard, PostToolUse format, TaskCreated, TaskCompleted, TeammateIdle)
- `.claude/hooks/{ownership_guard.sh,destructive_guard.sh,format_check.sh,task_schema.sh,task_complete_gate.sh,idle_gate.sh}` — hook scripts
- `tools/verify_toolchain.sh` — S0 hard-verify; `tools/ci_main.sh` — CI-on-main; `tools/wave_retro.md` — KPI template

**Project (from template, renamed `picosystem_template`→`renderer`, `PICOSYSTEM_TEMPLATE`→`RENDERER`):**
- Top `CMakeLists.txt`, `CMakePresets.json`, `Makefile`, `cmake/*`, `.clang-format`, `.clang-tidy`, `.orthodoxy.yml`, `.gitignore`, `.github/`
- `src/CMakeLists.txt` — pre-registers all renderer module subdirs (Stream A)
- `tests/CMakeLists.txt` — extends the EXISTS-guarded `foreach` with renderer modules
- Module skeleton dirs: `src/{fixed,arena,cmd,geom,clip,raster,aa,tex,shade,blend,sched,rdr}/` each `{<mod>.h,<mod>.cc,CMakeLists.txt}`; mirrored `tests/<mod>/`
- `src/rdr/types.h` — **all cross-module POD structs + fixed-point formats + error codes** (the frozen contract)
- `src/rdr/config.h` — compile-time build config (color format, coverage storage)
- `src/demo/` — integrator-owned demo app (added at C1; subdir pre-registered in Stream A)
- `tests/harness/` — float oracle + golden-image framework + ImGui inspector (T5; **not** Orthodoxy-enforced)
- `tools/spike/` — S0.5 throwaway-firmware measurement program (discarded after S0.5)

---

# PART A — S0 Bootstrap (sequential, Lead + T5)

Goal: a renderer project that builds green on both targets with the full workflow infrastructure in place and verified. Blocks everything.

## Task A1: Toolchain hard-verify script

**Files:**
- Create: `tools/verify_toolchain.sh`
- Test: `tools/verify_toolchain.sh` is its own test (exit 0 = all present)

- [ ] **Step 1: Write the verify script**

```bash
#!/usr/bin/env bash
# Fail loudly if any tool the workflow gates depend on is missing.
set -euo pipefail
fail=0
need() { command -v "$1" >/dev/null 2>&1 || { echo "MISSING: $1 ($2)"; fail=1; }; }
need clang-22         "host compiler (CMakePresets host-asan pins clang-22)"
need clang++-22       "host compiler"
need clang-tidy-22    "host + pico clang-tidy"
need arm-none-eabi-gcc "pico cross-compiler"
need picotool         "device flash + reboot-to-BOOTSEL"
need ccache           "shared object cache across worktrees"
need cmake            "build system (>=3.28)"
need ninja            "pico generator"
need jq               "hook JSON parsing"
# pico-sdk: env or fetch fallback
if [ -z "${PICO_SDK_PATH:-}" ] && [ "${PICO_SDK_FETCH_FROM_GIT:-}" != "ON" ]; then
  echo "MISSING: PICO_SDK_PATH (or PICO_SDK_FETCH_FROM_GIT=ON)"; fail=1
fi
# Serial device (on-target runner)
ls /dev/ttyACM* >/dev/null 2>&1 || echo "WARN: no /dev/ttyACM* yet (device not plugged in / BOOTSEL)"
id -nG | tr ' ' '\n' | grep -qx dialout || echo "WARN: user not in 'dialout' group (serial perms)"
[ "$fail" -eq 0 ] && echo "TOOLCHAIN OK" || { echo "TOOLCHAIN INCOMPLETE"; exit 1; }
```

- [ ] **Step 2: Run it**

Run: `chmod +x tools/verify_toolchain.sh && ./tools/verify_toolchain.sh`
Expected: `TOOLCHAIN OK` (exit 0). If any `MISSING:` line prints, install the tool before proceeding — bootstrap is blocked until this passes. (Orthodoxy plugin presence is verified at A6 via the CMake REQUIRE flag, not here.)

- [ ] **Step 3: Commit**

```bash
git add tools/verify_toolchain.sh
git commit -m "S0: toolchain hard-verify script"
```

## Task A2: Import the template and rename to `renderer`

**Files:**
- Create: the full template tree copied into the repo root (everything except its `.git`)
- Modify: all files containing `picosystem_template` / `PICOSYSTEM_TEMPLATE`

- [ ] **Step 1: Copy the template in (preserving our docs/ and .git)**

Run:
```bash
rsync -a --exclude='.git' --exclude='build*' ../picosystem-template/ .
```
Expected: `CMakeLists.txt`, `src/`, `tests/`, `cmake/`, `Makefile`, `CMakePresets.json`, `.clang-*`, `.orthodoxy.yml`, `.github/`, `tools/` now present alongside `docs/`.

- [ ] **Step 2: Rename project identifiers (excluding docs/build/deps)**

Run:
```bash
grep -rlZ --exclude-dir=.git --exclude-dir=docs --exclude-dir=.deps-cache \
  --exclude-dir=build-host --exclude-dir=build-pico \
  -e picosystem_template -e PICOSYSTEM_TEMPLATE . \
  | xargs -0 sed -i -e 's/picosystem_template/renderer/g' -e 's/PICOSYSTEM_TEMPLATE/RENDERER/g'
```
Expected: no remaining matches outside docs/ — verify: `! grep -rn --exclude-dir=.git --exclude-dir=docs -e picosystem_template -e PICOSYSTEM_TEMPLATE .`. (`docs/` is excluded so the design specs/this plan — which reference the template by name — are not corrupted. `pimoroni_picosystem` / `memmap_picosystem.ld` are safe: the pattern is the underscore-qualified `picosystem_template` only.)

- [ ] **Step 3: Configure + build host**

**Do Task A3's `FETCHCONTENT_BASE_DIR` preset edit BEFORE this first configure** — otherwise SDL3/ImGui are fetched+built into `build-host/` here and *again* into `.deps-cache/` at A3 (a wasted full SDL3 build). With the base dir set first, this build populates the shared cache directly.

Run: `cmake --preset host && cmake --build --preset host -j"$(nproc)"`
Expected: configure + build succeed; `build-host/src/renderer` exists.

- [ ] **Step 4: Run the template's tests**

Run: `ctest --preset host --output-on-failure`
Expected: all template smoke/unit tests PASS.

- [ ] **Step 5: Configure + build pico (Stage-2 baseline)**

Run: `cmake --preset pico && cmake --build --preset pico -j"$(nproc)"`
Expected: `build-pico/src/renderer.uf2` produced.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "S0: import picosystem-template, rename -> renderer; both targets green"
```

## Task A3: Shared FetchContent base dir (one SDL3/ImGui build across worktrees)

> **Ordering:** execute this task's Step 1 **before** Task A2 Step 3 (the first host build), so SDL3/ImGui build once into the shared cache rather than twice. A2 and A3 are otherwise independent.

**Files:**
- Modify: `CMakePresets.json` (host + host-asan presets)

- [ ] **Step 1: Add a shared deps cache dir to the host presets**

In `CMakePresets.json`, add to the `host` and `host-asan` preset `cacheVariables`:
```json
"FETCHCONTENT_BASE_DIR": "${sourceDir}/.deps-cache"
```
And add `/.deps-cache/` to `.gitignore`.

- [ ] **Step 2: Reconfigure and confirm deps resolve to the shared dir**

Run: `rm -rf build-host && cmake --preset host`
Expected: SDL3/ImGui sources fetched under `.deps-cache/` (not `build-host/`); `ls .deps-cache` shows the dependency dirs.

- [ ] **Step 3: Commit**

```bash
git add CMakePresets.json .gitignore
git commit -m "S0: shared FETCHCONTENT_BASE_DIR so worktrees build SDL3/ImGui once"
```

## Task A4: Destructive-op guard hook (TDD)

**Files:**
- Create: `.claude/hooks/destructive_guard.sh`
- Test: `tests/hooks/test_destructive_guard.sh`

- [ ] **Step 1: Write the failing test**

```bash
#!/usr/bin/env bash
# tests/hooks/test_destructive_guard.sh
set -u
H=.claude/hooks/destructive_guard.sh
run() { echo "{\"tool_input\":{\"command\":\"$1\"}}" | "$H"; echo $?; }
fail=0
# Blocked (exit 2):
for c in "git push --force origin main" "git reset --hard HEAD~3" "git branch -D impl/tex" \
         "git clean -fdx" "rm -rf src/"; do
  [ "$(run "$c")" = "2" ] || { echo "NOT BLOCKED: $c"; fail=1; }
done
# Allowed (exit 0):
for c in "git commit -m x" "git merge main" "make test" "rm -rf build-host/CMakeFiles"; do
  [ "$(run "$c")" = "0" ] || { echo "WRONGLY BLOCKED: $c"; fail=1; }
done
[ "$fail" -eq 0 ] && echo PASS || echo FAIL
```

- [ ] **Step 2: Run it to verify it fails**

Run: `chmod +x tests/hooks/test_destructive_guard.sh && ./tests/hooks/test_destructive_guard.sh`
Expected: FAIL (script missing → all `run` calls error, "NOT BLOCKED" lines print).

- [ ] **Step 3: Write the hook**

```bash
#!/usr/bin/env bash
# .claude/hooks/destructive_guard.sh — PreToolUse(Bash). exit 2 = block.
cmd="$(jq -r '.tool_input.command // ""')"
deny_re='git[[:space:]]+push([[:space:]].*)?[[:space:]]--force|--force-with-lease|git[[:space:]]+reset[[:space:]]+--hard|git[[:space:]]+branch[[:space:]]+-D|git[[:space:]]+clean[[:space:]]+-[a-z]*f[a-z]*d|git[[:space:]]+push[[:space:]].*:[^[:space:]]*$|rebase[[:space:]]+.*--force|filter-branch'
# rm -rf is allowed only inside a build dir
if echo "$cmd" | grep -Eq 'rm[[:space:]]+-[a-z]*r[a-z]*f' && ! echo "$cmd" | grep -Eq 'build-|/build|\.deps-cache'; then
  echo "BLOCKED: rm -rf outside a build dir" >&2; exit 2
fi
if echo "$cmd" | grep -Eq "$deny_re"; then
  echo "BLOCKED destructive op: $cmd" >&2; exit 2
fi
exit 0
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `./tests/hooks/test_destructive_guard.sh`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
chmod +x .claude/hooks/destructive_guard.sh
git add .claude/hooks/destructive_guard.sh tests/hooks/test_destructive_guard.sh
git commit -m "S0: destructive-op PreToolUse guard + test"
```

## Task A5: Ownership manifest + TaskCreated schema gate (TDD)

**Files:**
- Create: `.claude/ownership.json`, `.claude/hooks/task_schema.sh`
- Test: `tests/hooks/test_task_schema.sh`

- [ ] **Step 1: Write the ownership manifest**

```json
{
  "wave": "W1-foundation",
  "owners": {
    "T1": ["src/fixed/**", "tests/fixed/**", "src/geom/**", "tests/geom/**", "src/clip/**", "tests/clip/**"],
    "T2": ["src/raster/**", "tests/raster/**", "src/aa/**", "tests/aa/**"],
    "T3": ["src/tex/**", "tests/tex/**", "src/shade/**", "tests/shade/**", "src/blend/**", "tests/blend/**"],
    "T4": ["src/cmd/**", "tests/cmd/**", "src/arena/**", "tests/arena/**", "src/sched/**", "tests/sched/**", "src/platform/**", "tests/platform/**"],
    "T5": ["tests/harness/**", "tools/**", ".claude/**", ".github/**"],
    "Lead": ["src/rdr/**", "src/demo/**", "CMakeLists.txt", "src/CMakeLists.txt", "tests/CMakeLists.txt", "CMakePresets.json"]
  }
}
```

- [ ] **Step 2: Write the failing schema-gate test**

```bash
#!/usr/bin/env bash
# tests/hooks/test_task_schema.sh — TaskCreated requires files_owned + branch + verification
set -u
H=.claude/hooks/task_schema.sh
ok='{"task":{"files_owned":["src/raster/**"],"branch":"impl/raster","verification":"make test"}}'
bad='{"task":{"branch":"impl/raster"}}'
[ "$(echo "$ok"  | "$H"; echo $?)" = "0" ] && [ "$(echo "$bad" | "$H"; echo $?)" = "2" ] \
  && echo PASS || echo FAIL
```

- [ ] **Step 3: Run it to verify it fails**

Run: `chmod +x tests/hooks/test_task_schema.sh && ./tests/hooks/test_task_schema.sh`
Expected: FAIL (script missing).

- [ ] **Step 4: Write the schema gate**

```bash
#!/usr/bin/env bash
# .claude/hooks/task_schema.sh — TaskCreated. exit 2 = reject.
t="$(cat)"
for k in files_owned branch verification; do
  v="$(echo "$t" | jq -r ".task.$k // empty")"
  [ -n "$v" ] || { echo "TASK REJECTED: missing .task.$k" >&2; exit 2; }
done
exit 0
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `./tests/hooks/test_task_schema.sh`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
chmod +x .claude/hooks/task_schema.sh
git add .claude/ownership.json .claude/hooks/task_schema.sh tests/hooks/test_task_schema.sh
git commit -m "S0: ownership manifest + TaskCreated schema gate + test"
```

## Task A6: Remaining hook scripts + settings.json wiring

**Files:**
- Create: `.claude/hooks/{format_check.sh,task_complete_gate.sh,idle_gate.sh}`
- Create: `.claude/settings.json`

- [ ] **Step 1: Write format_check.sh (PostToolUse, advisory)**

```bash
#!/usr/bin/env bash
# .claude/hooks/format_check.sh — PostToolUse(Edit|Write). Advisory (exit 0 always).
f="$(jq -r '.tool_input.file_path // empty')"
case "$f" in *.cc|*.h) clang-format --dry-run --Werror "$f" 2>&1 | sed 's/^/[format] /' ;; esac
exit 0
```

- [ ] **Step 2: Write task_complete_gate.sh (TaskCompleted, hard gate — module-scoped)**

The gate runs only the **owning module's** tests (`ctest -L <mod>`), not the whole suite — otherwise another merged module's failure (or a flaky test pulled in via `git merge main`) would block this owner's unrelated task, and the gate would slow down every wave. The full suite is CI-on-main's job. `MODULE` is exported in the teammate's spawn env (Task at launch); fall back to the whole suite only if unset.

```bash
#!/usr/bin/env bash
# .claude/hooks/task_complete_gate.sh — TaskCompleted. exit 2 = block completion.
# Runs in the teammate's own worktree (cwd). Orthodoxy runs inside the host build.
set -uo pipefail
cmake --build --preset host -j"$(nproc)" >/tmp/tc_build.log 2>&1 || {
  echo "GATE FAIL: host build broken:" >&2; tail -20 /tmp/tc_build.log >&2; exit 2; }
if [ -n "${MODULE:-}" ]; then SEL=(-L "$MODULE"); else SEL=(); fi   # module-scoped, else full
if ! ctest --preset host "${SEL[@]}" --output-on-failure >/tmp/tc_test.log 2>&1; then
  echo "GATE FAIL: ${MODULE:-all} tests not green:" >&2; tail -20 /tmp/tc_test.log >&2; exit 2
fi
if ! make format-patch >/tmp/tc_fmt.log 2>&1; then
  echo "GATE FAIL: clang-format dirty (run 'make format')" >&2; exit 2
fi
exit 0
```
(Each module's `tests/<mod>/CMakeLists.txt` sets `LABELS <mod>` on its `gtest_discover_tests` so `-L <mod>` selects it — add that to the `renderer-module-scaffold` skill.)

- [ ] **Step 3: Write idle_gate.sh (TeammateIdle, blocked-aware keep-working + circuit-breaker)**

The gate must distinguish **legitimately blocked** (e.g. β/γ/δ waiting for B.0/ε to merge — they're *expected* to have absent/red numeric tests) from **slacking**. It keys off the **owning module's** tests only, and treats a teammate whose blocking dependency is unmerged as allowed-to-idle (it's waiting, not slacking). `MODULE` and `BLOCKED_ON` (a branch name, or empty) come from the spawn env.

```bash
#!/usr/bin/env bash
# .claude/hooks/idle_gate.sh — TeammateIdle. exit 2 = keep working (with feedback).
ATT=.git_idle_attempts; n=$(cat "$ATT" 2>/dev/null || echo 0)
# Blocked on an unmerged dependency? Idling is correct — don't loop.
if [ -n "${BLOCKED_ON:-}" ] && ! git merge-base --is-ancestor "origin/${BLOCKED_ON}" HEAD 2>/dev/null \
   && ! git rev-parse --verify --quiet "${BLOCKED_ON}" >/dev/null; then
  echo "Blocked on ${BLOCKED_ON} (unmerged). Idling OK — Lead will unblock." >&2; rm -f "$ATT"; exit 0
fi
SEL=(); [ -n "${MODULE:-}" ] && SEL=(-L "$MODULE")
if ctest --preset host "${SEL[@]}" >/dev/null 2>&1; then rm -f "$ATT"; exit 0; fi  # green -> idle OK
n=$((n+1)); echo "$n" > "$ATT"
if [ "$n" -ge 5 ]; then
  echo "CIRCUIT-BREAKER: ${MODULE:-owned} tests red after $n attempts — escalate to Lead/human." >&2
  rm -f "$ATT"; exit 0   # surface to human rather than burn tokens
fi
echo "${MODULE:-owned} tests red (attempt $n/5). Keep working; if stuck, message the Lead." >&2; exit 2
```
(Add `.git_idle_attempts` to `.gitignore` so it is never committed.)

- [ ] **Step 4: Write settings.json wiring the hooks**

```json
{
  "env": { "CLAUDE_CODE_EXPERIMENTAL_AGENT_TEAMS": "1" },
  "hooks": {
    "PreToolUse":  [{ "matcher": "Bash",        "hooks": [{ "type": "command", "command": ".claude/hooks/destructive_guard.sh" }] }],
    "PostToolUse": [{ "matcher": "Edit|Write",  "hooks": [{ "type": "command", "command": ".claude/hooks/format_check.sh" }] }],
    "TaskCreated":   [{ "hooks": [{ "type": "command", "command": ".claude/hooks/task_schema.sh" }] }],
    "TaskCompleted": [{ "hooks": [{ "type": "command", "command": ".claude/hooks/task_complete_gate.sh" }] }],
    "TeammateIdle":  [{ "hooks": [{ "type": "command", "command": ".claude/hooks/idle_gate.sh" }] }]
  }
}
```

- [ ] **Step 5: Verify Orthodoxy is active, errors-not-warns, and the canary doesn't break the default build**

Run: `cmake --preset host -DRENDERER_REQUIRE_ORTHODOXY=ON 2>&1 | tee /tmp/orth.log`
Expected: configure SUCCEEDS with `Orthodoxy plugin found: enforcement enabled`. If it FATAL-errors "plugin is not active", the plugin is missing — install per `docs/setup-llvm.md` before proceeding (a silent no-op would make every Orthodoxy gate a false pass).

Then confirm the **canary is `EXCLUDE_FROM_ALL`** so a normal build doesn't fail on it (the canary is *designed* to fail to compile when the plugin is live):
```bash
cmake --build --preset host -j"$(nproc)"            # must SUCCEED (canary excluded from 'all')
! cmake --build build-host --target orthodoxy_canary # must FAIL to compile (enforcement proven)
```
Expected: the default build succeeds; the explicit canary target fails to compile. If the default build fails *because of* the canary, the template's canary is in `all` — fix its CMake to `EXCLUDE_FROM_ALL` (verify `tests/orthodoxy_canary/CMakeLists.txt`) and have `ci_main.sh` run the expect-fail step. This same two-part check is reused at C2 Step 4.

- [ ] **Step 6: Commit**

```bash
chmod +x .claude/hooks/*.sh
echo ".git_idle_attempts" >> .gitignore
git add .claude/hooks/ .claude/settings.json .gitignore
git commit -m "S0: format/task-complete/idle hooks + settings.json (Orthodoxy REQUIRE + canary verified)"
```

## Task A7: Custom skills

> **Granularity:** this task (and A8, and C2's scaffolding) is larger than the skill's 2–5-minute ideal. For fine-grained subagent-per-task execution, **split into one sub-task per skill file** (author → verify frontmatter → commit), iterating the loop below per skill. Kept as one task here only for plan readability.

**Files:**
- Create: `.claude/skills/<name>/SKILL.md` for the eight skills in the spec.

- [ ] **Step 1: Author the eight skill files (one commit each)**

Each `SKILL.md` has YAML frontmatter (`name`, `description`) + body. Author full content for each — these are the durable workflow assets, not placeholders. Required bodies (one file each):

- `contract-first/SKILL.md` — procedure: define `<mod>.h` POD structs (from `rdr/types.h`) + `module_verb(Receiver*, …)` signatures + errno-like return codes; confirm header compiles + a stub `.cc` links *before* writing behavior.
- `orthodox-tdd-cycle/SKILL.md` (rigid) — red → run-fails → minimal green → run-passes → refactor-on-green → commit; bound to `make test`/`make format`/`make tidy`; Orthodox C++ idioms (struct-not-class, pointers-not-refs, C-casts, no-auto/lambda/exceptions, `malloc`/`free`); "do you have a test for that?"; copy-don't-cut extraction.
- `fixed-point/SKILL.md` — Q16.16 transform, Q12.4 screen, 1/w depth; no-`UMULL` discipline (16×16 partials via `MULS`); SIO divider for perspective; floats only in setup/asset tools.
- `golden-image-test/SKILL.md` — write a float-oracle reference, compare fixed-point output bit/tolerance; PNG golden compare; inject-an-error to confirm the test is wired before trusting green.
- `renderer-module-scaffold/SKILL.md` — emit `src/<mod>/{<mod>.h,<mod>.cc,CMakeLists.txt}` + `tests/<mod>/{<mod>_test.cc,CMakeLists.txt}` with `Renderer_<Mod>` target + `Renderer::<Mod>` alias, `project_warnings` link, `orthodoxy_enforce(Renderer_<Mod>)`, `gtest_discover_tests`. Assumes the subdir is pre-registered (Stream A).
- `worktree-pr/SKILL.md` — `git worktree add ../wt-<mod> -b impl/<mod> main`; keep current via `git merge main`; open PR (branch diff + checklist); self-merge after reviewer approval + green CI; retry on `packed-refs.lock`.
- `on-target-probe/SKILL.md` — build `.uf2` → `picotool reboot -f -u` (fallback: prompt "press BOOTSEL") → poll `/dev/ttyACM*` → read `KEY=VALUE\n` → assert thresholds; emit the human visual-check protocol.
- `wave-retro/SKILL.md` — collate per-worktree `WORKFLOW.md`; compute the workflow KPIs (PR cycle time, CI-red rate, rework-after-review, on-target pass rate, contract-delta count); ship agent/skill/hook updates as the next contract-delta PR; draft the next wave's topology.

- [ ] **Step 2: Verify the skills load**

Run: list `.claude/skills/*/SKILL.md`; confirm each has valid frontmatter (`name:` matches dir).
Expected: 8 files, each parseable.

- [ ] **Step 3: Commit**

```bash
git add .claude/skills/
git commit -m "S0: custom skills (contract-first, orthodox-tdd-cycle, fixed-point, golden-image-test, scaffold, worktree-pr, on-target-probe, wave-retro)"
```

## Task A8: Custom agent definitions

> **Granularity:** as with A7, split into one sub-task per agent file for fine-grained execution.

**Files:**
- Create: `.claude/agents/<name>.md` for the six agents in the spec.

- [ ] **Step 1: Author the six agent files (one commit each)**

Each has frontmatter (`description`, `model: opus`, `tools:`) + a body that (since teammate frontmatter `skills` are ignored) **names the skills to use explicitly** and cites `CLAUDE.md` + both specs. Files:
- `renderer-module-owner.md` (T1–T4) — body: use `contract-first` then `orthodox-tdd-cycle`; host-first; cite `fixed-point` + `golden-image-test`; stay in owned globs; PR via `worktree-pr`.
- `infra-tooling.md` (T5) — body: build harness/oracle/inspector + asset tool + serial runner + CI; author skills/hooks.
- `renderer-reviewer.md` — body: review one branch diff vs `rdr/types.h` + spec; coverage-first finding mode (report all findings w/ confidence+severity).
- `integrator.md` — body: C1/C2/C3; wire via `rdr` façade; own `build-pico*/`, `wave-base` tag; demo app.
- `perf-profiler.md` — body: on-target cycle counters; C3 throughput probe; perf-gate levers.
- `on-target-runner.md` — body: the `on-target-probe` mechanics.

- [ ] **Step 2: Commit**

```bash
git add .claude/agents/
git commit -m "S0: agent definitions (module-owner, infra, reviewer, integrator, perf, on-target)"
```

## Task A9: CI-on-main

**Files:**
- Create: `tools/ci_main.sh`; if a GitHub remote is chosen (Unresolved Q1), also `.github/workflows/ci.yml`

- [ ] **Step 1: Write the CI script**

```bash
#!/usr/bin/env bash
# tools/ci_main.sh — run on every merge to main. Keeps main green.
set -euo pipefail
# Hook self-tests: a broken enforcement hook must fail CI, not regress silently.
for t in tests/hooks/test_*.sh; do echo "[$t]"; bash "$t" | grep -q '^PASS$' || { echo "HOOK TEST FAIL: $t"; exit 1; }; done
cmake --preset host -DRENDERER_REQUIRE_ORTHODOXY=ON
cmake --build --preset host -j"$(nproc)"                  # canary is EXCLUDE_FROM_ALL
! cmake --build build-host --target orthodoxy_canary 2>/dev/null && echo "orthodoxy canary correctly fails to compile"
ctest --preset host --output-on-failure                   # full suite (CI's job, not per-task)
cmake --preset pico            # Stage-2: arm-none-eabi compile-compat
cmake --build --preset pico -j"$(nproc)"
echo "CI-MAIN GREEN"
```

- [ ] **Step 2: Run it on the current main**

Run: `chmod +x tools/ci_main.sh && ./tools/ci_main.sh`
Expected: `CI-MAIN GREEN`.

- [ ] **Step 3: Commit**

```bash
git add tools/ci_main.sh
git commit -m "S0: CI-on-main (host tests + Orthodoxy REQUIRE + arm compile-compat)"
```

## Task A10: Team + worktree smoke test

**Files:** none created; this is an operational verification of the novel agent-teams-+-worktrees combination.

- [ ] **Step 1: Create a throwaway worktree + branch**

Run: `git worktree add ../wt-smoke -b smoke/test main && ls ../wt-smoke`
Expected: a second working tree on branch `smoke/test`, sharing this repo's object store.

- [ ] **Step 2: Spawn a teammate in that worktree, make a trivial commit**

With the team feature enabled, spawn one teammate (cwd `../wt-smoke`, a **sibling** of the repo dir), have it create `SMOKE.md`, commit on `smoke/test`. Confirm: (a) the teammate **loads project config from the sibling worktree** — `.claude/` skills+hooks, `settings.json`, `CLAUDE.md` are all in effect (not just the repo's primary tree); (b) the PreToolUse/PostToolUse/TaskCompleted hooks fire in the teammate's session; (c) the teammate's commit lands on its branch only; (d) the Lead can `git merge smoke/test` into `main`.
Expected: config loads from the sibling worktree; hooks observed firing; branch isolation confirmed; merge clean. **If config does NOT load from a sibling worktree**, site worktrees where the harness treats them as in-project (or pass explicit config paths at spawn) before launching the real team.

- [ ] **Step 3: Tear down**

Run: `git worktree remove ../wt-smoke && git branch -d smoke/test && git rm -f SMOKE.md && git commit -m "S0: remove smoke artifact" 2>/dev/null || true`
Expected: worktree gone. Record the smoke result (which hooks fired) in `WORKFLOW.md`.

**Planned NO-GO branch — if hooks do NOT fire for teammate sessions:** the TaskCompleted/idle/format hooks evaporate for teammates (PostToolUse/PreToolUse may also not fire). Do **not** improvise. Adopt this written degraded mode and record the decision: (1) the **TaskCompleted hard gate moves to CI-on-main** — a PR cannot merge unless `tools/ci_main.sh` is green, which now also runs the *owning module's* `ctest -L <mod>` and `make format-patch`; (2) **PR review becomes mandatory and blocking** (no self-merge without `renderer-reviewer` approval); (3) the destructive-op guard, if it also doesn't fire, is replaced by **branch-protection on `main`** (no force-push/delete) plus the rule that teammates only ever operate on their own `impl/*` branch. This keeps every gate enforced at the merge boundary instead of per-edit; it is slower-feedback but sound. (This is the relocated S-1 GO/NO-GO decision.)

- [ ] **Step 4: Commit the bootstrap completion**

```bash
git add -A && git commit -m "S0 complete: workflow infra verified; both targets green" || true
```

---

# PART B — S0.5 Hardware Spike (sequential, T5 + T2)

Goal: measure free RAM, fixed-point throughput proxies, and scanout timing on the real device so Stream A sizes layouts from data. Throwaway — the branch is discarded after measurement.

## Task B1: Spike firmware on a throwaway branch

**Files:**
- Create (on branch `spike/hw-probe`, never merged to `main`): `tools/spike/spike_main.cc`, `tools/spike/CMakeLists.txt`

- [ ] **Step 1: Branch**

Run: `git worktree add ../wt-spike -b spike/hw-probe main`

- [ ] **Step 2: Write the probe firmware**

`tools/spike/spike_main.cc` — a pico program (links pico-sdk, `pico_enable_stdio_usb`, **picotool reset stub + hardware watchdog**) that prints over USB-CDC serial, one `KEY=VALUE` per line:
- `RAM_FREE=<bytes>` — measured by allocating until failure / reading linker symbols (`__StackLimit - __bss_end__`), minus a 115 KB framebuffer reservation.
- `MULDIV_NS=<ns>` — time N fixed-point `fx_mul` + SIO-divider `fx_div` ops (proxy for transform cost).
- `FILL_MBPS=<mbps>` — time a 240×240×2 memset/DMA (proxy for raster/scanout fill bandwidth).
- `MICRORAST_TRIS_PER_S=<n>` — time a tiny flat-triangle inner loop over a tile (proxy for raster cost).
- `SCANOUT_US=<us>` — time one ST7789 DMA flush.
- Final line `PROBE_DONE`.

- [ ] **Step 3: Build + flash + read**

Run: build the spike `.uf2`, then use the `on-target-probe` skill: `picotool load -x …`, poll `/dev/ttyACM*`, capture lines until `PROBE_DONE`.
Expected: all six `KEY=VALUE` lines captured (re-run on parse failure; press BOOTSEL if hung).

- [ ] **Step 4: Record measurements + architecture decision**

Append the captured values + the **sort-middle vs immediate-forward+8-bit-Z decision** (derived from `RAM_FREE` and the `MULDIV_NS`/`MICRORAST_TRIS_PER_S` ratio per the renderer spec) to `docs/superpowers/specs/hardware-measurements.md` on `main` (Lead commits this one file).

- [ ] **Step 5: Discard the spike branch**

Run: `git worktree remove ../wt-spike && git branch -D spike/hw-probe`
Expected: spike code gone; only the recorded measurements survive on `main`.

```bash
git add docs/superpowers/specs/hardware-measurements.md
git commit -m "S0.5: record on-device RAM/throughput/timing + architecture decision"
```

---

# PART C — Stream A: Contract Freeze (barrier; Lead + owners)

Goal: land all module headers + shared types on `main` as one PR so every Wave-1 branch cuts from a stable contract. Sized by the S0.5 measurements.

## Task C1: `src/rdr/types.h` — all cross-module structs + formats + error codes

**Files:**
- Create: `src/rdr/types.h`, `src/rdr/config.h`

- [ ] **Step 1: Write the shared contract header**

`src/rdr/types.h` — POD only, C headers, no behavior. Includes (sizes from S0.5):
```c
#include <stdint.h>
// ---- error codes ---- (0 = ok, errno-like otherwise)
enum RdrErr { RDR_OK = 0, RDR_ENOMEM = 1, RDR_EOVERFLOW = 2, RDR_EINVAL = 3 };
// ---- fixed-point formats ----
typedef int32_t fx16_16;   // matrices / transform
typedef int32_t fx12_4;    // screen coords (subpixel)
typedef int32_t fx_invw;   // 1/w depth
// ---- input vertex (GBI Vtx-union style) ----
struct Vtx { int16_t pos[3]; int16_t uv[2]; union { uint8_t rgba[4]; struct { int8_t n[3]; uint8_t a; } nrm; } c; };
// ---- compact transformed vertex (~14B; sized by S0.5) ----
struct TVtx { fx12_4 x, y; fx_invw inv_w; int16_t u_iw, v_iw; uint16_t rgba; };
// ---- tile bin, render-state block, descriptors, Command tagged union ----
// (full field lists per the renderer design spec §"Key Data Structures")
```
Add the remaining structs (tile bin ref, render-state block, `Command` tagged union with all opcodes, tex/material/light/fog descriptors) with concrete fields per the renderer spec. `src/rdr/config.h` — compile-time `RDR_COLOR_565`/`RDR_COLOR_4444` + coverage-storage switches.

- [ ] **Step 2: Verify it compiles standalone (host + arm)**

Run: `echo '#include "rdr/types.h"
int main(){return 0;}' > /tmp/t.c && clang++-22 -I src -c /tmp/t.c -o /tmp/t.o && arm-none-eabi-gcc -I src -c /tmp/t.c -o /tmp/t.arm.o`
Expected: compiles on both (POD-only, no STL).

- [ ] **Step 3: Commit (into the freeze PR branch)**

```bash
git add src/rdr/types.h src/rdr/config.h
git commit -m "Stream A: freeze shared contract (rdr/types.h, config.h) sized from S0.5"
```

## Task C2: Module header + stub skeletons; pre-register subdirs

**Files:**
- Create: for each `<mod>` in `fixed arena cmd geom clip raster aa tex shade blend sched`: `src/<mod>/{<mod>.h,<mod>.cc,CMakeLists.txt}` and `tests/<mod>/{<mod>_test.cc,CMakeLists.txt}`
- Create: `src/rdr/{rdr.h,rdr.cc,CMakeLists.txt}`, `src/demo/` placeholder
- Modify: `src/CMakeLists.txt`, `tests/CMakeLists.txt`

- [ ] **Step 1: Scaffold each module (header + stub + CMake), one commit per module**

Use the `renderer-module-scaffold` skill per module (split one-module-per-sub-task for fine-grained execution). Each `<mod>.h`: `#include "rdr/types.h"` + the module's `module_verb(Receiver*, …)` signatures (per the spec's Modules section, returning `RdrErr`). Each `<mod>.cc`: stub bodies returning `RDR_OK`/zero. Each `CMakeLists.txt`: `add_library(Renderer_<Mod>)` + `Renderer::<Mod>` alias + `target_sources` + `target_include_directories(... PUBLIC src)` + `orthodoxy_enforce(Renderer_<Mod>)` + link `project_warnings`/`sanitizers`. Each `tests/<mod>/CMakeLists.txt` sets `LABELS <mod>` on `gtest_discover_tests` (so the per-task gate's `ctest -L <mod>` selects it).

The **`rdr` façade and the `demo` placeholder must be buildable stubs at Stream A** (the subdirs are pre-registered next step; an empty dir would break configure):
- `src/demo/CMakeLists.txt`: `add_executable(renderer_demo ...)` (host) / pico exe, linking `Renderer::rdr`; `src/demo/main.cc`: a trivial `int main(){ return 0; }` (real command-buffer demo code lands in C1). The demo is **not** Orthodoxy-enforced (it's app-level, like the template's `app`).
- `orthodoxy_enforce` is applied **only to renderer module libs**, never to `tests/harness/` (ImGui) or `src/demo/` — the ImGui/demo Orthodoxy carve-out.

- [ ] **Step 2: Pre-register all subdirs in the shared CMake files**

In `src/CMakeLists.txt` add `add_subdirectory(<mod>)` for every module + `rdr` + `demo`. In `tests/CMakeLists.txt` extend the `foreach(sub …)` list to include every renderer module (EXISTS-guard already lets not-yet-populated dirs pass). This is the conflict-avoidance pre-registration — feature branches never touch these files again.

- [ ] **Step 3: Configure + build both targets (headers compile, stubs link)**

Run: `cmake --preset host && cmake --build --preset host -j"$(nproc)" && cmake --preset pico && cmake --build --preset pico -j"$(nproc)"`
Expected: all module libs build (empty stubs) on host AND arm; `renderer.uf2` still produced.

- [ ] **Step 4: Orthodoxy canary still green; smoke tests pass**

Run: `cmake --preset host -DRENDERER_REQUIRE_ORTHODOXY=ON && cmake --build --preset host -j"$(nproc)" && ctest --preset host --output-on-failure`
Expected: PASS (canary fails-to-compile as designed only in its isolated target; suite green).

- [ ] **Step 5: Commit + open the freeze PR + merge to main**

```bash
git add src/ tests/CMakeLists.txt src/CMakeLists.txt
git commit -m "Stream A: module headers + stubs + subdir pre-registration; both targets green"
```
Then: review the freeze PR (renderer-reviewer), merge to `main`, run `tools/ci_main.sh` → `CI-MAIN GREEN`. **This is the barrier — no implementation branch starts until this is on `main`.**

---

# PART C→D HANDOFF — Launch the Wave-1 Team & Seed the Task List (Lead)

The pivotal transition: with the contract on `main`, the Lead brings up the agent team and turns the Part-D streams into a dependency-encoded shared task list. Without this, nothing in Part D runs.

## Task H1: Create per-owner worktrees

- [ ] **Step 1: One worktree + branch per owner stream, cut from frozen `main`**

Run (via the `worktree-pr` skill):
```bash
for s in fixed cmd-arena geom-clip raster shading harness assets on-target-runner platform; do
  git worktree add "../wt-$s" -b "impl/$s" main
done
git worktree list
```
Expected: nine worktrees on `impl/*` branches, all based on the Stream-A `main` commit.

## Task H2: Spawn the team in tmux (Opus), one teammate per owner

- [ ] **Step 1: Launch teammates with agent type, worktree cwd, and env**

Bring up the team (`CLAUDE_CODE_EXPERIMENTAL_AGENT_TEAMS=1`, tmux split panes). Spawn five standing teammates, each with: its **agent type** (`renderer-module-owner` for T1–T4, `infra-tooling` for T5), **cwd = its worktree**, and **env** `MODULE=<mod>` + `BLOCKED_ON=<dep-branch-or-empty>` (the gates read these). Spawn prompt per PROMPT_GUIDELINES: role, owned globs (from `.claude/ownership.json`), branch, the exact verification commands, and success criteria up front. Example (T2):

> "You are T2, the Rasterization owner (`renderer-module-owner` agent). Worktree: `../wt-raster`, branch `impl/raster`, env `MODULE=raster`. You own `src/raster/**`, `src/aa/**` and their tests — touch nothing else. Implement `raster` (flat fill + per-tile Z) test-first via the `orthodox-tdd-cycle` skill against the frozen `src/rdr/types.h`; golden-test tile output vs the oracle once `impl/fixed` and `impl/harness` are on `main` (`git merge main`). Land via the `worktree-pr` skill: PR → `renderer-reviewer` approval + green CI → self-merge. Done = `impl/raster` merged, `ctest -L raster` green, arm-compiles on CI."

Expected: five teammate panes, each in its worktree, hooks confirmed firing (per A10).

## Task H3: Seed the shared task list with the Part-D streams + dependency edges

- [ ] **Step 1: Create one task per stream, with dependencies encoding the barriers**

The Lead creates the team tasks (each carries `files_owned` + `branch` + `verification`, per the TaskCreated schema gate) with these dependency edges so the task system enforces the barriers:
- `A` (done) → `B.0`, `B.1-α`, `B.1-ε`, `B.1-ζ`, `B.1-η`, `B.1-θ` (all unblocked now).
- `B.0` **and** `B.1-ε` → `B.1-β`, `B.1-γ`, `B.1-δ` (numeric-green dependents).
- `B.0, α, β, γ, δ, ε, ζ, η, θ` → `C1` (θ gates device output).
- `C1` → `C2` → `C3` → `D`.

Assign B-streams to owners (T1: `fixed`+`geom-clip`; T2: `raster`; T3: `shading`; T4: `cmd-arena`+**`platform`** (the ST7789 565 fork); T5: `harness`+`assets`+`on-target-runner`; raster/sched/platform split into sub-stream tasks for rebalancing per the spec). C1/C2/C3 are assigned to the integrator/perf roles at their barriers.

Expected: task list shows the graph; only the unblocked B-streams are claimable; the rest gated until deps complete. **Now Part D executes.**

---

# PART D — Wave-1 Implementation Streams (dispatched to the agent team)

These are **dispatchable units**, not bite-sized TDD walkthroughs: each is executed in its own worktree/branch by the assigned teammate, who runs the emergent red-green-refactor loop via `orthodox-tdd-cycle` against the frozen contract, lands work via `worktree-pr`, and is gated by the hooks + CI. Each carries the parallelize-plan schema.

**Legend:** Parallelism ∈ {Serialized, Fully-Parallel, Fan-In, Barrier-Gated}.

### Stream B.0 — `fixed` math
- **Goal:** Q-format fixed-point + vec/matrix + no-UMULL multiply + SIO divider, host-tested.
- **Owner / branch:** T1 / `impl/fixed`. **Files owned:** `src/fixed/**`, `tests/fixed/**`. **Forbidden:** all other lanes, `rdr/types.h`.
- **Dependencies:** Stream A merged. **Parallelism:** Fully-Parallel (fan-out point; β/γ/δ branch immediately against the frozen `fixed.h`).
- **Verification:** host golden tests for `fx_mul`/`fx_div`/`mat4` vs the float oracle (B.1-ε); Stage-1 hard gate; arm compile on CI.
- **Deliverables:** `impl/fixed` PR merged to `main`; `fixed` golden tests green.
- **Failure isolation:** its own branch; only dependents' *numeric* greens wait on it. **Re-surface:** a needed op can't meet precision in Q-format → contract-delta for a format change (counts against the churn budget).

### Stream B.1-α — `cmd` + `arena`
- **Goal:** bump arenas + tagged command buffer + display-list record/replay.
- **Owner / branch:** T4 / `impl/cmd-arena`. **Files owned:** `src/{cmd,arena}/**`, `tests/{cmd,arena}/**`. **Forbidden:** other lanes, shared files.
- **Dependencies:** A. **Parallelism:** Fully-Parallel. **Verification:** host unit tests (alloc/reset/overflow-drop-with-count; record/replay/branch); Stage-1 gate; arm CI.
- **Deliverables:** PR merged. **Failure isolation:** own branch. **Re-surface:** command opcode missing from the frozen union → contract-delta.

### Stream B.1-β — `geom` + `clip`
- **Goal:** transform / **basic lighting** / near+guard-band clip / project / cull + tile binning into the frozen `TVtx`/bin. **Lighting scope (pinned):** Wave-1 = the pre-lit RGBA path **plus a single directional light + one ambient term**; **configurable N lights + specular/environment texgen are W4.** So C1's demo is textured + single-directional-lit (or pre-lit), not multi-light.
- **Owner / branch:** T1 / `impl/geom-clip`. **Files owned:** `src/{geom,clip}/**`, `tests/{geom,clip}/**`. **Forbidden:** other lanes, shared files.
- **Dependencies:** A; *numeric greens* need B.0 + ε merged (`git merge main` when they land). **Parallelism:** Fully-Parallel. **Verification:** golden tests vs oracle (transform/clip/project); Stage-1; arm CI.
- **Deliverables:** PR merged. **Re-surface:** `TVtx`/bin layout can't carry needed attrs → contract-delta.

### Stream B.1-γ — `raster` (flat + Z)
- **Goal:** half-space edge rasterizer, per-tile Z test/write, flat fill (no tex/shade yet).
- **Owner / branch:** T2 / `impl/raster`. **Files owned:** `src/raster/**`, `tests/raster/**`. **Forbidden:** `aa` (separate sub-stream), other lanes, shared files.
- **Dependencies:** A; numeric greens need B.0 + ε. **Parallelism:** Fully-Parallel (size as sub-streams `raster-edge`, `raster-z` for reassignment — heavy module). **Verification:** golden tile images vs oracle; top-left fill-rule + degenerate-reject tests; Stage-1; arm CI.
- **Deliverables:** PR merged. **Re-surface:** per-tile Z scratch size from S0.5 wrong → architecture re-open.

### Stream B.1-δ — `tex`(point) + `shade`(modulate) + `blend`(opaque)
- **Goal:** point sampler, modulate combiner, opaque copy blend — the minimal pixel path.
- **Owner / branch:** T3 / `impl/shading`. **Files owned:** `src/{tex,shade,blend}/**`, `tests/{tex,shade,blend}/**`. **Forbidden:** other lanes, shared files.
- **Dependencies:** A; numeric greens need B.0 + ε. **Parallelism:** Fully-Parallel. **Verification:** golden pixel tests vs oracle (sample/modulate/pack 565); Stage-1; arm CI.
- **Deliverables:** PR merged. **Re-surface:** sampler/combiner signature insufficient → contract-delta.

### Stream B.1-ε — host harness (float oracle + golden framework + ImGui inspector)
- **Goal:** the correctness oracle + golden-image compare + ImGui frame inspector.
- **Owner / branch:** T5 / `impl/harness`. **Files owned:** `tests/harness/**`. **Forbidden:** Orthodoxy-enforced module code.
- **Dependencies:** A. **Parallelism:** Fully-Parallel but **on the correctness critical path** — prioritize alongside B.0 (β/γ/δ golden tests depend on it). **Verification:** oracle self-tests; golden-diff harness exercised on a known image; inspector builds (NOT Orthodoxy-enforced — ImGui carve-out).
- **Deliverables:** PR merged early. **Re-surface:** golden-storage decision (commit PNGs vs cache) — Unresolved Q5.

### Stream B.1-ζ — minimal asset tool
- **Goal:** mesh → fixed-point vtx/index; texture → 565/4444 passthrough.
- **Owner / branch:** T5 / `impl/assets`. **Files owned:** `tools/**` (asset subset). **Forbidden:** module code.
- **Dependencies:** A; uses `rdr/types.h` formats. **Parallelism:** Fully-Parallel (genuinely independent). **Verification:** host round-trip tests (known mesh/texture → expected bytes).
- **Deliverables:** PR merged; one texture asset for the C1 demo.

### Stream B.1-θ — `platform` display fork (RGB565 ST7789 + scanline-race DMA)
- **Goal:** fork the template's `src/platform/` for the renderer's output: `screen.pio` to 16-bit (drop the two `out null,4` alpha discards, `set x,15`), `COLMOD 0x55`, RGB565 `rgb()` packing, the DMA scanline-race watermark hook, and the host SDL framebuffer presenter. Firmware links the picotool reset stub + watchdog (shared with B.1-η).
- **Owner / branch:** T4 / `impl/platform`. **Files owned:** `src/platform/**`, `tests/platform/**`. **Forbidden:** module internals, other lanes.
- **Dependencies:** A; the RGB565 config from `rdr/config.h`. **Parallelism:** Fully-Parallel; **required before the C1/C2 device output.**
- **Verification:** host — SDL window presents a known framebuffer (visual + golden); device — a solid-color/gradient flush over the forked `screen.pio` confirms COLMOD/565 (on-target via the runner). Stage-2 arm.
- **Deliverables:** PR merged. **Failure isolation:** own branch. **Re-surface:** 565 `screen.pio` edit doesn't flush correctly on device → fall back to the ARGB4444 path (template default) + record in `WORKFLOW.md` (per the spec's fallback ladder).

### Stream B.1-η — on-target serial runner
- **Goal:** `picotool` flash + reset-to-BOOTSEL + USB-CDC `KEY=VALUE` parse + threshold assert; firmware reset-stub + watchdog helper.
- **Owner / branch:** T5 / `impl/on-target-runner`. **Files owned:** `tools/**` (runner subset). **Forbidden:** module code.
- **Dependencies:** A. **Parallelism:** Fully-Parallel; **required before the C1 device gate.** **Verification:** runner parses a canned `KEY=VALUE` stream + on a real flashed blink/print firmware.
- **Deliverables:** PR merged. **Re-surface:** `picotool reboot -f -u` needs a physical button on this unit (Unresolved Q3) → document the human-BOOTSEL fallback.

### Stream C1 — single-core integration (Fan-In)
- **Goal:** wire modules through the `rdr` façade on a single-core frame path + the demo app → spinning **textured, single-directional-lit** triangle, host **and** device. (Multi-light/texgen is W4 — see B.1-β lighting scope.)
- **Owner / branch:** integrator / `integ/c1-singlecore`. **Files owned:** `src/rdr/**`, `src/demo/**`. **Forbidden:** module internals (consumes them).
- **Dependencies:** B.0, α, β, γ, δ, ε, ζ, η, θ merged to `main` (θ = platform fork, required for device output). **Parallelism:** Fan-In.
- **Verification (automated exit criteria):** host **full-frame golden vs oracle** within tolerance; on-target frame renders + `FRAME_US=` read over serial; no Stage-2 regression on CI.
- **Deliverables:** `integ/c1-singlecore` merged; demo + golden in repo. **Re-surface:** cross-module semantic mismatch surfaces here (per-PR review is lane-scoped).

### Stream C2 — concurrency integration (Barrier-Gated)
- **Goal:** dual-core `sched` + scanline-race + frame exec.
- **Owner / branch:** T4 + integrator / `integ/c2-concurrency`. **Files owned:** `src/sched/**`, `src/rdr/**` (frame loop). **Forbidden:** module internals.
- **Dependencies:** C1. **Parallelism:** Barrier-Gated.
- **Verification:** **host 2-worker simulation** of queue/bin-merge/watermark logic + **prioritized on-target** runs; Stage-2 arm; on-target **Phase-0 re-confirm** of S0.5 numbers; tag `wave-base`.
- **Deliverables:** merged; `wave-base` tag. **Re-surface:** integrated ratio/free-RAM contradicts S0.5 → re-open architecture fork.

### Stream C3 — throughput probe (de-risk the headline claim)
- **Goal:** flood the real pipeline with flat triangles; measure achievable tri/s on device vs the 100K target — **before** investing W2–W5 in features.
- **Owner / branch:** perf-profiler / `integ/c3-throughput`. **Files owned:** `src/demo/**` (stress scene), `tools/**` (probe). **Forbidden:** module internals.
- **Dependencies:** C2. **Parallelism:** Serialized (single device).
- **Verification:** on-target `TRIS_PER_S=` read + asserted against target; recorded.
- **Deliverables:** throughput report. **Re-surface:** throughput far under target → re-open architecture/feature scope NOW, not at W5.

### Stream D — retro + plan W2 (Fan-In)
- **Goal:** run `wave-retro`; collate per-worktree `WORKFLOW.md`; compute KPIs; ship workflow improvements; draft W2 contract-delta + topology.
- **Owner:** Lead + all. **Dependencies:** C3. **Parallelism:** Fan-In.
- **Deliverables:** W2 plan (`docs/superpowers/plans/<date>-wave2-*.md`); shipped agent/skill/hook updates; pruned merged branches/worktrees.

---

## Wave-1 Dependency Graph

```
A ──> B.0 ─┬─> β ─┐
           ├─> γ ─┤
           ├─> δ ─┼─> C1 ─> C2 ─> C3 ─> D
   B.1-ε ──┘      │
   B.1-α ─────────┤
   B.1-ζ ─────────┤
   B.1-η ─────────┤   (η + θ gate C1's device output)
   B.1-θ ─────────┘   (platform 565 fork)
```
ε prioritized with B.0 (correctness critical path). α/ζ/η/θ independent of B.0. C1 needs all B merged.
