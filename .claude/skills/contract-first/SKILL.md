---
name: contract-first
description: Use before implementing any renderer module — define and freeze the module's header (POD structs from rdr/types.h + module_verb signatures + errno-like return codes) and prove it compiles + a stub links, before writing behavior.
---

# Contract-First

Define the interface before the implementation. Behavior comes later, behind a frozen header.

## Procedure
1. Read the module's role in `docs/superpowers/specs/2026-05-26-command-buffer-renderer-design.md` (§Modules) and the shared types in `src/rdr/types.h`.
2. Write `src/<mod>/<mod>.h`: `#include "rdr/types.h"`; declare only this module's `module_verb(Receiver* r, ...)` free functions; every fallible function returns `RdrErr` (0 = `RDR_OK`, errno-like otherwise). No behavior, no new cross-module structs (those live in `rdr/types.h` — request a contract-delta from the Lead if one is missing).
3. Write `src/<mod>/<mod>.cc` stubs returning `RDR_OK`/zero so the library links.
4. Gate: the header compiles standalone (host + arm) and the stub library links. Only then start `orthodox-tdd-cycle`.

## Rules
- POD structs only; pointers not references; C-style casts; plain `enum`. (See CLAUDE.md Orthodox C++.)
- If a signature can't express the behavior you later need, STOP and request a contract-delta PR — do not work around a frozen interface silently (it counts against the wave's contract-churn budget: >2 deltas/wave ⇒ the freeze was premature).
