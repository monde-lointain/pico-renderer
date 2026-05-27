# Design: Modern Command-Buffer Software 3D Renderer for PicoSystem

## Context

Greenfield project (`renderer/` is empty, no git). Goal: a **practical, reusable software 3D
renderer** for the Pimoroni PicoSystem (RP2040), targeting **240×240 @ 30 FPS**. Architecture is
**inspired by the N64 GBI** (command buffer, display-list reuse, transform→rasterize pipeline,
vertex-batch + indexed triangles, explicit render state) but uses a **clean modern API** — no
literal `gSP*`/`gDP*` macros, no 64-bit packed command words, no hidden global state.

Why these choices drive the design:
- **No FPU, no data cache, 264 KB SRAM** → fixed-point math everywhere; memory is the binding
  constraint, not cache locality.
- A full 240×240×16-bit Z-buffer (~115 KB) cannot coexist with the color framebuffer (~115 KB) →
  **tiled rasterization** with a small per-tile Z scratch.
- Dual Cortex-M0+ @ 250 MHz → tile-parallel rasterization across both cores.

**Why sort-middle tiling (deliberate trade, not obvious):** at ~19 px triangles, binning is nearly
1:1 and we pay ~66 KB to retain transformed geometry — so the choice is a genuine close call vs.
(A) immediate-mode forward + reduced-precision full-screen Z (8-bit = 57.6 KB; no retention/binning,
simpler, but Z-fighting + screen-split Z for dual-core) and (B) screen-split immediate (doubles
transform work). Sort-middle wins for one reason: it **transforms each vertex once *and* parallelizes
raster** without the 2× transform of screen-split — decisive only because transform is ~40% of the
budget. **Phase 0 measures the transform:raster ratio; if transform is cheaper than estimated,
immediate-forward + 8-bit Z is the simpler fallback architecture.**

## Targets / Non-Goals

- **Targets:** 240×240, RGB565 (or ARGB4444) output, **30 FPS at ~3000 triangles/frame (~100K
  tri/s)**; textured + Gouraud + per-vertex lit + alpha-blended triangles; full perspective-correct
  texturing (**adaptive: affine on small tris, perspective on large**); RDP-style coverage
  **anti-aliasing** (full, on all triangles). Budget: 16.7M cyc/frame across 2 cores; avg triangle
  ≈19 px → scenes are **bimodal** (few large surfaces where perspective + AA matter; many tiny tris
  where per-vertex/per-triangle setup dominates and per-pixel cost should be minimized).
- **RDP/RSP-derived feature set** (selected from the N64 manual): a configurable **color combiner**
  (modulate/decal/blend + PRIM/ENV registers), **fog**, ordered **RGB dither**, **alpha
  compare/cutout**, **texture sampling** (wrap/mirror/clamp + 3-point filter + per-triangle mip
  LOD), **texture compression** (CI4/CI8 palette + IA8/IA4), **specular + environment mapping**
  (texgen), **volume/AABB cull + LOD branch**. Plus freebies: Z **decal mode**, perspective
  normalize, configurable **N directional lights**, **reduced-AA** fill-bound fallback.
- **Non-goals (v1):** full per-pixel trilinear mip, YUV textures, chroma key, line/copy microcode
  modes, 32-bit framebuffer, interlace, full MSAA/supersampling, dynamic shadows, runtime asset
  loading, a 2D sprite/UI layer (deferred extension). Architect so these can be added.

## Hardware Facts (binding)

| Resource | Value | Consequence |
|----------|-------|-------------|
| Display | 240×240, ARGB4444, ST7789 via PIO+DMA | Reuse picosystem `_fb` + `_flip()`/`_wait_vsync()` |
| CPU | 2× Cortex-M0+ @ **250 MHz (overvolt 1.20 V) — required** | Fixed-point; at the safe 125 MHz the budget halves (~1500 tris). 250 MHz stability assumed; not for non-overclocked units |
| Divide | HW integer divider, ~8 cyc (per-core, SIO) | Makes perspective divide affordable |
| **Multiply** | **`MULS` 32×32→32 only; NO `UMULL`/`MLA` (ARMv6-M)** | **64-bit products synthesized from 16×16 partials → keep operands ≤16-bit where possible; transform loop likely hand-tuned asm** |
| Interpolators | 2× per core (INTERP0/1) | Edge/attribute stepping, UV, blends |
| SRAM | 264 KB total; ~115 KB to color FB | ~120 KB free; tight at 3000-tri target |
| Multicore | FIFO (8×32b) + 32 spinlocks | Tile work-queue, frame handoff, parallel front end |
| XIP | 16 KB cache for flash code **and** texture reads | Hot raster/transform code → SRAM (`__not_in_flash_func`); textures swizzled for locality |

## Architecture Overview

```
App builds command buffer (per frame; static geometry via reusable display lists)
        │
        ▼
[ FRONT END  — cores 0 & 1 ]  Command interpreter + geometry (parallel)
   core 0 walks commands → maintains state (matrix stack, material, render mode), emits draw jobs
   both cores: transform + light verts → near + guard-band clip → project → compact transformed verts
               emit triangle refs into per-tile bins (atomic append)
        │   (barrier: binning complete)
        ▼
[ BACK END  — cores 0 & 1 ]  Tile rasterizer (atomic tile queue)
   per tile: clear Z scratch → opaque pass (Z test+write) → translucent pass (sorted, Z test no write)
             write color into FB + per-pixel coverage into per-tile coverage scratch
        │   (tiles complete top-to-bottom; scanout DMA chases behind — see scanline race)
        ▼
[ AA RESOLVE  — cores 0 & 1 ]  Edge-gated coverage filter, per-tile (consumes tile coverage scratch)
   per pixel with coverage<full: blend toward neighbors weighted by (1-coverage); optional divot
        │
        ▼
   scanline-race flip → ST7789
```

**Both front and back ends are dual-core** (required by the 3000-tri budget). Core 0 walks the
command stream (matrix stack, current material/render state) and emits **draw jobs** — each a
`LOAD_VERTS`+`DRAW_TRIS` unit tagged with a **render-state version id** (state snapshots are
interned/versioned, not copied per job). Both cores drain the job queue, transforming/lighting verts
into private arenas and appending triangle refs to **per-core bin segments** (merged per tile at the
barrier). For our **mostly-rigid scenes** the serial walk is cheap (few matrix muls/frame), so
parallelizing only the transform/setup suffices; if matrix-heavy content appears later, the walk's
matrix work is the thing to parallelize next. A bin→raster barrier separates the stages.

## Modules (each: purpose · key interface · depends on)

1. **`fixed` (math)** — Q-format fixed-point + vec/matrix. `fx_mul/div`, `mat4`, `vec4`,
   transform/normalize. Depends on: SIO divider, ROM float (setup only). Pure, unit-testable on host.
2. **`cmd` (command buffer)** — tagged command structs + bump-allocated buffer; display-list
   record/replay/branch. Two lifetimes: **transient** per-frame buffers (from the frame arena, reset
   each frame) and **retained** display lists (app-owned persistent RAM or baked in flash, recorded
   once, replayed via `CALL_LIST` across frames) — the retained-mode payoff. `cb_reset`, `cb_push_*`,
   `cb_call_list`. Depends on: arena (transient only).
3. **`arena` (frame memory)** — bump allocators for command buffer, transformed-vertex pool, tile
   bin nodes. `arena_alloc`, `arena_reset`. Depends on: nothing.
4. **`geom` (front end)** — interpret commands, matrix stack, vertex transform + lighting (N
   directional + ambient, optional specular/env **texgen**), fog factor, near-plane clip,
   projection, **volume/AABB cull + LOD-branch** evaluation, triangle setup-lite, tile binning.
   `geom_run(cmdbuf, &frame)`. Depends on: fixed, cmd, arena, clip.
5. **`clip`** — near-plane + **screen guard-band** polygon clipping (Sutherland-Hodgman) into a
   per-frame clip scratch; output tris counted against the cap. Interior X/Y handled by tile scissor
   + bbox. `clip_tri(tri) -> verts`. Depends on: fixed.
6. **`raster` (back end)** — per-tile triangle rasterization: edge functions, Z test (+ decal-Z
   mode), perspective texture sample, Gouraud, **combiner → alpha-compare → blend → dither**,
   framebuffer write + **per-pixel coverage**. `raster_tile(tile, bin, fb, z)`. Depends on: fixed,
   tex, shade, blend, interpolators.
6b. **`aa` (coverage resolve)** — post-raster, pre-flip edge filter (CPU analogue of the N64 VI):
   for pixels whose stored coverage < full, blend toward neighbors weighted by (1−coverage),
   optional divot speckle filter. `aa_resolve(fb, y0, y1)` (row range → dual-core). Depends on:
   fixed, blend.
7. **`tex`** — texture descriptors + samplers. Formats: RGBA (565/4444), **IA8/IA4, I8/I4,
   CI4/CI8 + TLUT palette**; wrap/mirror/clamp per axis; **3-point filter** or point; **per-triangle
   mip-level** select. Lives in flash/XIP. `tex_sample(tex, u, v, lod)`. Depends on: fixed.
8. **`shade`** — the fixed-function pixel pipeline: configurable **color combiner** `(A−B)·C+D`
   (modulate/decal/blend presets, inputs TEXEL/SHADE/PRIM/ENV/COMBINED), **fog** lerp, **alpha
   compare** (threshold/cutout). `shade_pixel(state, texel, shade) -> (color, alpha, keep)`.
   Depends on: fixed.
8b. **`blend`** — framebuffer blend ops (opaque copy, alpha-over, add, etc.) + ordered **RGB
   dither** at pack time. `blend_pixel(mode, src, dst)`. Depends on: fixed.
9. **`sched` (multicore)** — launch/park core1, distribute front-end transform jobs across both
   cores, bin/raster barrier, atomic tile counter (scanout order), scanline-race DMA watermark,
   frame handoff. `sched_geom(&frame)`, `sched_rasterize(&frame)`. Depends on: pico-sdk multicore,
   spinlocks, FIFO.
10. **`rdr` (façade)** — top-level frame API: begin/submit/end + display flip. `rdr_begin_frame`,
    `rdr_submit(cmdbuf)`, `rdr_end_frame`. Depends on: all above + picosystem SDK.

## Key Data Structures

- **Command** — tagged union: `SET_MATRIX(target,push,mat)`, `POP_MATRIX`, `SET_VIEWPORT`,
  `SET_MATERIAL(tex,combine,rendermode)`, `SET_COMBINER(mode|custom)`, `SET_PRIM_COLOR`,
  `SET_ENV_COLOR`, `SET_FOG(near,far,color)`, `SET_RENDERMODE(zmode,alpha_cmp,blend,flags)`,
  `SET_LIGHTS(n, dirs[], ambient)`, `SET_TEXGEN(on, hilite/env)`, `LOAD_VERTS(ptr,count,base)`,
  `DRAW_TRIS(idx_ptr,tri_count)`, `CULL_VOLUME(verts) -> skip-list`, `BRANCH_LESS_Z(vtx,z,dl)`,
  `CALL_LIST(ptr)`, `RETURN`, `END`. Explicit, no hidden state beyond the command-driven matrix
  stack + the small render-state block these commands mutate.
- **Vertex (input)** — pos `int16[3]`, uv `int16[2]` (S10.5), and **either** RGBA color **or**
  normal+alpha (16 B, GBI `Vtx`-union style). A per-`DRAW_TRIS`/material flag selects pre-lit RGBA
  vs normal+lighting, so both static-lit and dynamic-lit geometry are supported in v1.
- **TransformedVertex (arena)** — **compact ~12–16 B**: screen x/y (Q12.4, 2×16b), `inv_w` (16b),
  `u*inv_w`/`v*inv_w` (2×16b), rgba (16b). Compaction is required to retain ~3000 verts in the
  tiled/sort-middle model (28 B would overflow SRAM). Transformed once per `LOAD_VERTS`, indexed by
  `DRAW_TRIS`.
- **Tile bin** — per-tile triangle-ref list (index into transformed-vertex pool + material id),
  built via per-core segments merged at the barrier (avoids cross-core contention). Triangle setup
  (edge eqs, gradients) computed at raster time per tile — bounds memory, localizes work (small
  ~19px tris hit 1 tile, so setup recompute is rarely duplicated). **Bin-arena overflow policy:**
  fixed cap sized to the 3000-tri budget; on exhaustion, drop excess triangles (counted + surfaced
  in debug) rather than corrupt — never silently.
- **Z scratch** — one per core: `tile_w × tile_h × uint16` depth. Cleared per tile.
- **Coverage scratch** — one per core, tile-sized (~4 KB), consumed by per-tile AA resolve (565
  build); or the full-screen nibble/buffer in the global-resolve configs.

## Frame Execution (dual-core)

1. `rdr_begin_frame` → reset arenas, clear color FB + (per-tile) buffers.
2. App records/`submit`s command buffer(s); static geometry replayed via `CALL_LIST`.
3. `geom_run` (**both cores**): core 0 walks command state and emits per-draw transform jobs; both
   cores transform/light/clip/project verts into private arenas and append triangle refs to per-core
   bin segments. Barrier + merge segments into per-tile lists.
4. `sched_rasterize`: both cores atomically claim tiles **in scanout order (top→bottom)**; each runs
   `raster_tile` (own Z + coverage scratch) then per-tile AA resolve. Completed tile-rows free the
   scanout DMA to chase behind (scanline race).
5. `rdr_end_frame` → ensure DMA caught up, present.

## Memory Budget (grounded in linker reality)

RP2040 SRAM = **256 KB main** (`RAM`, 0x20000000) + **2× 4 KB** `SCRATCH_X`/`SCRATCH_Y`
(`memmap_default.ld`). The scratch banks + `.data` hold what must be RAM-resident: `.ram_vector_table`,
`.time_critical*`/`__not_in_flash_func` (flash erase/program routines — *all* executing code must be
in SRAM during a flash write, relevant only if the game persists save data), and both core stacks.
picosystem's 32 KB spritesheet is `const` → lives in **flash**, not SRAM.

Budget at **3000 tris/frame** (the tiled/sort-middle model must retain all transformed geometry until
raster — this is the dominant pressure):

| Item | Size |
|------|------|
| Color FB (240×240×2, ours; 565 or 4444) | 115 KB |
| SDK `.data`/`.bss`/vectors/stacks (est.) | ~20 KB |
| **Realistic free pool** | **~120 KB** |
| Transformed-vertex pool (~3000 verts × ~14 B, **compacted**) | ~42 KB |
| Tile bins (3000 tris × refs, per-core segments) | ~24 KB |
| Z scratch ×2 cores (60×60×2) | ~14 KB |
| Coverage scratch ×2 cores (per-tile, 565 build) | ~8 KB |
| Command buffer arena | ~16 KB |
| Hot raster/transform code in SRAM (`__not_in_flash_func`) | ~8–12 KB |
| Textures (flash/XIP; hot ones optionally copied to SRAM) | mostly flash |

≈100–106 KB → fits ~120 KB with thin margin **only because** coverage uses per-tile scratch (not the
28.8 KB full-screen buffer) and vertices are compacted to ~14 B. **Key consequence:** at 3000 tris
the retained-geometry cost (~66 KB) approaches the 115 KB full-screen Z-buffer that tiling avoids —
tiling still wins on per-tile Z precision + dual-core parallelism, but the memory margin is slim.
The transformed-vertex/triangle cap is the primary tunable. **Validate real free RAM on-device in
Phase 0** before locking sizes.

## Conventions

- **Coordinate system**: right-handed, **CCW front-facing**, NDC **z ∈ [0,1]** (suits the fixed-point
  w-buffer), clip-space Y-up, **screen Y-down** (row 0 at top, matching the framebuffer/scanout).
  Backface cull compares signed screen-space area against the winding, **flipping with the modelview
  determinant sign** (mirrored scale). `mtx_*` helpers and the asset exporter emit to this convention.
- **Rasterization fill rule**: **top-left** tie-break — a pixel center on a shared edge belongs to
  exactly one triangle, so adjacent tris neither double-cover nor gap. Sample at **pixel center**
  (0.5 offsets); texel addressing uses center sampling for point, edge-clamped for 3-point.
- **Subpixel precision**: Q12.4 (4 bits) screen coords for edge eval; revisit if edge jitter shows.
- **Degenerate triangles** (≤ epsilon area) are rejected in setup (no div-by-zero in the 1/area
  reciprocal).
- **Textures are power-of-2** (mask-based wrap/mirror; non-pow2 would force a per-texel modulo) with
  a max dimension cap; **CI palettes (TLUT) are SRAM-resident** (CI sample = index fetch from flash +
  palette fetch from SRAM — avoids two dependent flash reads).
- **Vertex batches**: indexed arrays, **16-bit indices**, max batch sized to the arena; `LOAD_VERTS`
  base+count addresses the transformed pool (no fixed N-entry HW cache — arbitrary indexed lists).
- **Guard-band**: clip to an expanded screen rect (a few× screen) before edge setup; geometry inside
  the band is span-clamped, only band-crossers are true-clipped — keeps Q12.4 coords in range.
- **No gamma / linear-ish display space** (VI gamma dropped); lighting in display space (retro-
  conventional, cheap). **No destination alpha** semantics relied upon (565/4444 limitation).

## Math

Fixed-point throughout. Proposed formats: matrices/transform Q16.16; screen coords Q12.4
(subpixel edge eval); depth as **1/w (w-buffer)**; perspective attributes Q16.16; colors 8-bit per
channel internally, packed to 565/4444 on write. Divides use SIO HW divider. **Multiply discipline
(no `UMULL` on M0+):** choose operand ranges so products fit 32 bits and use single-cycle `MULS`
where possible; reserve synthesized 64-bit products (16×16 partials) for the few places that need
them; the transform/edge-setup hot path is a candidate for hand-written asm. Interpolators set up
for per-pixel edge/attribute increments. Floats only at setup/asset-tools, never in inner loops.

## Rasterizer Inner Loop (per tile, per triangle)

Half-space edge-function rasterizer clipped to tile bounds. Per pixel inside: incremental edge
values → barycentric/`inv_w` → **perspective divide** (`w = 1/inv_w`, HW divider) → `u=u/w·… ,
v=v/w` → `tex_sample` → modulate with interpolated Gouraud color → `blend_pixel` vs FB →
Z test/write (1/w) → **write coverage** (min of the 3 normalized edge distances; full for interior)
to the nibble or per-tile coverage scratch. **Adaptive perspective:** at triangle setup, choose by
screen extent — **affine UV (no divide) for small triangles** (the common ~19px case, where affine
is visually identical) and **perspective for large triangles** (per-span divide, escalating to
per-pixel only if needed). This skips the per-pixel divide on the many tiny triangles where it can't
be seen; the threshold is a Phase-15 tunable.

## Transparency

Two passes per tile: (1) opaque triangles, Z test + Z write, any order; (2) translucent triangles
sorted back-to-front (per-tile, by `inv_w`), Z test but **no** Z write, alpha-over blend. Sorting
is per-tile and small.

## Color Format, ST7789 Driver & Fallback Configuration

Color depth and AA-coverage storage are **compile-time options** behind a thin config header, so
"can't afford the RAM" is a one-line change, not a redesign. Default chosen after the Phase-0
on-device free-RAM measurement.

**RGB565 path (preferred for 3D — 65 K colors, no banding on Gouraud/lighting ramps):** fork the
picosystem display path (we own FB + flip anyway). Concrete, confirmed changes:
- **COLMOD (0x3A):** write `0x55` (16-bit/565) instead of picosystem's `0x03` (12-bit/444).
  Datasheet p.224 §9.1.32 (bits[6:4]=101 RGB, bits[2:0]=101 control); matches Adafruit ST7789. MADCTL stays `0x04`.
- **`screen.pio`:** remove the two `out null, 4` alpha-discards, change `set x, 11` → `set x, 15`
  (16 data bits/pixel). Net scanout *drops* to ~10–11 ms/frame (no wasted discard cycles).
- **Packing:** `rgb()` → `(r>>3)<<11 | (g>>2)<<5 | (b>>3)`. FB stays `uint16_t[240*240]` (115 KB),
  DMA word count unchanged. We supply our own color/blend path; picosystem 2D primitives (which
  assume the 4444 layout) are not used by the 3D renderer.

**ARGB4444 path (fallback / fast-bring-up):** reuse picosystem `_flip`/PIO/COLMOD unchanged (zero
display work), coverage free in the alpha nibble. 4096 colors; mitigate banding with cheap ordered
dither.

**Fallback ladder if SRAM is tight (cheapest-impact first):**
1. **Right-size the transformed-vertex arena** (biggest, most flexible lever — a triangle-budget
   cap; 1500→800 verts frees ~20 KB, more than the coverage buffer costs). Structural-free.
2. **Reduce coverage precision** — full-screen 2-bit (14.4 KB) or 1-bit (7.2 KB).
3. **Per-tile coverage scratch (~4 KB) + inline resolve** (accepts minor 1px tile-border AA seams).
4. **ARGB4444 build** — drops coverage memory *and* the driver fork entirely.
5. **Pixel-doubled 120×120** (picosystem `PIXEL_DOUBLE`) — FB→28.8 KB, everything 4× smaller;
   frees ~85 KB at a large quality cost. Last-resort low-memory mode.

Also: keep renderer hot loops out of `.time_critical`/scratch unless profiling demands it; reserve
a small RAM budget for flash-write routines if the game persists data.

## Anti-Aliasing (RDP-adapted, coverage-based)

The N64 RDP does AA in two stages: (1) per-pixel **coverage** (3-bit, from 8 subpixel edge samples)
written to dedicated framebuffer coverage bits and used for edge blending; (2) a **VI scanout
filter** that re-blends partial-coverage pixels against fully-covered neighbors (offset of
second-min/second-max), plus an optional **divot** median-of-3 to remove speckle. We adapt this —
not bit-exactly — with five changes that exploit our renderer's specifics:

1. **Analytic coverage from edge functions, not subpixel sampling.** The half-space rasterizer
   already evaluates the 3 signed edge functions per pixel. Normalize each by its edge-gradient
   magnitude (precomputed reciprocal at triangle setup) to get a signed pixel-distance to the edge;
   `coverage ≈ clamp(0.5 + dist)` and the per-pixel coverage is the min across the 3 edges. This is
   continuous, cheaper, and more accurate than the RDP's 8-sample mask, and reuses values we already
   have. Interior pixels saturate to "full".

2. **Coverage storage is build-configurable** (see Color Format section). Two paths: in the
   **ARGB4444** build, coverage rides in the free alpha nibble (discarded at scanout → zero extra
   SRAM, 4-bit/16-level); in the **RGB565** build (full 16-bit color, no spare bit), coverage lives
   in a **separate buffer** — full-screen 4-bit (28.8 KB) for an order-independent global resolve,
   or a small per-tile scratch (~4 KB) with inline resolve. Precision is dialable (4/2/1-bit).

3. **Edge-gated resolve** using the RDP offset form per channel: `out = center + ((nmin + nmax −
   2·center) · (full−c)) >> shift`, `nmin/nmax` = min/max of a small neighbor set (3-tap cross to
   start). **Interior pixels (full coverage) are skipped**, so cost scales with edge length, not
   area. Two resolve scopes, set by where coverage lives:
   - **Global, order-independent** (preferred, when a full-screen coverage source exists — the
     ARGB4444 free nibble, or a 565 + 28.8 KB buffer at lower poly counts): resolve the whole FB
     after all tiles, reading the composited image → no halos from any-order opaque draw.
   - **Per-tile** (default in the 565 + 3000-tri build, where only a ~4 KB per-tile coverage scratch
     fits): resolve each tile right after rasterizing it, reading neighbor *colors* from the global
     FB and center *coverage* from the live scratch. Accepts a minor 1px AA error only at tile
     borders where a silhouette crosses into a not-yet-rendered neighbor. This scope is what the
     scanline-race scanout consumes.

4. **Optional divot** — for partial-coverage runs, replace center with median(left,center,right)
   per channel to kill isolated-edge speckle. Off by default; cheap toggle.

5. **Dropped RDP baggage**: no CVG accumulation modes (CLAMP/WRAP/ZAP/SAVE), no framebuffer
   coverage-bit semantics, no VI scale/gamma/dither stage — none are needed for our fixed 240×240
   output. Translucent-pass pixels write coverage = full (AA ignores them) in v1.

6. **Reduced-AA fill-bound fallback** (from pro24): a mode that resolves only silhouette edges
   (coverage-partial pixels bordering very different neighbors) and skips internal-edge work —
   trades a little quality for speed when the frame budget is tight. Compile/runtime toggle.

**AA cost at high poly density (accepted):** the "interior pixels skipped → cheap" property holds
only when interiors dominate. At ~19px triangles, edge≈area (≈15 of 19 px are edge pixels), so the
edge-gate barely fires and the resolve is *not* cheap, plus there are many internal edges. Decision:
**keep full coverage AA on all triangles** anyway (uniform quality) — but flag this as a real budget
risk; if Phase-15 profiling overruns, the first AA lever is to **gate AA to large-triangle
silhouettes** (where it both costs less relatively and matters most).

**Internal-edge limitation (be honest about it):** because we dropped RDP coverage *accumulation*, a
pixel fully covered by two abutting triangles still stores only the last triangle's *partial*
coverage, so it's flagged as an edge. The composited post-pass mostly cancels this — the partial
pixel's neighbors are the *same surface*, so the offset-form blend is ≈ a no-op — but on Gouraud-
shaded adjacent tris it can slightly soften/shimmer internal edges. Accepted for v1 (the exact fix
is coverage accumulation, which we omit for cost). The top-left fill rule keeps it to genuinely
shared edges. Tunables: neighbor footprint (3- vs 6-tap), divot on/off — settled in Phase 15.

## Clipping / Culling

- Backface cull at setup (signed area sign, mode-selectable: front/back/both/none).
- Near-plane clip before projection (Sutherland-Hodgman → fan re-triangulation).
- **Guard-band**: clip only triangles crossing an expanded screen rect; interior geometry is
  bbox-vs-tile-scissor + span-clamped (keeps Q12.4 coords in range without per-triangle X/Y clip).
- **Volume/AABB cull**: `CULL_VOLUME` transforms a small bounding hull; if fully off-screen, the
  front end skips to the matching `RETURN`/end-of-list — whole objects rejected before transform.
- **LOD branch**: `BRANCH_LESS_Z` tests one vertex's depth against a threshold and branches the
  display list → distance-based mesh swapping at near-zero cost.
- **Z modes**: opaque (test+write), transparent (test, no write), **decal** (write only within a
  dZ tolerance of stored Z → coplanar decals without z-fight).

## Texturing

Textures in flash (XIP-cached). Descriptor: ptr, w, h, format, wrap-S/T, mip chain.
- **Formats**: RGBA (565/4444), IA8/IA4, I8/I4, CI4/CI8 (+ TLUT palette in RAM/flash). CI/IA give
  ~¼–½ the flash of RGBA; CI4 supports 16 palettes for recoloring. Each format = a decode path in
  `tex_sample` (small switch; palettes = one indexed load).
- **Wrap modes** per axis: wrap (mask), mirror, clamp — integer-coord ops in the sampler.
- **Filtering**: **point sampling is the default** (protects the 3000-tri budget); N64-style
  **3-point triangular filter** (3 fetches + 2 lerps) is **opt-in per material**.
- **Mip LOD**: precomputed mip chain; **one level chosen per triangle** from its screen-space size
  (cheap log2). Opt-in per material; kills distance shimmer at a fraction of trilinear cost.
- **Texgen** (specular/env) is likewise opt-in per material. Rationale: with ~19px triangles the
  budget lives in per-vertex/per-triangle work, so expensive per-pixel sampling must be selective.
- **Texture locality**: store textures swizzled/tiled (Morton or block order) so perspective-warped
  access stays within the 16 KB XIP cache; hot textures may be copied to SRAM if profiling demands.

## Shading: Color Combiner + Blender + Fog

Fixed-function pixel pipeline (the N64's "shader"), in `shade` then `blend`:
- **Color combiner** — `out = (A−B)·C + D` for RGB and alpha, inputs ∈ {TEXEL0, SHADE, PRIMITIVE,
  ENVIRONMENT, COMBINED, constants}. Ship presets (modulate, decal, blend/interpolate) + a custom
  form. Nearly free: we already have the interpolated SHADE color and the sampled TEXEL.
- **Fog** — per-vertex linear factor from view-z (`clamp((z−near)/(far−near))`), interpolated like
  any attribute, then `color = lerp(color, fog_color, fog_factor)` in the blender.
- **Alpha compare / cutout** — discard pixel when alpha < threshold (or dithered); cutouts still
  write Z, so foliage/billboards are order-independent.
- **Blender** — `out = src·a + dst·b` (alpha-over, add, etc.), then **ordered Bayer RGB dither**
  on the channel truncation at pack time (4×4 screen-space matrix; kills Gouraud/fog banding,
  essential for the 4444 build).

## Lighting

Per-vertex, transform-stage: **N directional lights + one ambient term** (count configurable), no
attenuation (cheap: normal·lightdir dot products, clamped, accumulated, modulated by material/vertex
color). Point lights deferred. **Specular + environment mapping via texgen**: with texgen enabled,
the transformed/normalized vertex normal is projected to screen space to generate (s,t) into a
highlight or environment texture — gives shiny/reflective surfaces without per-pixel lighting (uses
the texture path, so it replaces the base texture on that surface, as on N64). Computed only for
normal-format vertices; pre-lit RGBA vertices bypass lighting.

## PicoSystem Integration

Build on the PicoSystem SDK for boot/`init()`/`update()`/`draw()` loop, input, multicore, and DMA
plumbing. **ARGB4444 build:** reuse the SDK display path unchanged. **RGB565 build:** fork the
display path — our own FB + a patched `screen.pio` (16-bit) and COLMOD `0x55` init (see Color Format
section). Core1 launched via pico-sdk `multicore_launch_core1`.

**Single-buffer scanline race** (no room for a 2nd 115 KB FB): both cores rasterize tiles in
**row-band order** (a band = one tile-row); both finish a band before advancing, so band *N* is fully
final before band *N+1* starts. The scanout watermark = last fully-completed band; DMA chases it
down the screen, overlapping ~the full ~11 ms scanout with compute. The band-lockstep avoids the
straggler-tile stall that a free out-of-order tile queue would cause. Guard: DMA never crosses the
watermark; if compute falls behind the beam, stall DMA briefly rather than tear. Fallback if the
race proves fragile: serialized single-buffer (render-all-then-flip), losing the overlap. Validated
in Phase 0.

## Concurrency Model & Frame Pacing

- **Core ownership**: the renderer owns both cores *during a frame*. Game `update()` runs on core 0
  between frames; core 1 is parked (WFE) and woken via FIFO at `rdr_submit`, re-parked at `present`.
  Game logic is single-core; the renderer's use of core 1 is transparent to it. (No general job
  system — out of scope.)
- **Frame pacing**: target 30 FPS **locked to vsync, adaptive on overrun** — if a frame exceeds the
  budget, present late (variable frame time) rather than hard-stalling to the next 30 Hz boundary,
  avoiding a 30→15 FPS hitch on a single slow frame. `_wait_vsync` (GPIO) gates presents.
- **Multicore correctness testing**: barriers/bin-merge/watermark can't be golden-tested on the
  single-threaded host harness → covered by (a) a host 2-worker simulation of the queue/merge and
  (b) on-device stress scenes with determinism checks (same input → same framebuffer CRC).

## App-Facing API & Math Utilities

The internal command enum is wrapped by an ergonomic builder API: `cb_set_matrix`, `cb_load_verts`,
`cb_draw_tris`, `cb_set_material`, `cb_call_list`, etc., plus a `gu`-style fixed-point math helper
lib (`mtx_perspective`, `mtx_lookat`, `mtx_ortho`, `mtx_rotate/translate/scale`, quaternion optional)
that emits matrices in the renderer's fixed-point format. `SET_MATERIAL` is a convenience bundle that
expands to the granular combiner/rendermode/texture state commands (one source of truth; the bundle
is sugar). Display lists are plain command buffers the app records once and replays via `CALL_LIST`.

## Asset Pipeline (minimal, in scope)

Small offline converters built alongside the renderer:
- **Mesh** → interleaved vertex array (pos/uv/color-or-normal) + index list, in our fixed-point input
  format; optional pre-lighting bake.
- **Texture** (PNG/etc.) → RGBA565/4444, IA, I, or CI4/CI8 + TLUT, **swizzled** for XIP locality,
  with a generated **mip chain** (box/Lanczos).
- Emits C arrays / a flat binary placed in flash. Kept lean; not a general engine pipeline.

## Risks & Mitigations

- **Hitting 100K tri/s on M0+ is aggressive.** No 64-bit multiply → the transform/setup inner loops
  are the make-or-break; plan for hand-tuned (asm) fixed-point with ≤16-bit operands, and treat 3000
  as a stretch target validated in Phase 0/15. Mitigation levers: cap lights, opt-out of per-pixel
  features, reduce divide cadence, lower the cap.
- **Texture bandwidth from flash is the top per-pixel unknown.** Swizzled layout + point-sample
  default + small textures; copy hot textures to SRAM if profiling shows XIP thrash.
- **Coordinate overflow after projection** (near-camera/large tris) → add a **screen-space
  guard-band clip** (clamp/clip to an expanded rect) before edge setup; keep Q12.4 within range.
- **Depth representation**: use a **1/w (w-buffer)** for even precision and because perspective
  already yields `inv_w`; decal `dZ` tolerance expressed in 1/w units. Confirm precision in Phase 6.
- **Transparency ordering**: sort translucent tris per-tile by **centroid 1/w**; intersecting
  translucent geometry is not resolved (documented limitation). Opaque any-order via Z.
- **Bin/vertex arena exhaustion**: hard caps + drop-with-count, never corrupt (see Tile bin).
- **Scanline-race tearing**: row watermark + DMA stall guard; serialized fallback.
- **Normals under non-uniform scale** use plain modelview (no inverse-transpose) — documented
  limitation, as on N64; use uniform scale for lit meshes.
- **Texgen replaces base texture** (1-cycle pipeline) — a surface is textured *or* env/specular, not
  both, in v1.
- **Host/device golden-image parity** requires strict no-float discipline in all pixel/geometry math
  (floats only in setup/asset tools).
- **Clip-generated geometry** (near + guard-band) expands tris; allocated from a per-frame clip
  scratch and **counted against the triangle cap** so a clip storm can't overflow silently.
- **Compact-vertex UV precision**: 16-bit `u·inv_w` limits range for high-repeat tiled textures;
  budget S10.5 UV × reasonable repeat, and allow a per-material "wide UV" path (32-bit) where needed.
- **Lighting space**: normals transformed by modelview upper-3×3; **light directions supplied in
  that same (post-modelview) space** — one source of truth, no per-light per-vertex re-transform.
- **Backface cull sign factors the matrix determinant** (negative/mirrored scale flips winding).
- **CI/palette textures are point-sampled only** (indices don't interpolate) — 3-point/mip on CI is
  rejected at material setup.
- **Decal `dZ` tolerance is depth-scaled** (1/w is non-linear), not a fixed constant.
- **RGB565 has no destination alpha** — only source-alpha "over" and add/min/max blends; dst-alpha
  modes unavailable in that build (4444 too has only 4-bit dst alpha).
- **Supported build configs are a pinned small set** (e.g., 565+per-tile-cov, 4444+nibble-cov),
  not the full toggle cross-product — bounds the test matrix.
- **Render-to-texture / off-screen targets are out of scope for v1** — the single-FB + scanline-race
  + tiled model makes arbitrary render targets awkward; revisit only if a use case demands it.
- **Audio coexistence**: picosystem audio is a **timer-IRQ callback on core 0** (`repeating_timer`).
  It preempts core-0 render work → **reserve core-0 headroom** for audio IRQ cycles + jitter; the
  "both cores symmetric" budget is slightly core-0-light. Confirm IRQ rate/cost in Phase 0. (core 1
  is confirmed free — the SDK doesn't launch it.)
- **Power / thermal (battery handheld)**: both cores at 100% @ 250 MHz/1.20 V overvolt is a real
  battery-drain and heat increase. v1 accepts it (perf target first); note a future option to drop
  to 125 MHz or idle-core when scene load is light. Not optimized in v1, but acknowledged.
- **Overclock is a hard requirement** (resolved): 250 MHz assumed stable; no separate 125 MHz tuning.
  A `NO_OVERCLOCK` build will simply run slower (lower achievable poly count), not separately tuned.
- **Licensing = GPL** (resolved): the AA and other RDP-derived logic may **adapt the GPL
  angrylion-rdp-plus / parallel-rdp references** with proper attribution and GPL compliance (not
  restricted to clean-room). BSD-3 pico-sdk/picosystem and BSD/MIT Adafruit driver are GPL-compatible.
  *Consideration:* plain GPL means games statically linking the renderer inherit GPL — if broad
  third-party reuse is wanted later, revisit LGPL. Recorded, not blocking.

## Verification

- **Host unit tests** for `fixed`, `clip`, triangle setup, binning (compile modules for host; pure
  integer logic is portable). Golden-image compare for rasterizer on host (bit-identical fixed-point).
- **Float reference renderer (host-only oracle)**: a simple, slow floating-point implementation of
  transform/raster/shade to validate *correctness* (not just host↔device *consistency*) — catches
  systematic fixed-point/geometry bugs both targets would otherwise share. Compare within tolerance.
- **On-device**: a demo scene exercising the feature set (spinning textured lit cube, fog, a
  combiner-tinted surface, an alpha-cutout billboard, a translucent quad, a specular/env-mapped
  object) measuring frame time via `time_us`; assert ≤ 33.3 ms. Visual check on hardware; AA and
  dither on/off toggles to confirm edge smoothing, no speckle/halos, no banding.
- **Profiling**: per-stage cycle counters (transform / bin / raster / aa-resolve) printed;
  tile-time + core-balance histograms; **triangles/frame at 30 FPS measured against the 3000 target.**
- **Perf gate**: if over budget, in order — raise the affine/perspective threshold, gate AA to
  large-tri silhouettes, drop per-pixel features to point, cap lights, lower the triangle cap;
  re-measure. Confirm the scanline race holds (no tearing).

## Implementation Phases

0. **On-device bring-up + RAM/timing/arch probe**: blank app; measure free SRAM and scanout timing
   to lock the color-format/coverage default and validate scanline-race vs serialized scanout; run a
   micro-benchmark of vertex-transform vs per-pixel-raster cost to **confirm the transform:raster
   ratio** that justifies sort-middle (vs the immediate-forward + 8-bit-Z fallback architecture).
   If RGB565: patch `screen.pio` (16-bit) + COLMOD `0x55`, verify a solid-color flush.
1. `fixed` math (incl. multiply-discipline helpers) + host tests.
2. `arena` + `cmd` command buffer + display-list replay + app-facing builder API.
3. `geom` front end: transform/light/clip(near+guard-band)/project/cull, host-tested (single-core
   first for correctness, then parallelized in phase 6).
4. Tile binning + compact transformed-vertex format + `raster` single flat triangle on-device.
5. Gouraud + perspective texture (point sample) + basic blend inner loop.
6. `sched`: **dual-core front end** (job fan-out + bin merge) **and** dual-core tile queue + Z/
   coverage scratch per core (+ Z decal mode) + scanline-race scanout.
7. Transparency pass + per-tile sorting.
8. `aa`: analytic coverage + edge-gated resolve (per-tile default; global for 4444/full-buffer);
   optional divot + reduced-AA.
9. `shade`: color combiner + PRIM/ENV + alpha compare/cutout; `blend`: ordered RGB dither.
10. Fog (per-vertex factor → blender lerp).
11. `tex` formats (IA/I, CI4/CI8+TLUT) + wrap/mirror/clamp + (opt-in) 3-point filter + per-tri mip.
12. Lighting: configurable N lights; (opt-in) specular/environment mapping via texgen.
13. Front-end culling: volume/AABB cull + LOD branch.
14. Asset converters (mesh + texture/mip/palette, swizzled) + demo scene exercising the feature set.
15. Profiling, perf gate (validate ~3000 tris @ 30 FPS), tuning (tile size, divide cadence, AA
    footprint, filter/mip, hot-code-in-SRAM, hot-textures-in-SRAM).

## Resolved Decisions

- **Performance target**: ~3000 triangles/frame @ 30 FPS (~100K tri/s), **predicated on a stable
  250 MHz overvolt overclock** (hard requirement); 125 MHz units run slower, untuned.
- **License**: GPL (may adapt GPL RDP references; BSD deps are compatible).
- **Both front and back ends dual-core** (the 3000-tri budget requires parallel T&L + binning).
- **Texture sampling defaults to point**; 3-point filter / mip / texgen are opt-in per material.
- **Depth = 1/w (w-buffer)**.
- **Adaptive perspective**: affine UV on small triangles, perspective on large (reverses the earlier
  uniform full-perspective choice, justified by the ~19px average).
- **Full coverage AA on all triangles** (accepted budget risk at density; fallback = large-tri-only).
- **Architecture = sort-middle tiling**, chosen for transform-once + parallel raster; Phase-0 measures
  the transform:raster ratio with immediate-forward + 8-bit Z as a documented fallback architecture.
- **Scanout**: single-buffer scanline race (DMA chases rasterizer); serialized fallback.
- **Asset toolchain in scope**: minimal mesh + texture/mip/palette converters.
- **Vertex input**: both pre-lit RGBA and normal+lighting via per-draw flag.
- **Lighting**: configurable N directional + ambient (no attenuation); specular/env via texgen;
  point lights deferred.
- **Color clear**: API provides an explicit `CLEAR`/background command (don't assume full overdraw).
- **Texture residence**: sample from flash/XIP in v1; SRAM-resident hot textures deferred as an opt.
- **Feature set**: color combiner (presets+PRIM/ENV), fog, RGB dither, alpha cutout, texture
  sampling (wrap/mirror/clamp + 3-point + per-tri mip), texture compression (CI/IA), specular/env
  texgen, volume cull + LOD branch, Z decal mode, reduced-AA — all in scope (staged, phases 9–13).
- **Deferred extension**: a 2D sprite/UI layer (textured rects) on top of the 3D renderer — not v1.
- **Anti-aliasing**: RDP-adapted coverage AA — analytic edge coverage + edge-gated, order-independent
  resolve pass before `_flip`; full MSAA out of scope.
- **Color format & coverage storage**: compile-time configurable. RGB565 preferred for color, but
  at the 3000-tri memory budget it pairs with **per-tile coverage scratch** (not the 28.8 KB
  full-screen buffer); ARGB4444 keeps the free-nibble full-screen coverage. Default + fallback ladder
  locked after the Phase-0 RAM probe.

## Unresolved Questions (tunables, not blockers)

1. **Tile size / shape** — default 60×60 square (16 tiles); may switch to scanline bands or smaller
   tiles after profiling load balance. Decide empirically in Phase 15.
2. **Perspective divide cadence** — per-pixel target; fall back to per-N-pixels if the Phase 15 perf
   gate fails. Structural support already in the design.
3. **AA neighbor footprint / divot** — 3-tap horizontal cross vs RDP-like 6-tap; divot on/off.
   Decide on quality-vs-cost in Phase 15.
4. **Whether 3000 tris is reachable with the full feature set** — the open empirical question; the
   perf gate's levers (features, lights, divide cadence, cap) tune toward it. Measured in Phase 15.
