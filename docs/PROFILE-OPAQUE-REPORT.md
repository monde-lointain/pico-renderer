# Back-End Performance Report — T5 Optimization (Abrash-correlated, deep-dive)

> **Snapshot as of 2026-06-05** — captures the pre-optimization back-end profile + the
> ranked lever roadmap. Line refs (e.g. `raster.cc:642`) and the golden CRC reflect this
> point; they will drift as optimizations land. The **live, current roadmap** is the
> approved plan (`.claude/plans/...`), not this report. Treat this as the baseline
> characterization, not a tracking document.

> ## ⚠ On-target validation & roadmap update (2026-06-05, supersedes the §2/§3/§6/§7 L6 ranking)
>
> The §1–§7 analysis below was built on host/SysTick estimates. An on-target probe
> (`pico-prof`, dual-core, scripted `cam_mode=0`, run-cumulative @450 frames; the overdraw
> counters added in this PR) **confirmed the estimates and produced two new facts that
> invert the roadmap.** Confirmed (estimate → on-target): opaque ~4,323 → **4,434 cyc/px**;
> xlu ~2,651 → **2,683 cyc/px**; sweep_xlu 64.5% → **63.6%** of dispatch (= **1.9× opaque**);
> present 5.2% → **5.3%**. The estimates hold.
>
> **New fact 1 — XLU overdraw is even worse than feared (6.12× mean, 100% of XLU px depth≥2;
> ~8,745 distinct px painted as ~53,568 fragments/frame). L6 is the largest single lever and is
> AFFORDABLE after an SRAM reclaim.** The precise free SRAM, from the production (PROFILER=OFF)
> linker symbols (`__end__=0x2003cb3c`, `__StackLimit=0x20040000`), is **13,508 B (~13.2 KB)** —
> *not* the ~30 KB a naive `264KB−bss` subtraction implied. L6's correct front-to-back form needs
> **~21.6 KB** (`C_acc` 14.4 + `acc_alpha` 7.2), so at first glance it doesn't fit.
>
> **UPDATE (same day, SRAM dig — REVERSES the initial "L6 dropped" call):** an audit of the
> production SRAM map found **the 16 KB `Frame.arena_buf` is DEAD** — `arena_alloc` is called
> nowhere in `src/` (only in the arena's own unit test); it is a vestige of the pre-Wave-E binning
> model (bins moved to the fixed `bin_pool`/`bin_jobs` arrays). Reclaim ledger (measured;
> host-deterministic peaks == device peaks):
> - **Dead arena** (`RDR_CMD_ARENA_BYTES` 16 K→~512 B): **+15.9 KB, zero-risk, golden-neutral**
>   (never read/written in rendering).
> - **TVtx pool** 2400→2048 (measured peak **1827** across the scripted loop, +12% margin): +6.9 KB.
> - **`bin_pool`** 2048→1920 (peak 1756) / **`bin_jobs`** 1536→1408 (peak 1248): +2.0 KB.
> - **USB stack** (drop from the *shipping* build only): +2.5 KB (reserve — keeps the on-target
>   determinism gate on the dev/CI build).
> - **`-Os`/MinSizeRel**: frees ~0 SRAM (bss is data-dominated: −8 B measured) but −25 KB flash;
>   only a Q1-resident-code multiplier, NOT an SRAM lever, and a blanket `-Os` slows the hot loop.
>
> → **The dead arena ALONE reopens L6** (13.2 + 15.9 = **29.1 KB ≥ 21.6 KB**, one config one-liner).
> **L6 + Q1 together** (~31.6 KB) fit via arena + TVtx-trim (**36 KB, ~4 KB margin**) with USB and
> the determinism gate intact. **L6 is REOPENED**, gated on a small **SRAM-reclaim prerequisite**
> (config.h `RDR_CMD_ARENA_BYTES` + `RDR_MAX_TVERTS`; optional frame.h bin caps) — itself
> golden-neutral (all caps stay above the measured peaks). The 6× overdraw and on-screen-overlap
> facts (L5/cap cannot reach it) stand — they are exactly *why* L6 is worth the reclaim.
>
> **New fact 2 — Q3 payoff ≈ nil.** opaque z-rejects = **0–4/frame** → opaque terrain barely
> overdraws this pose. Q3 stays a free, bit-identical correctness tidy, but it is **not a lever**.
>
> **Roadmap (replaces §6/§7 ordering):**
> 0. **SRAM-reclaim prerequisite** (golden-neutral): shrink the dead arena + right-size the pools
>    per the ledger above → frees the headroom for L6 (and L6+Q1). Lands first; verify the linker
>    map (`__StackLimit−__end__`) shows the target headroom.
> 1. **L6 — front-to-back + alpha-saturation early-out** is BACK as a top lever (the single biggest
>    cost: 64% of dispatch, 6× overdraw, 100% depth≥2; L5/cap cannot reach on-screen overlap, only
>    L6 can). Needs the §0 reclaim + the frame.h/blend.h barriers + a rebake (§4 L6 caveats hold:
>    premultiplied front-to-back accumulation, fog-before-premultiply, tune `XLU_SAT` vs the golden).
> 2. **Q1** (flash→SRAM-resident fill loop) — free, ~1.3–2× on **both** sweeps, ~10 KB resident; the
>    §0 reclaim leaves room for it alongside L6 (re-measure the map after placing it).
> 3. **L1 → L2 → Q2** (DDA divide-removal, **no buffers**) — the structural per-fragment cut; helps
>    both sweeps. **Q3** is free but ~0 payoff (opaque z-rej ≈ 0) — do for cleanliness only.
> 4. **L5** minor (front-end transform + cap-drop recovery; not the overdraw). **Q5/L4/L3** — P4
>    endgame, gated on residual headroom.

**Build:** `pico-prof` (RENDERER_PROFILER=1, DEMO_FRAMES=480), on-device PicoSystem
RP2040 @250MHz, dual M0+ (ARMv6-M: no FPU, `MULS` 32×32→low-32 only — no UMULL/MLA;
SIO HW divider ~8cyc, 32/32 only; 16KB XIP code+texture cache; 264KB SRAM), scripted
camera (cam_mode=0, reproducible). 450 frames averaged (30 warm-up discarded). FINE
(SysTick) per-triangle anchors `opaque_setup`/`opaque_fill` + a per-core filled-pixel
counter → ns/pixel. Raw captures: `opaque-prof-capture.log`, `opaque-xlu-prof-capture.log`.

**Scope of this revision:** the back-end is two near-identical fill loops — `raster_one`
(opaque) and `raster_one_xlu` (translucent soft trees). The XLU re-measure proved XLU is
the **bigger** fish (2.0× opaque). This report therefore covers the **whole back-end**,
not opaque alone, and is the synthesis of five per-lever deep-dives correlating the
profiling against Michael Abrash's *Graphics Programming Black Book* (digest at
`~/books/technology/abrash-black-book/`, levers in `RASTERIZER-APPLICABLE.md`, asm in
`ARM-ASM-CANDIDATES.md`).

**Measurement validity (the explicit overflow check):** `wrap=` verified per anchor.
`opaque_setup` **wrap=0**. `opaque_fill` **wrap=0 in all three mid-run snapshots
(f120/240/360), wrap=1 only in the final** — the single worst near-camera triangle's
fill (3600px × 17.3µs ≈ 62ms) brushes SysTick's ~67ms cliff; the averaged ns/px uses
only the wrap-free windows. `sweep_xlu` originally **wrap=1 every window** (a tree-heavy
tile's XLU fill > 67ms) → its FINE number was invalid and under-reporting; **re-measured
on the coarse `time_us_64` timer (wrap=0 all windows: 530/614/699/570 ms)**. Anchor
ordering disassembly-verified not reordered around the timed work (Appendix). Profiler
observer effect ≈ +20% on absolute frame time → **rankings are exact; absolute µs are
for the profiled build.**

---

## 1. Executive summary of performance state

The renderer is **single-bottleneck, per-pixel-bound** — not parallelism-, present-, or
front-end-bound. The frame divides (core0 wall, % of root):

| Stage | % of frame | note |
|---|---|---|
| **raster_dispatch (back-end)** | **85.4%** | the whole ballgame |
| geom (front-end, 1-core) | 6.1% | fixed single-core cost, by design |
| present (scanout/upload) | 5.2% | **NOT a lever** — kills the old watermark theory |
| clear + sky blit | 3.2% | |
| cmdgen | 0.01% | |

Inside the back-end (both cores, **98% utilized, core0 join-idle only 2.1% → already
balanced**, so more parallelism buys nothing):

| Back-end anchor | core-ms/frame | % of cores×dispatch | wrap | trust |
|---|---|---|---|---|
| raster_tile (whole tile) | 865.8 | 98.1% | 0 | ✅ |
| ├ **opaque sweep** | 285.7 | **32.3%** | 0 | ✅ |
| │  ├ opaque **setup** (tri_setup) | 5.5 | 0.6% | 0 | ✅ |
| │  └ opaque **fill** (raster_one) | **274.4** | **30.5%** | 1 (worst tri only) | ✅ avg |
| ├ **xlu sweep** (soft trees) | **570.1** | **64.5%** | 0 | ✅ (coarse re-measure) |
| ├ aa_resolve | 9.3 | 1.0% | 0 | ✅ |
| └ glue | ~0.8 | ~0.1% | — | reconciles |

`raster_tile` reconciles exactly: opaque 285.7 + xlu 570.1 + aa 9.3 + ~0.8 = 865.8.

**Four load-bearing findings:**

1. **XLU is the single biggest back-end cost — 2.0× the opaque sweep** (570 vs 286 core-ms;
   64.5% vs 32.3% of cores×dispatch). The driver is **pixel volume, not per-pixel cost**:
   XLU shades **53,761 inside-px/frame vs opaque's 15,820 (3.4×)** from back-to-front
   billboard **overdraw** (no z-write → every layer painted). XLU is actually *cheaper*
   per pixel (2,651 cyc/px vs opaque 4,323) because it z-tests **before** shading, so its
   many occluded fragments are cheap z-rejects. **XLU therefore has a lever opaque lacks:
   cut the overdraw.**

2. **Opaque is overwhelmingly FILL-bound: fill : setup = 50 : 1.** Setup is 0.6% of
   dispatch (4.9µs/tri); the per-pixel fill loop is 30.5%. **Optimize the inner loop —
   not setup, not binning.**

3. **The per-pixel cost is dominated by software 64-bit divides.** Each opaque textured
   pixel executes **8 `__aeabi_ldivmod` 64-bit divides** (9 with fog) — inv_w (raster.cc:543),
   u_iw_p (584), v_iw_p (585), two perspective recovers (`perspective_texcoord_q16`:416),
   three Gouraud channels (`gouraud_chan`:428) — **plus ~18 synthesized 64-bit multiplies**
   (`__aeabi_lmul`, the `(int64)wᵢ·attrᵢ` numerators at 540-542/580-583). A 64-bit ldivmod
   is ~150-250 cyc on M0+, so the divides alone are ~1,200-2,000 of the 4,323 cyc/px.
   **Critically, none of these touch the SIO hardware divider — they are 64/64, and the
   SIO unit is 32/32.** The SIO ~8-cyc divider sits idle. XLU pays the same divides (770-791).

4. **The hot path runs entirely from flash.** `grep __not_in_flash_func src/raster src/tex
   src/shade` = **zero hits** (confirmed). The ~9KB inlined loop + leaves fetch through the
   16KB XIP cache that the two cores **share** (core1's fills evict core0's hot lines). The
   ~8-12KB SRAM budgeted for resident hot code was never spent. This is the XIP-I-fetch-stall
   component of the 4,323 cyc/px, distinct from texel-fetch bandwidth.

For scale: Abrash's hand-tuned Pentium textured-fill loop is **7.5 cyc/px**. Our 4,323 (opaque)
/ 2,651 (XLU) vs 7.5 is the headroom — and it lives in **one inner loop shared by both sweeps.**

**Bottom line:** 30 FPS is gated by (a) XLU billboard overdraw and (b) per-pixel 64-bit
arithmetic + flash latency in one fill loop. Every lever below attacks one of those two.

---

## 2. Categorized optimization opportunities (Abrash-correlated)

### CPU / arithmetic — the dominant category
- **8 per-pixel 64-bit divides** (inv_w, u_iw_p, v_iw_p, 2× perspective recover, 3× Gouraud).
  → **L1** (two-stage DDA: hoist all `÷area2` to setup, step interpolants with adds — ch.56/57,
  1/z is screen-linear ch.66); → **L2** (per-span perspective: exact `1/(1/z)` every 16px, affine
  between, **overlap the SIO divide** behind the burst — ch.68/70); → **Q2** (affine on small/distant
  tris — zero divides for that tri, ch.70).
- **~18 per-pixel 64-bit multiplies** (`__aeabi_lmul` numerators). → eliminated by **L1** (numerators
  become stepped adds — ch.57 "cumulative additions replace multiplication"); discipline in **Lever 4**
  (four-partial-product reserved for setup, narrow per-pixel operands — ch.54).
- **2-cycle color combiner per pixel** (`shade_pixel2`: two `run_cycle`, TEXEL0×TEXEL1×ENV). → **L4**
  surface-cache the frame-stable part once, bare fetch in the loop (ch.68) — **gated** by the cache
  hazard below.

### Memory / I-cache / texture bandwidth
- **Hot path flash-resident** (0 `__not_in_flash_func`) → per-pixel XIP I-fetch stalls through a
  16KB cache shared by both cores. → **Q1** (SRAM-resident inner loop — ch.57/68 "the inner loop is
  the whole program").
- **Up to 8 flash texel fetches/px** on the 2-cycle detail path (THREE_POINT base = 4 taps +
  `sample_texel1` detail = 4 more). → **Q5** (mipmap to ≈1:1 texel:pixel + point-sample distant →
  1-2 fetches, ch.68); the `lod` plumbing is half-built (`mip_levels` always 1, `tex_sample`'s `lod`
  is `(void)`'d). → **L5-coherency** (group the opaque sweep by texture, not bin order — ch.67/70).
- **Surface-cache cache hazard** (ch.68 "a 64×64 tile fits cache; a surface doesn't — miss every 32
  texels"): the terrain atlas is **512²=512KB**, ~32× the cache → a naive combined surface is a *net
  loss*. **Defer L4; do Q1+Q5 first** (they capture most of the benefit safely).

### Algorithms / redundant work
- **Opaque shades BEFORE the z-test** (texture+combiner+fog at raster.cc:580-640, z-test last at 642)
  → z-rejected opaque pixels pay full shading. XLU already z-tests first (779). → **Q3** (hoist the
  z-test, bit-identical).
- **XLU overdraw**: no z-write, every back-to-front layer fully shaded (53,761 px). → **L6**
  (front-to-back + accumulated-alpha saturation early-out — the z-write XLU gave up, recovered as an
  alpha cull; ch.68 zero-overdraw discipline). **The single biggest available reduction.**
- **No frustum/AABB cull**: all 127 billboards transform every frame; worst poses **drop up to 184 of
  1,276 source tris** on the XLU cap (visible foliage loss). → **L5** (worldspace/screen cull — ch.61
  "~50% of polys culled before transform"; pre-stored normals dodge the sqrt, ch.54).

### Concurrency
- **Saturated.** Dual-core 98% busy, core0 idle 2.1%. No headroom. Front-end geom single-core (6%,
  by design — C2 decision).

### Data structures
- Layout is fine. The win is computing gradients **once per triangle** (DDA) instead of re-deriving
  per pixel — a code change, not a layout change. (The one existing layout guard — int16→int32 widening
  of `u_iw`/`v_iw` at raster.cc:206-209 — is itself Abrash's "guard every multiply's range," ch.62.)

---

## 3. Prioritization matrix (impact vs effort)

Impact = est. reduction in total back-end cost (opaque fill + XLU fill ≈ 96% of dispatch). Effort
includes the bit-identical-vs-golden re-verification each change demands. **"Rebake?" = does the
change move the golden CRC** (bit-identical changes ship with no fidelity debate; pixel-changing ones
need a justified golden rebake + on-target host==device re-check).

| # | Optimization | Impact | Effort | Rebake? | Class |
|---|---|---|---|---|---|
| Q3 | **z-test before texture/combiner** (opaque) | Med | **Very Low** | No | Quick win |
| Q1 | **SRAM-resident fill loop** (`__not_in_flash_func`) | **High** | **Low** | No | Quick win |
| **L6** | **Cut XLU overdraw** (front-to-back + alpha-sat early-out) | **Very High** | High | Yes | Structural |
| L1 | **DDA interpolants** (hoist 6 of 8 ÷ to setup, step with adds) | **Very High** | Med-High | **No\*** | Structural |
| L2 | **Per-span perspective** (÷ every 16px, overlap SIO divide) | High | High | Yes | Structural |
| Q2 | **Affine UV on small/distant tris** (zero ÷ for that tri) | **High** | Med | Yes | Quick-ish |
| L5 | **Frustum/AABB-cull billboards** (fewer tris, fewer drops) | Med-High | Med | No\*\* | Structural |
| Q5 | **Mip + point-sample distant** (cut 8-fetch → 1-2) | Med-High | Med | Yes | Quick-ish |
| Lc | **L5-coherency** (group opaque sweep by texture) | Low-Med | Low-Med | No\*\* | Quick win |
| L4 | **Surface-cache the combiner** (gated on cache size) | Med | High | No\*\*\* | Long-term |
| L3 | **Packed-fraction texel stepper** (hand ARM asm) | Med | Very High | No\*\*\* | Long-term endgame |

\* **L1 is golden-neutral** if done with Bresenham-exact remainder carry (reproduces the truncated
divide bit-for-bit — see §4). \*\* Conservative frustum cull / stable texture grouping are
image-preserving (bit-identical); a draw-distance cap rebakes. \*\*\* Bit-identical *only if* the
baked/asm math reproduces the inline path byte-for-byte (verify vs the float oracle).

**Ordering rationale.** Three buckets, dependency-ordered:
- **Free now (no rebake, localized):** Q3 then Q1 — measurable, bit-identical, de-risk the baseline.
- **Biggest single cost:** L6 — XLU is 2× opaque and 100% overdraw-bound; this is the largest lever
  in the whole frame. Needs a rebake, so it follows the free wins that establish a clean baseline.
- **Structural inner loop (helps BOTH sweeps):** L1 (golden-neutral, the largest per-pixel cut) → L2
  → Q2, on the serialized R-lane, test-first against the oracle. L5/Q5/Lc/L4/L3 follow.

---

## 4. Detailed recommendations

Each: current state (+ profiling/line refs) · proposed change · steps · expected gain · determinism · code.

### Q3 — Z-test before the texture/combiner work (opaque) — FREE
**Abrash basis** — ch.39 ("profile, then run the cheapest discriminator first"). The z-test is a single
`uint16_t` compare; the shading chain after it is hundreds of cycles. XLU already proves the order
(raster.cc:779 tests first → cheap rejects).
**Current state** — `raster_one` computes `znew` at raster.cc:548, then runs UV recover (580-587),
bilinear sample (603-612), Gouraud (590-596), 2-cycle combiner (621), fog (636-640), and only **then**
tests depth at **642** (`if (znew < zbuf[zi]) continue;`). A z-rejected opaque pixel paid the entire
chain. The rejection criterion sat unused ~100 lines. This is part of the 4,323-vs-2,651 cyc/px
opaque-vs-XLU asymmetry.
**Proposed** — hoist `if (znew < zbuf[zi]) continue;` to immediately after line 548 (textured branch),
matching XLU. The cutout discard (606) and z-**write** (645, on keep) stay in their relative order —
a z-rejected pixel writes nothing in either ordering.
**Steps** — (1) add the early reject after 548 in the textured branch; (2) delete the redundant test at
642 (keep the write at 645); (3) leave the flat fast path (550-575) untouched (the bit-identical
regression gate); (4) confirm cutout/keep still precede the write so a discarded fragment never seeds
depth; (5) golden CRC host + on-target.
**Expected gain** — pure work removal proportional to **opaque overdraw depth**; touches no winning
fragment. Bounded (opaque is 32.3% of dispatch; only its overdrawn fraction is recovered), largest on
near poses with deep opaque stacks.
**Determinism** — **bit-identical, no rebake.** Same winning fragment, same write order, same arithmetic
— only *when the loser's shading is skipped* changes. If CRC moves, the reorder has a bug.
```c
// raster.cc raster_one(), textured branch, right after znew is computed:
uint16_t const znew = depth_pack(inv_w);
if (znew < zbuf[zi]) continue;     // Q3: was at line 642, post-shade. Skip ALL shading.
// ...UV recover, sample, Gouraud, combiner, fog (unchanged)...
if (!keep) continue;               // cutout/keep gate the WRITE, not the test
zbuf[zi] = znew;                   // z-write only on keep (was line 645)
```

### Q1 — SRAM-resident fill loop (`__not_in_flash_func`) — BIGGEST WIN / LEAST RISK
**Abrash basis** — ch.57/68: the inner loop *is* the whole program; its instruction stream must live
somewhere fast. Our 16KB XIP cache is the analogue of Abrash's Pentium L1; when code+texture don't fit,
the memory subsystem (not instruction count) bounds the loop (ch.39/68).
**Current state** — `raster_one` (raster.cc:446) + `raster_one_xlu` (680) + their leaves
(`edge_eval`, `gouraud_chan`, `perspective_texcoord_q16`, `tex_sample*`, `point_rgba`,
`decode_texel_rgba`, `three_point_rgba`, `shade_pixel*`, `fog_lerp`) — ~9KB — all compile to flash
`.text`. Every XIP-miss stalls the core. The **two cores share the one 16KB cache**, so dual-core
*worsens* I-cache pressure. The irony is documented in `prof_pico.cc:3-7` (the profiler timer reads
were SRAM-pinned to avoid evicting the very loop they measure; the loop itself was not).
**Proposed** — place the inner-loop functions + per-pixel leaves in SRAM via the SDK macro
`__not_in_flash_func(name)`; optionally pin the *active mip tile* (Q5) in SRAM for the duration of a tile.
**Steps** — (1) host-neutral shim (`#define __not_in_flash_func(f) f` off-device) so the SDL build is
untouched; (2) wrap the definitions; (3) **SRAM re-sum**: FB ~113KB + Z scratch 7.2KB + cov 3.6KB +
pools, leaving comfortable room for ~8-12KB resident `.text` (placement, not new alloc — the budgeted-
but-unspent block); (4) re-probe on-target.
**Expected gain** — removing XIP I-fetch stalls from a tight loop is commonly **1.3-2× on M0+**, and it
attacks the stall component shared by **both** fill loops; relieving the shared cache compounds across
cores.
**Determinism** — **placement-only, bit-identical.** Changes where instructions are fetched, never what
bytes are computed. Host build sees a no-op. Only guard: re-probe to *confirm* the speedup (invisible to
host CI).
```c
#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
#include "pico/platform.h"
#else
#define __not_in_flash_func(f) f
#endif
static uint32_t __not_in_flash_func(raster_one)(const struct TriSetup* t, /*...*/) { /* unchanged */ }
uint16_t __not_in_flash_func(tex_sample)(const struct TexDesc* t, fx16_16 u, fx16_16 v, int lod) { /*...*/ }
```

### L6 — Cut XLU overdraw (PRIMARY near-term target) — BIGGEST SINGLE COST
**Abrash basis** — ch.68 zero-overdraw discipline: Quake draws front-to-back; *occluded surfaces are
never drawn, so they're never built.* ch.39: the bottleneck is wherever the cycles are — measurement
says **XLU (570 core-ms, 64.5%).**
**Current state** — `raster_one_xlu` (raster.cc:680) has **no z-write** (777, back-to-front, every layer
painted over every layer below). XLU = 53,761 px (3.4× opaque); overdraw-bound, not per-pixel-bound. Sort
= `xlu_depth_key` (857, ascending inv_w = farthest first) + `xlu_sort_stable` (872). A pixel under N
overlapping soft-tree billboards pays N× shade+blend, with no early-out. **Why XLU has this lever and
opaque doesn't:** opaque's z-buffer kills overdraw for free (z-write → nearest wins); XLU *deliberately*
disables z-write for correct alpha layering, so it re-pays every layer. That re-payment × 3.4× pixels = 570 ms.
**Proposed** (priority order):
1. **L5 cull first** (below) — removes off-screen/distant billboards' fragments outright.
2. **Front-to-back + accumulated-alpha saturation early-out** — reverse the sort to front-to-back, keep
   a per-tile `acc_alpha[]` byte (like `cov`), and **skip** a fragment once its pixel saturates
   (≈opaque). This is the z-write XLU gave up, recovered as an alpha-saturation cull. Requires
   **under-operator** compositing (dst stays, accumulate) instead of the current over (raster.cc:837).
3. **Tighter draw distance / smaller billboards** (L5 distance cap) → fewer texels per distant tree.
**Steps** — land L5; add per-tile `acc_alpha[RDR_TILE_W*RDR_TILE_H]` (1B/px, cleared per tile like
zbuf/cov); reverse to a deterministic stable front-to-back sort; after the opaque z-test (779) add
`if (acc_alpha[zi] >= XLU_SAT) continue;`; switch the blend to under-op + saturating accumulate; rebake
golden; re-probe dual==serial.
**Expected gain** — directly attacks the **single biggest cost** (570 ms / 64.5%, all overdraw). For
dense foliage a deeply-overlapped pixel drops from ~N× to ~1× once saturated — the largest available
reduction in the frame, far bigger than Q3/L4.
**Determinism** — **changes the image → rebake.** Front-to-back + under-op changes which fragments
contribute and the compositing equation. The new sort must stay a deterministic stable tie-break
(`xlu_sort_stable` at 880 is load-bearing for dual==serial); `acc_alpha` must be per-tile-local and
cleared per tile (each tile drawn by one worker, raster.cc:971) so dual==serial holds. **Pitfall:** too-
aggressive saturation drops the faint soft-edge contributions the XLU-soft-trees work added (bilinear-
alpha foliage) — tune against the golden, not blindly. A pure draw-distance cap (no compositing change)
is the lowest-risk slice.
```c
// raster.cc raster_one_xlu(), front-to-back, after the opaque z-test (line 779):
if (acc_alpha[zi] >= XLU_SAT) continue;   // saturated by nearer layers -> skip farther one entirely
// ...UV recover, sample rgba, combiner, fog (unchanged)...
fb[fbi] = blend_pixel_under(combined, rgba[3], fb[fbi], acc_alpha[zi]);  // under-op
acc_alpha[zi] = sat_add_u8(acc_alpha[zi], rgba[3]);                      // accumulate
```

### L1 — Two-stage DDA interpolants (the structural payoff; GOLDEN-NEUTRAL)
**Abrash basis** — ch.56 ("only two divides per scan line"); ch.57 ("cumulative additions replace
multiplication"); ch.66 (1/z, s/z, t/z are screen-linear → steppable with the same adds as the edge
functions).
**Current state** — the inner loop recomputes, **per pixel**, `(Σ wᵢ·Qᵢ)/area2` from scratch for
`inv_w` (543), `u_iw_p` (584), `v_iw_p` (585) and three Gouraud channels (428) — **6 of the 8 ÷area2
divides + two 64-bit numerator mults each** (540-542, 580-583). All six share the divisor `area2`,
which is what makes the hoist legal. This is the largest single cluster in 4,323 cyc/px.
**Proposed** — for OUR half-space raster (a "span" = a contiguous covered run in a tile row): in
`tri_setup`, precompute each interpolant's per-pixel screen-x step `dQ/dx = (Σ (dwᵢ/dx)·Qᵢ)/area2` once
(the `dwᵢ/dx` are the Q12.4 edge-y-deltas already implied by the verts). At a span's first inside pixel,
seed exactly (one divide). Per pixel: `acc += dQ_dx` (six `ADDS`) — **zero divides** for these six.
**Steps** — (1) add `tri_setup` step fields `diw_dx, duiw_dx, dviw_dx, dsr_dx, dsg_dx, dsb_dx` (+ `_dy`
row steps), each a setup-frequency divide (6/tri vs 6×15,820/frame today); (2) in `raster_one`, seed at
the run start, replace 540-543/580-585/`gouraud_chan`×3 with `acc_* += d*_dx`; (3) identical edit in
`raster_one_xlu` (767-799); (4) handle 1-px spans (seed == value, no stepping overhead).
**Expected gain** — removes **6 of 8 ldivmods/px + ~9 `__aeabi_lmul`/px** (the numerator mults),
converting them to 6 adds — on the order of **~900-1,500 cyc/px removed**, the dominant single cut, on
**both** sweeps. Cost moved to negligible setup (fill:setup already 50:1).
**Determinism** — **golden-neutral (no rebake) if stepped with Bresenham-exact remainder carry.** A naive
truncating DDA accumulates biased error (ch.53 "the cubes distort... biased errors become visible") and,
though host==device consistent, would *still* move the golden vs from-scratch re-evaluation. The fix:
keep an integer remainder alongside each accumulator and carry exactly, so the stepped value **equals**
the truncated `(Σwᵢ·Qᵢ)/area2` at every pixel:
```c
int32_t inv_w = (int32_t)(num0 / (int64_t)t->area2);     // exact seed == today's value
int64_t rem   = num0 - (int64_t)inv_w * t->area2;        // Bresenham remainder
for (int sx = run_x0; sx < run_x1; ++sx) {
  /* use inv_w (identical bits to a from-scratch num/area2) */
  rem += t->dnum_iw_dx;                                   // step the numerator
  while (rem >= t->area2) { ++inv_w; rem -= t->area2; }   // exact carry, no re-divide
  while (rem < 0)         { --inv_w; rem += t->area2; }
}
```
Validate on host (golden-image-test) then on-target CRC. (Levers-4 gate: round-half on any consume that
*does* shift; per-span reciprocals may truncate.)

### L2 — Per-span perspective with overlapped SIO divide
**Abrash basis** — ch.70 (exact (u,v) at each end + every 16px, affine between; "almost never visible");
ch.68 ("FDIV overlapped with 16 pixels of drawing → 7.5 cyc/px"); ch.66 (the linearity license).
**Current state** — after L1, the *only* remaining per-pixel divides are #4/#5: the perspective recover
`(num_uiw<<27)/inv_w_p` inside `perspective_texcoord_q16` (raster.cc:416), done twice/px (u,v). These are
the true perspective divide.
**Proposed** — within a covered run: at the start and every **N=16px** checkpoint, do the exact recover —
one **32/32 reciprocal** `recip=(1<<K)/inv_w_p` (this is the **first divide in the hot path eligible for
the SIO ~8-cyc HW divider**, because today's are 64/64) + a multiply, not a divide. Affine-interpolate
`u_q16,v_q16` between checkpoints (L1 adds). **Overlap the SIO divide:** kick `sio_hw->div` for the next
checkpoint's reciprocal at burst start, run the ~16 affine pixels while it computes (~16px of fetch+
combine+store ≫ 8 cyc), read the quotient at burst end → the divide is fully hidden.
**Steps** — (1) split `perspective_texcoord_q16` into `persp_recip` (32/32 → SIO) + `persp_apply` (mul+
shift); (2) restructure `raster_one`/`raster_one_xlu` inner loop into outer-run + inner-16px-burst; (3)
issue/read `sio_hw->div` async (SDK `hw_divider_*` or raw regs); (4) compose with Q2 (affine tris skip the
checkpoint divide too); (5) tune N (8/16/32) on-target.
**Expected gain** — removes the last 2 divides/px, replacing them with **1 overlapped 8-cyc divide per
16px ≈ free**. With L1, per-pixel divide count goes **8 → ~0**. Both sweeps.
**Determinism** — **rebake.** Keep the **exact divide at checkpoints** (bit-identical there), but the
affine-interpolated interior pixels were previously exact-per-pixel → their bits change → justified
rebake ("per-span perspective, N=16 affine fill, Abrash ch.70; perceptually invisible"). Validate bowing
vs the oracle. **SIO caveat:** the divider is per-core (tiles partition disjointly across cores → no
cross-core contention), but mask/`hw_divider_save_state` around the burst if an ISR on the same core could
use it mid-flight; truncation parity host==device must hold.

### Q2 — Affine UV on small/distant triangles (zero divides for that tri)
**Abrash basis** — ch.70 ("small enough, distant enough → affine distortion invisible; ~3× faster than
affine[sic] for distant small tris"); ch.56 (affine bows only large, angled polys).
**Current state** — every tri pays the full perspective path including #4/#5, even the dominant small
near-screen-parallel terrain/foliage tris where perspective is invisible.
**Proposed** — classify once in `tri_setup` (integer-only → deterministic): affine-eligible iff bbox
extent < AFFINE_PX **or** the `iwN` spread < AFFINE_IW_EPS (near-parallel). For eligible tris, step
`u_q16,v_q16` as screen-linear interpolants (L1 adds) — **no `÷inv_w_p` at all** (beats even L2 for those
tris). Depth (z-test) stays perspective-correct; only the *texture* goes affine (N64/Quake convention).
**Steps** — add `int affine` to `TriSetup`; set from integer bbox + `iwN` spread; precompute affine UV
gradients when set; branch in `raster_one`/`raster_one_xlu`; tune thresholds vs golden.
**Expected gain** — for the small-tri-dominated workload, the **largest aggregate divide reduction** —
an affine + L1-stepped tri does **zero per-pixel divides**.
**Determinism** — **rebake** (affine ≠ perspective bit-for-bit). Classifier must be integer-only and
host==device identical; thresholds are compile-time enums; round-half the affine accumulators (ch.53).
```c
enum { AFFINE_PX = 16, AFFINE_IW_EPS = 64 };  // tune on-target
t->affine = (extent_x < AFFINE_PX && extent_y < AFFINE_PX) || ((iw_hi - iw_lo) < AFFINE_IW_EPS);
// raster_one: if (t->affine) { u_q16 += du_dx; v_q16 += dv_dx; }  // NO ÷inv_w_p
```

### L5 — Frustum/AABB-cull the 127 billboards
**Abrash basis** — ch.61 (worldspace dot-sign cull eliminates ~50% of polys *before* transform: 3 mul +
2 add, no divide, no sqrt); ch.54 (pre-store unit normals → no per-frame sqrt).
**Current state** — all 127 billboards transform every frame, no cull; worst near poses drop up to **184
of 1,276 source tris** on the XLU cap (`RASTER_XLU_TILE_CAP=256`, raster.cc:148; drop-count
`s_xlu_dropped` at 1014) — *visible foliage loss* plus wasted transform on off-screen trees.
**Proposed** — per-billboard worldspace AABB/sphere cull **before** transform+bin: 6 frustum-plane sign
tests (ch.61); optional draw-distance cap. Culled billboards never transform, bin, or compete for the
XLU cap → **fewer cap-drops, not more.**
**Steps** — precompute per-billboard AABB at scene load; sign-test against the frustum (with guard-band
margin = `RDR_GUARD_X/Y`) before transforming; add a `DRAW_DIST` config; assert `s_xlu_dropped` falls on-target.
**Expected gain** — removes front-end transform/bin for off-screen/distant billboards **and** recovers the
184-tri cap-drop (visible foliage). Secondary assist to L6 (fewer/closer billboards = less overdraw).
**Determinism** — a **conservative** frustum cull (rejects only provably-fully-outside AABBs, with
guard-band margin) is **image-preserving → no rebake**. A **draw-distance cap rebakes** (drops distant-
but-visible trees). Removing cap-drops also changes the image (more foliage now present) → rebake.
Front-end decision, before the tile partition → dual==serial unaffected.

### Q5 — Mipmap + point-sample distant tris (cut 8-fetch → 1-2)
**Abrash basis** — ch.68 (mip to ≈1:1-1:2 texel:pixel by nearest-vertex distance; box-filter + error-
diffusion dither offline); ch.70 (distant ⇒ cheaper sampling).
**Current state** — THREE_POINT (`three_point_rgba`, tex.cc:204) = **4** point fetches; the 2-cycle
detail path (`sample_texel1`, raster.cc:276) doubles to **8** flash fetches/px, scattered across the 512²
atlas (ch.68's "miss every 32 texels"). `TexDesc.mip_levels` exists (types.h:81) but is always 1;
`tex_sample`'s `lod` is dead (`(void)lod`, tex.cc:235) — plumbing half-built.
**Proposed** — bake a mip chain offline; select per-tri LOD in `tri_setup` (`log2` screen size) for ≈1:1;
on distant/small tris force POINT (1 fetch) and skip the detail pass.
**Steps** — (1) mip bake in `tools/asset_tool.py`/`asset_convert.py` (box-filter + dither, set `mip_levels`);
(2) `tri_setup` LOD = clamp(log2(texels/pixel)); store in `TriSetup`; (3) wire `lod` in tex.cc
(`w>>lod`, `h>>lod`, mip base offset — pow2 stays exact, CL-5); (4) force POINT when `lod ≥ threshold`
(reuse the CI POINT branch at tex.cc:245). Both sweeps fetch THREE_POINT from flash → both benefit.
**Expected gain** — **4-8× fewer texel fetches on distant tris**; shrinks the working set so it fits cache
(compounds with Q1's hot-texture residency). Hits the small-tri-dominated majority.
**Determinism** — **rebake** (a mip is a different texel; point drops the bilerp). Threshold-tunable
against the oracle.

### Lc — L5-coherency: group the opaque sweep by texture
**Abrash basis** — ch.70/67 ("all spans for one surface, then the next → texture/cache coherency; mixing
into one global list is wrong for a real textured renderer").
**Current state** — the opaque sweep iterates TriRefs in **bin order** (raster.cc:924), selecting texture
per-tri (951); adjacent refs can switch textures → the active working set thrashes the shared 16KB cache.
**Proposed** — stable bucket/counting sort the bin by `ref->material` (mirror the existing `xlu_idx`/
`xlu_sort_stable` discipline) so same-texture refs draw consecutively.
**Steps** — build a fixed `uint16_t draw_order[]` histogram by material before SWEEP 1; **stable** sort;
iterate grouped, `tri_setup`+`raster_one` unchanged.
**Expected gain** — fewer texture-line evictions between consecutive tris → fewer cold-flash fetches.
Hardest to quantify (depends on per-tile material mix; single-texture tiles see ~0). Multiplies with
Q1/Q5 (a coherent group can be pinned/mipped resident).
**Determinism** — **bit-identical IF the sort is stable** (opaque z-test+write is order-independent at a
pixel; the only risk is equal-`znew` cross-material ties → a stable sort preserving bin order within a
material covers it). Validate vs golden; treat as bit-identical-pending-CRC, not free.

### L4 — Surface-cache the combiner (GATED — cache hazard)
**Abrash basis** — ch.68 ("draw with the surface as input texture, no lighting during texture mapping...
2.25 cyc/lit-texel build + 7.5 cyc/px map"; "two specialized loops beat one"). **And the liability:** "a
64×64 tile fits cache; a surface doesn't — every texel unique → miss every 32 texels."
**Current state** — `shade_pixel2` (raster.cc:621 → shade.cc:204) runs the full 2-cycle combiner per
pixel (two `run_cycle`: 4× input-mux + 3× `(a-b)*c+d`). Its PRIM/ENV/mux inputs are **frame-stable per
material**; only TEXEL0/TEXEL1/SHADE vary.
**Proposed** — pre-bake the frame-stable part (TEXEL0×TEXEL1-detail×ENV/PRIM) once into an SRAM surface;
the loop fetches it and applies only the cheap per-pixel SHADE modulate. Real payoff is **2-cycle detail**
materials (two combiner cycles → one bake + one modulate, deleting the per-pixel TEXEL1 fetch).
**Steps** — per-material surface cache keyed `(material, mip)` with a **hard size gate**; bake once/frame
(or invalidate-on-change); replace `shade_pixel2` with surface-fetch + SHADE modulate; keep the inline
path as fallback. Behavior-preserving refactor (copy → compile → switch → delete).
**Expected gain** — removes one full `run_cycle` + the per-pixel TEXEL1 fetch from every 2-cycle opaque
pixel. Bounded by how many of the 15,820 opaque px use detail.
**Determinism** — **bit-identical only if the bake math reproduces the inline `(a-b)*c+d`/round/shift/
clamp/565-pack byte-for-byte** (any 888-vs-565 drift moves CRC). **HAZARD:** the 512² atlas is 512KB ≈ 32×
the cache — a naive combined surface stalls worse than the combiner it replaces. **Gate strictly:** only
bake ≤64×64-class **Q5-mipped** frame-stable surfaces, keep the active one SRAM-resident, never bake large
unique surfaces. This is why L4 is deferred behind Q1+Q5 (which capture most of the benefit safely). The
portable takeaway even if we cache nothing: **two specialized all-register loops beat one** — Q1's
resident specialized loop is the low-risk realization.

### L3 — Packed-fraction texel stepper, hand ARM asm (ENDGAME)
**Abrash basis** — ch.58 (Miles/Hecker "9-cycle" stepper: pack U+V fractions in one reg; `ADD` steps both,
the X-carry folds into the source pointer via `ADC`, the Y-carry bumps a scanline; no per-pixel mul/div).
**Current state** — after L1/L2, the residual per-pixel work is texel address + fetch + combiner + store.
The address today is `lin=(v*w)+u` (`decode_texel_rgba`, tex.cc:124) — a per-pixel `MULS` + two mask-wraps.
**Proposed** — port the packed-fraction DDA to ARMv6-M: it **is** the body of L1/L2's affine loop, but
stepping the **byte pointer** directly (`ADDS frac,step` / `ADCS src,iadv` / expose V-carry via `LSLS`
+`BCC` / `ADDS src,yadv` / `BICS`), so `v*w+u` never executes in the loop again.
**ARM port (Thumb-1 sketch):**
```armasm
loop:
    ldrh  r4, [r0]              @ texel fetch (LDRB for I8/CI8)
    @ ...combiner + store r4 -> [r5]; advance r5...
    adds  r1, r1, r2           @ step BOTH packed fractions; C = U-fraction carry
    adcs  r0, r0, r3           @ src += integral advance + U carry  (iadv MUST be a low reg)
    lsls  r7, r1, #15          @ expose V-carry (bit16) into N
    bpl   1f                   @ no V carry -> skip
    add   r0, r0, r8           @ V carried: += one scanline (r8 = stride, hi-reg add)
    bics  r1, r1, r9           @ clear the V sentinel (r9 = mask)
1:  subs  r6, r6, #1
    bne   loop
```
**Register pressure (the real enemy)** — 8 live values (src, packed frac, packed step, integral advance,
scanline advance, pixel, count, dst) exactly fill r0-r7 with zero headroom; `iadv` *must* be low (an `ADCS`
operand); park `yadv`/`vmask` in r8-r11 (touched only on the cold V-carry path). tile-Z stays in the C
caller. **Branch vs branchless V-carry:** branch (~1 cyc not-taken / ~5 taken) beats the ~6-fixed-instr
mask form for typical V-carry rates; revisit if minification pushes the carry rate past ~70%.
**Bilinear caveat** — our sampler does 4 (or 8) taps; the stepper steps only the **base** pointer, the 3
neighbors are fixed `+bpt`/`+stride`/`+stride+bpt` offsets (stride ≥16px needs a register offset). The 5-bit
S/T lerp weights are already in the packed `frac` high bits. **Pow2 wrap:** restrict the asm burst to the
texture interior, fall back to the C `wrap_coord` path at wrap seams (recommended — keeps the kernel
branch-light, wrap semantics byte-identical).
**Steps** — (1) land L1/L2 in C first (asm against the current per-pixel-divide loop is pointless); (2)
SRAM-resident `__not_in_flash_func` leaf (inline `__asm__ volatile`, matching house idiom; **no** self-
modifying code — XIP); (3) test-first: device-asm tile == device-C tile == host-C tile, **zero tolerance**,
across fuzzed spans/formats; (4) dual==serial CRC across boots; (5) pin half-LSB rounding parity.
**Expected gain** — texel addressing → a handful of instr. **Honest scope:** the **last** lever — the
elephant (the per-pixel divides) is killed by L1/L2 in plain C; L3 is diminishing-returns, high-effort,
high-risk, justified only if the post-L1/L2 profile still shows addressing material. Don't over-unroll (16KB
I-cache); a 4-8× unroll amortizes the loop branch without blowing the cache.
**What does NOT transfer** — x86 segment tricks (the 9→8-cyc move), U/V pairing/AGI scheduling, `SCANOFFSET`
disp32 baked stores, the Mode-X planar apparatus, predicated `TEST`+`AND` (→ `LSLS`/`BPL`/`BICS`).

---

## 5. Quick wins (low effort, high impact)

Do these first — localized, individually measurable on-target:

1. **Q3 — z-test before shading (opaque).** *Free, bit-identical, do now.* Stops paying full shading for
   z-rejected pixels.
2. **Q1 — `__not_in_flash_func` the fill loop.** *Placement-only, bit-identical, ~1.3-2×.* Attacks the
   XIP-stall component shared by **both** fill loops, spends the budgeted-but-unspent SRAM. **Biggest win
   for least risk.**
3. **Lc — group the opaque sweep by texture (stable sort).** Bit-identical pending the stable-sort CRC
   check; keeps the working set hot.

(Q2 / Q5 are high-impact but pixel-changing → they belong with the rebake-gated structural work in §7,
not the free-wins bucket.)

Sequencing: the profiler-validity fix (Q4: `sweep_xlu`→coarse + the opaque setup/fill/ns-px anchors) is
**already landed in this worktree** — every before/after number below is now wrap-free and per-lever
attributable. Then Q3 (free) → Q1 (free, big) → measure.

---

## 6. Long-term architectural improvements

1. **L6 — XLU overdraw control.** The biggest single cost (570 ms, 64.5%). Front-to-back + alpha-
   saturation early-out = the z-write XLU gave up, recovered as a cull. Promoted to a **primary** target
   (not a tail item) because XLU is 2× opaque and 100% overdraw-bound.
2. **L1 — DDA interpolants.** The structural fix and the largest per-pixel cut: hoist 6 of 8 divides + ~9
   mults to setup, step with adds. **Golden-neutral** via exact remainder carry. Foundation for L2/L3.
3. **L2 — per-span perspective with overlapped SIO divide.** Kills the last 2 divides/px; first use of the
   idle SIO HW divider; with L1 drives per-pixel divides to ~0.
4. **Q2 — affine on small/distant tris.** Skips even L2's checkpoint divide for the dominant small-tri
   population.
5. **L5 + Q5 — cull billboards; mip + point distant.** Feed the loop fewer, cheaper-to-sample fragments;
   L5 also fixes the 184-tri cap-drops (visible foliage).
6. **L4 — surface-cache the combiner.** Only after Q5 shrinks surfaces to a cache-fitting size; gated hard
   on the 16KB-cache hazard.
7. **L3 — packed-fraction texel stepper in ARM asm.** The hand-tuned endgame, after the C algorithm (L1/L2)
   is bit-identical and stable.
8. **Profiler hardening (carry-forward):** keep `sweep_xlu`→coarse + the opaque setup/fill/ns-px anchors —
   they are the per-lever scoreboard for everything above; add per-scanline fill spans so FINE never wraps.

**Architectural note:** do **not** add cores or chase present — the back-end is 98% utilized and present is
5%. The entire return is in the per-pixel inner loop (shared by opaque + XLU) and XLU overdraw.

---

## 7. Implementation roadmap (phased, with dependencies)

**Phase 0 — Trust the instrument (DONE in this worktree).** Q4 profiler validity (`sweep_xlu`→coarse;
opaque setup/fill/ns-px anchors; `prof_wrap=0`). *Why first:* every before/after number must be wrap-free
and per-lever attributable. *Exit:* met — true XLU magnitude (570 ms, 2× opaque) known.

**Phase 1 — Free, bit-identical quick wins (no golden rebake).** Q3 (z-test before shading) → Q1 (SRAM-
resident loop) → Lc (texture-coherent opaque sweep). Re-profile after each (ch.39: the bottleneck moves).
*Why:* highest impact-per-risk; all preserve fb_crc → ship behind the existing dual==serial + golden gates
with no fidelity debate. *Dep:* Phase 0. *Exit:* measured cyc/px drop; goldens unchanged.

**Phase 2 — Attack the biggest cost: XLU overdraw (rebake).** L5 (conservative frustum cull — bit-
identical slice first; then the draw-distance cap) → L6 (front-to-back + alpha-saturation early-out).
*Why:* XLU is 2× opaque and the single largest lever; L5 shrinks the pixel count cheaply and recovers the
cap-drops, L6 deletes the deep overdraw layers. *Dep:* Phase 1 (clean baseline). *Exit:* XLU core-ms
toward the opaque magnitude; `s_xlu_dropped`→0; new golden, on-target host==device + dual==serial.

**Phase 3 — Structural inner loop (R-lane, serialized, test-first; helps BOTH sweeps).** L1 (DDA, exact
remainder carry — **golden-neutral**, validate vs the oracle) → L2 (per-span perspective + overlapped SIO
divide, rebake) → Q2 (affine on small tris, rebake). One sub-stream at a time on `raster_one`/
`raster_one_xlu`, each gated by the float oracle + dual==serial + golden. *Why:* the biggest per-pixel cut,
but it touches the proven core — so it goes behind tests, after the free wins (Phase 1) and the biggest-cost
win (Phase 2) have de-risked the baseline and isolated L1's contribution. *Dep:* Phase 0 (measure) +
Phase 1/2. *Exit:* per-pixel divides gone (8→~0); cyc/px approaching Abrash territory.

**Phase 4 — Sampling + caching + asm endgame (only if still short of target).** Q5 (mip + point distant,
rebake) → L4 (surface-cache the combiner, **only on Q5-mipped ≤64×64 tiles**, gated on the cache hazard) →
L3 (packed-fraction ARM asm, after L1/L2 are bit-identical and stable). *Why:* highest effort, diminishing
once L1/L2/L6 land; gated on the perf-characterization showing residual headroom. *Dep:* Phase 3.

**The bottleneck moves (ch.39 — re-probe between every phase).** Expected migration: after **Phase 2**,
the frame's center of mass shifts from XLU to the **opaque sweep (286 ms)**; after **Phase 3**'s divide
removal, opaque per-px drops toward XLU's and the ceiling moves to **texel cache misses + the (now
relatively larger) present (5%)**; once divides + combiner are gone, the ceiling is the **memory subsystem,
not instruction count** — at which point the next lever is SRAM/texture residency + mip (Q1/Q5), not more
arithmetic. Host cycle counts lie about the 16KB XIP cache → the on-target-probe (these anchors) is ground
truth.

**The L1-vs-L6 ordering call (recommendation).** Both are top-tier. I sequence **L6 before L1** (Phase 2
before Phase 3) because: L6 is the single biggest cost and a lever *unique* to XLU (opaque can't use it),
so it's pure additive headroom; and L1 touches the proven `raster_one` core, so it benefits most from the
de-risked baseline L6+Phase-1 establish. But L1 is **golden-neutral and helps both sweeps**, so if the
R-lane owner prefers to bank the bit-identical structural win first, swapping Phase 2↔3 is defensible —
the dependency graph (both depend only on Phase 0/1) permits either order.

---

## Abrash chapter → our-loop correlation (index)

| Lever | Abrash chapters | Attacks | Our site |
|---|---|---|---|
| L1 DDA | 56, 57, 66 | 6 of 8 ÷/px + ~9 mul/px | raster.cc:540-585, 428; tri_setup:286 |
| L2 per-span persp. | 68, 70, 66 | the 2 perspective ÷/px (→ idle SIO divider) | perspective_texcoord_q16:408/416 |
| Q2 affine | 56, 70 | all ÷ on small/distant tris | tri_setup:286; raster_one/xlu UV path |
| Lever 4 (gate) | 52, 53, 54, 62 | determinism (round-half), no-UMULL, overflow audit | fixed.cc:32-82; raster.cc:206-209, 404-407 |
| Q1 SRAM-resident | 57, 68 | XIP I-fetch stalls (both sweeps) | raster.cc:446/680; tex.cc; shade.cc |
| Q5 mip/point | 68, 70 | 8 texel fetches/px | tex.cc:204/235 (dead lod); raster.cc:276/605 |
| Lc coherency | 67, 70 | texture-line eviction | raster.cc:924 (bin-order sweep) |
| L4 surface-cache | 68, 69 | per-pixel combiner (gated by cache) | shade.cc:204 (shade_pixel2) |
| Q3 z-before-shade | 39 | shading z-rejected opaque pixels | raster.cc:548 vs 642 |
| L5 cull | 61, 54, 52 | off-screen transforms + cap-drops | geom front-end; raster.cc:148/1014 |
| L6 XLU overdraw | 68, 39 | 53,761 px of billboard overdraw | raster_one_xlu:680/777; xlu sort:857/872 |
| L3 ARM asm | 57, 58 | per-pixel texel address | tex.cc:124; the post-L1/L2 affine loop |

---

## Unresolved questions

1. **L1 exact-remainder cost vs benefit.** The Bresenham `while(rem>=area2)` carry is golden-neutral but
   adds a data-dependent loop; for steep gradients it could step >1 texel/pixel (multi-iteration). Measure:
   is the exact-carry overhead still a net win over the 6 ldivmods it removes, or is the rebake-accepting
   "round-half DDA" (simpler, one add) the better trade?
2. **L6 compositing change.** Front-to-back under-operator + saturation early-out changes the XLU result.
   Is the fidelity hit (faint soft-edge contributions behind a saturated pixel) acceptable, and at what
   `XLU_SAT` threshold? Or is the lower-risk draw-distance-cap-only slice sufficient given L5?
3. **L1-vs-L6 phase order.** Recommended L6 first (biggest, unique, additive); L1 first is defensible
   (golden-neutral, helps both). Which does the R-lane owner want?
4. **Land the profiler instrumentation as a PR?** This worktree holds the Q4 fix + opaque/XLU pixel
   counters — the per-lever scoreboard for all of the above. Land as a small bit-identical PR, or keep as a
   measurement spike?
5. **SIO async-divide save/restore.** L2 overlaps the per-core SIO divider behind the 16px burst. Confirm
   no same-core ISR (USB-CDC on core0) can corrupt an in-flight divide mid-burst, or wrap with
   `hw_divider_save_state`.
6. **Q5 mip bake determinism.** Box-filter + error-diffusion dither must be host==device reproducible (it's
   an offline asset-tool step → safe), and the per-tri LOD selection must be integer-only on-device.

---

## Appendix — anchor-reordering verification (per earlier request)

Disassembled `raster_tile_noclear` (`arm-none-eabi-objdump`, pico-prof build). Timed work sits **strictly
between** the start/end timer reads, per block: SETUP brackets `bl tri_setup`; FILL brackets the inlined
`raster_one` body (`tex_sample_rgba`/`tex_sample`/`shade_pixel2`, the UV/inv_w `lmul`/`ldivmod`). Nothing
hoisted before the start read or sunk after the end read. **Why it holds:** the timer reads are cross-TU
calls through RAM veneers (timers are `__not_in_flash_func`) and no preset enables LTO/IPO → each is an
opaque call that may touch escaped memory, a hard reordering barrier. A `__asm__ volatile("" ::: "memory")`
clobber was tried and **rejected** (it forced `ProfileBlock` members to the stack and turned the compile-
time `flavor` into a runtime branch in the dtor — heavier per-block dtor = more perturbation, for a future-
LTO case no preset builds). If LTO is ever enabled, re-verify and add a *targeted* fence only if the bracket
breaks. Documented in `src/prof/prof.h`.
