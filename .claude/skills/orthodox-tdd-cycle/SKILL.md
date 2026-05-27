---
name: orthodox-tdd-cycle
description: Use when implementing any renderer module function — the rigid red→green→refactor loop bound to make test/make format/make tidy, in Orthodox C++, host-first, with frequent commits.
---

# Orthodox TDD Cycle (rigid — follow exactly)

Host-first (Stage-1 microcycle). The bulk of code is written and proven on the host; the target is exercised at the barriers.

## The loop (one function/behavior at a time)
1. **Red:** write the smallest failing test in `tests/<mod>/<mod>_test.cc` (GoogleTest). Choose test data a human can verify by eye.
2. **Run-fails:** `make test` (or `ctest -L <mod>`) — confirm it fails for the expected reason. (Inject-an-error if a new test passes immediately — prove it's wired.)
3. **Green:** write the minimal production code in `src/<mod>/<mod>.cc` to pass. Nothing more.
4. **Run-passes:** `make test` — green.
5. **Refactor (only on green):** remove duplication/magic numbers; extract intention-revealing `static` helpers; copy-don't-cut when extracting (compile the new path, switch one caller, then delete the old).
6. **Commit** the test+code together. Small, frequent commits.

## Orthodox C++ (non-negotiable; CLAUDE.md is the source of truth)
- `struct` not `class`; POD only; no ctors/dtors/inheritance/virtuals/access specifiers.
- Pointers not references; C-style casts; `malloc`/`free`; index loops; plain `enum`; no `auto`/lambda/exceptions/RTTI/overloading/default-args.
- C headers (`<stdint.h>`, `<string.h>`), `printf` not iostream. Return error codes.

## Discipline
- "Do you have a test for that?" — never let production code get ahead of tests; never paste an untested guard clause.
- Only refactor when green. If a refactor breaks a test, **undo and inspect** — don't debug a hole deeper.
- `make format` is the source of truth for layout; `make tidy` must be clean.
