# Wave E — T0 on-target barrier + R-lane (textured raster pipeline)

Drafted by the Wave-D retro (2026-06-03). Base = `main @ c65d24a` (Wave D landed: D.0 oracle-split, D.1 atlas assets, D.2 tex, D.3 blend/fog, D.4 material-intern, D.5 blit2d, D.7 scene/camera — all on-flash, host+pico green).

**Character:** unlike Wave D's 6-wide fan-out, Wave E is **mostly serial** — the raster inner loop (`raster_one`) is one hot function, so R.1→R.4 are a *sequence* (single owner of `raster.cc` at a time), not a parallel fan-out. Parallelism is limited to: T0 asset prep ∥ R.1 start; the blit2d `tex_sample` swap ∥ the R-lane; contract-delta/`.claude` prep ∥ everything. This is by design (see the Wave-D plan's "raster is a single hot function" note).

---

## Contract-delta PR (Lead, pre-wave — lands at a quiesced barrier)

Bind at next team spawn. Sequence the layout deltas at quiesced points (they force rebase + churn every `TVtx`/`Frame` literal).

- **D1 — `TVtx.fog`** (`uint8_t fog` + pad → keep `TVtx` at 16 B). Before R.3-fog. geom populates (reuse `geom_fog_factor`), clip lerps, raster interps. **Churn:** every test constructing a `TVtx` literal rebases.
- **Φ2 — `frame.h` extension** (the parts deferred from Wave-D's Φ): `uint8_t cov[RDR_NUM_RASTER_WORKERS][RDR_TILE_W*RDR_TILE_H]` (per-worker coverage scratch for R.3-AA) + `Blit2dRect blits[RDR_MAX_BLITS]` + `uint16_t blit_count` (for T3 blit wiring). Land before R.3-AA / T3 consume them. **RAM:** cov = 2×3600 = 7.2 KB; budget against the ~3.3 KiB current margin → **couple with the RDR_MAX_TVERTS drop below.**
- **`RDR_MAX_TVERTS` 3000 → 2048** (config.h, N7) — frees ~15 KB. **GATED on T0** swapping the demo to D.1's *indexed* mesh (1152 verts): the D.7 debug placeholder is non-indexed and needs ~2772 tverts, so the cap can only drop *after* T0 uses the indexed mesh. Order: T0 indexed-swap → then cap drop → then Φ2 cov fits.
- **D2 — `RenderState.tex1` + `combiner2` + `COMBINE_TWO_CYCLE`** (detail multitexture). Before R.4. Largest blast radius → opt-in; single-tex-terrain fallback if it slips. **Default = DERIVED detail-UV (no 2nd UV, no delta);** a true 2nd UV (widen `Vtx`+`TVtx`) only if T4 inspection demands it.
- **D4 — widen `TVtx.u_iw/v_iw` int16→int32** IFF T1 measurement shows terrain UV×inv_w overflow. Measure before locking.
- **`src/aa/`** new module subdir registered (R.3-AA owns it) — per the D.5 R8 carve-out, the owner adds its own `add_subdirectory(aa)` line.
- **`.claude` improvements** (from the retro triage): probe-validity checklist → `on-target-probe`/`perf-profiler`; pico-link-gate rule + spike→consumer-handoff + glossary/K_-naming → dispatch template / `renderer-module-owner` gate; `ci_main.sh` tidy-log `error:` grep (LIVE); `task_schema.sh` `.task.metadata.*` (LIVE).

---

## Topology

### T0 — integration barrier (Lead, ON-TARGET; FIRST) — `Fan-In`
- **Goal:** wire D.1 mesh + D.7 camera into a real-density **flat-shaded** on-target probe → first true perf number + the host↔device `fb_crc` cross-check (now bit-identical via D.7's offline-baked Q16.16 camera table).
- **Scope:** swap the D.7 debug placeholder (`demo_terrain_geometry`/`demo_tree_geometry` seam) for D.1's **indexed** const mesh (`g_terrain_grid_vtx`/`_idx`); keep debug colors for legibility OR accept gray (decide — debug colors caught C1 bugs, but D.1's mesh has atlas UVs not debug colors → may need a debug-color override pass). Re-bake assets from the real `~/development/n64/terrain` (CI can't — no source on runners; Lead runs `make assets`/the gen scripts locally). Bind ONE atlas `TexDesc` (deferred to T1 texturing; T0 is flat). Drop `RDR_MAX_TVERTS`→2048 after the indexed swap.
- **Verify:** `on-target-probe` — `frame_ms` (single vs dual), `tris`, `dropped` (must be 0 for the worst frame), `fb_crc` stable across boots, dual==serial, **manual host-vs-device fb_crc cross-check** (the parity payoff — should now be bit-identical). Re-bake round-trip vs real source.
- **Re-surface:** near-plane pop after the real-mesh swap (kf2 worst case, N1 → schedule Stream N); host≠device fb_crc (parity break); `dropped>0` (cap/size).

### R.1 — textured fill + Gouraud + cutout (SERIALIZED, raster.cc head)
- **Owns:** `src/raster/raster.{h,cc}`, `tests/raster/*`, `src/sched/drain.h` (1-line material-table sig). **Deps:** Φ, D.2, D.3, D.4 (all landed). **Parallel:** none (raster.cc).
- **Scope:** `raster_one` interpolates UV+shade, perspective divide, `tex_sample`, combiner, Gouraud, alpha-cutout. Flat-fill fast path stays **bit-identical** (regression gate). Wire ENV color (P4-2).
- **Carry-in from D.2 (verify):** coords are Q16.16 texel space — R.1 converts `Vtx` S10.5 u/v first; **apply the half-texel/center-sampling bias** (D.2 sampler floors raw, assumes caller pre-biases — else 3-point shifts ½ texel); call `tex_validate` at SET_MATERIAL; use `tex_sample_rgba` for cutout alpha. **Per-pixel divide parity (P3-5):** reuse `num/area2` truncation-toward-zero (bit-identical host↔device).
- **Verify:** textured/shaded/cutout vs `oracle_render_tri`; flat-fill goldens unchanged; `tests/sched` dual==serial; cutout skips fb+z write (N11).
- → **T1** (Lead, on-target): opaque single-texture; measure `u_iw` → D4 decision. Terrain looks flat/washed until R.4 (expected staging, P4-2).

### R.3-fog (SERIALIZED, raster.cc + geom.cc + clip.cc) — needs D1
- Populate+interp+lerp fog (reuse D.3 `fog_lerp`). Fog z-space matches pre-scaled near/far (D1/Q9). After R.1.

### R.2 — XLU two-pass + per-tile sort (SERIALIZED, raster.cc) — **independent of R.3** (D3-4: XLU=texel alpha)
- Translucent pass, Z-test no-write, back-to-front per-tile sort (deterministic tie-break). Blend caller pre-multiplies coverage into src-alpha; `blend_pixel_alpha` is alpha-source-agnostic. → **T2**.

### R.3-AA — coverage AA (SERIALIZED on raster.cc; new `src/aa/`) — needs Φ2 cov
- Analytic coverage write (reuse `oracle_coverage`) + within-tile edge-gated resolve (`aa_resolve_tile`). dual==serial preserved; within-tile only (cross-tile = barrier'd follow-on).

### R.4 — 2-cycle detail (SERIALIZED, raster.cc + shade.cc; last) — needs D2 or derived-UV
- `shade_pixel2` + `CC_TEXEL1`/`CC_COMBINED` + dual fetch. **Default derived-UV (no delta);** true 2nd-UV only if inspection demands. **R.4/shade hygiene:** fix `shade_test.cc:41 state_modulate` (un-memset `RenderState`) while here. → **T4** (+ SRAM re-sum).

### D.5b — blit2d `tex_sample` swap (PARALLEL-able alongside R-lane; not raster.cc)
- Once D.2's `tex_sample` grows CI8/I8 (point), swap blit2d's self-decode (one-line per call site, marked TODO(T3)). Signature parity: `tex_sample(TexDesc*, fx16_16 u, fx16_16 v, lod)` honoring `TEXFMT_CI8`+`tlut`/`TEXFMT_I8`. → **T3** (Lead: blit ordering in rdr.cc + Φ2 `Blit2dRect` list; panorama+clouds; whole scene; recheck round-vs-trunc horizon seam at shipping dims + cloud `/255` perf lever).

### Stream N — near-plane clip (CONDITIONAL) — only if T0/T1 path crosses near
- True Sutherland near-clip in geom/clip (interp UV/shade/fog). Build only on the N1 trigger (kf2 + taller real mesh).

### T5 — perf-gate characterization (Lead, on-target) = DONE
- Apply levers (watermark/scanout-overlap FIRST — the ~44% present tax, P3-2; then DDA bit-identical, affine-UV, point/bilinear, AA-gating, pow2 mask-wrap). 30-FPS characterization report. **DONE = all 5 elements rendering + inspection-approved faithful + the perf report**, NOT "30 FPS achieved."

---

## Critical path
T0(on-target) → R.1 → T1(measure u_iw→D4) → {R.3-fog, R.2} → R.3-AA → R.4 → T4 → T3(blit) → T5.
**Parallel-able:** T0 asset re-bake ∥ R.1 prep; D.5b ∥ R-lane; contract-delta/`.claude` prep ∥ all.

## Unresolved (carry from Wave-D plan)
- Q7 depth precision (16-bit w-buffer, far/near ≈160:1 z-fighting) — confirm at T1.
- Q8 cmd-arena/CALL_LIST (16 KB vs 128 cells + 127 trees + sky) — measure at T0.
- Q21 wire the scanline-race watermark (likely required for 30 FPS) — T5 or earlier.
- T0 debug-color-vs-real-mesh: D.1's mesh has atlas UVs, not per-cell debug colors — decide whether T0 keeps a debug-color override (legibility) or accepts the textured look once R.1 lands.
- RAM budget: after Φ2 cov (7.2 KB) + RDR_MAX_TVERTS→2048 (−15 KB) the net is +~8 KB headroom; re-sum at T4 with AA scratch.
