// prof.h — granular intra-frame profiler (T5). Carve-out module: the RAII
// ProfileBlock ctor/dtor are a USER-SANCTIONED exception to Orthodox C++
// (modeled on Casey Muratori's "Computer, Enhance!" scope timer) because a
// scope guard has far more utility here than the C alternative. The module is
// NOT orthodoxy_enforce'd; every OTHER rule still holds (POD anchors, C
// headers, C-style casts, plain enum, no STL).
//
// Two timer flavors, chosen by the RP2040's hard timing limits:
//   - PROF_BLOCK(id)     -> time_us_64 (1us, 64-bit no-wrap, SHARED
//   cross-core):
//                          frame / stage spans, AND any per-tile block that can
//                          exceed ~67ms (sweep_opaque does on the worst tile).
//   - PROF_BLOCK_FINE(id) -> SysTick CVR (4ns, per-core BANKED): only for spans
//                          guaranteed < ~67ms where 1us is too coarse (the ~1us
//                          XLU sweep). The reader RESETS CVR per block, so its
//                          COUNTFLAG is a TRUE >67ms detector (no false
//                          positive from a benign zero-straddle); a real
//                          overrun sets wrap=1 and means: move that anchor to
//                          PROF_BLOCK.
// Both store NANOSECONDS so the per-core merge is unit-uniform.
//
// PROFILER=0 (default / golden / CI): PROF_BLOCK* -> ((void)0), the public API
// are empty inline stubs -> zero codegen, fb_crc bit-identical. PROFILER=1 is
// carried by renderer::prof_config (the pico-prof preset /
// -DRENDERER_PROFILER=ON).
#ifndef RDR_PROF_H
#define RDR_PROF_H

#include <stdint.h>

// Default off unless the build switch (renderer::prof_config) bakes PROFILER=1.
#ifndef PROFILER
#define PROFILER 0
#endif

// Anchor IDs — EXPLICIT enum (not Casey's per-TU __COUNTER__, which aliases
// indices across our many .cc files). Single source of truth; sizes the array.
// Index 0 (PROF_NONE) is the null-parent sentinel: it is never a real timed
// block and is never reported — it harmlessly absorbs the outermost block's
// parent-subtract (core0 PROF_ROOT's, and core1 PROF_RASTER_DISPATCH's orphan
// negative). Always visible (plain enum = no codegen) so call sites name the
// id even when PROFILER=0 (the macro discards it).
enum ProfAnchor {
  PROF_NONE = 0,
  PROF_ROOT,             // frame: build..present (== frame_ms span, coarse)
  PROF_CMDGEN,           // demo_terrain_build (front-end scene build, coarse)
  PROF_GEOM,             // rdr_submit -> sched_geom -> geom_run (coarse)
  PROF_CLEAR_BLIT,       // rdr_end_frame: fb clear + 2D sky blits (coarse)
  PROF_RASTER_DISPATCH,  // sched_rasterize wall span incl. core join (coarse)
  PROF_PRESENT,          // plat_present scanout/upload (coarse)
  PROF_RASTER_TILE,      // raster_tile whole tile (both cores, coarse)
  PROF_SWEEP_OPAQUE,  // raster sweep 1 — opaque (both cores, COARSE: >67ms)
  PROF_SWEEP_XLU,     // raster sweep 2 — translucent (both cores, FINE)
  PROF_AA_RESOLVE,    // aa_resolve_tile (both cores, FINE)
  PROF_ANCHOR_COUNT
};

#if PROFILER

// Timer flavor selector (no enum class — Orthodox plain enum).
enum ProfTimer { PROF_T_COARSE = 0, PROF_T_FINE = 1 };

// Raster runs on BOTH cores concurrently, and SysTick is per-core banked, so
// each core owns its OWN anchor row (no atomics: a core writes only its row).
#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
#define PROF_NUM_CORES 2
#else
#define PROF_NUM_CORES 1
#endif

// POD anchor (Casey's exclusive/inclusive accumulators). `excl_ns` is SIGNED:
// a child's dtor does parent.excl -= child before the parent's own dtor does
// self.excl += self, so it transiently goes negative — correct at frame end.
// No `label` field: the explicit enum resolves names via PROF_NAMES[] at
// report.
struct ProfileAnchor {
  int64_t excl_ns;   // self-time (excludes instrumented children)
  uint64_t incl_ns;  // total time (includes children)
  uint64_t hits;     // block entry count
  uint8_t wrapped;   // FINE only: SysTick COUNTFLAG tripped (block > ~67ms)
};

struct ProfileState {
  struct ProfileAnchor a[PROF_ANCHOR_COUNT];
  uint32_t parent;  // current open-block id on THIS core (Casey's parent stack)
};

extern struct ProfileState g_prof[PROF_NUM_CORES];

// --- platform-split timer + core primitives (prof_pico.cc / prof_host.cc) ---
// Hot ones are __not_in_flash_func on device (the renderer hot path is entirely
// flash-resident and the XIP code-cache is only 16KB — un-SRAM'd timer reads
// would evict the very texture/code lines they measure).
uint64_t prof_coarse_read(void);  // time_us_64*1000 / clock_gettime
uint64_t prof_coarse_elapsed_ns(uint64_t start);  // now - start
uint64_t prof_fine_read(void);                    // SysTick CVR / monotonic ns
uint64_t prof_fine_elapsed_ns(uint64_t start, int* overflowed);
uint32_t prof_core(void);  // get_core_num() / 0

// --- public API (called from main.cc; carve-out) ---------------------------
void prof_init(void);  // cache clk_sys Hz; call AFTER plat_init (250MHz)
void prof_systick_enable(void);  // enable SysTick on the CALLING core (banked)
void prof_frame_begin(void);  // core0: zero all cores' anchors + reset parents
void prof_frame_end(uint32_t frame,
                    uint32_t frame_ms);  // accumulate + snapshot
void prof_report_final(void);  // final averaged block (call at bounded exit)

// The one ctor/dtor (user-sanctioned). ctor: save parent, snapshot the anchor's
// old inclusive, become the parent, read the start timer. dtor: compute elapsed
// ns, restore parent, subtract from parent's exclusive + add to self's, fold
// inclusive (recursion-correct via old_incl), bump hits, latch wrap.
struct ProfileBlock {
  ProfileBlock(uint32_t anchor, uint8_t flavor) {
    struct ProfileState* s = &g_prof[prof_core()];
    this->parent_idx = s->parent;
    this->anchor_idx = anchor;
    this->flavor = flavor;
    this->old_incl = s->a[anchor].incl_ns;
    s->parent = anchor;
    this->start =
        (flavor == PROF_T_FINE) ? prof_fine_read() : prof_coarse_read();
  }
  ~ProfileBlock() {
    int ovf = 0;
    uint64_t const elapsed = (this->flavor == PROF_T_FINE)
                                 ? prof_fine_elapsed_ns(this->start, &ovf)
                                 : prof_coarse_elapsed_ns(this->start);
    struct ProfileState* s = &g_prof[prof_core()];
    s->parent = this->parent_idx;
    s->a[this->parent_idx].excl_ns -= (int64_t)elapsed;
    s->a[this->anchor_idx].excl_ns += (int64_t)elapsed;
    s->a[this->anchor_idx].incl_ns = this->old_incl + elapsed;
    s->a[this->anchor_idx].hits += 1;
    if (ovf) {
      s->a[this->anchor_idx].wrapped = 1;
    }
  }
  uint64_t old_incl;
  uint64_t start;
  uint32_t parent_idx;
  uint32_t anchor_idx;
  uint8_t flavor;
};

// __LINE__-unique local name so a scope can hold one of each flavor. No `name`
// arg — `id` indexes PROF_NAMES[] at report time.
#define PROF_CONCAT2(a, b) a##b
#define PROF_CONCAT(a, b) PROF_CONCAT2(a, b)
#define PROF_BLOCK(id)                                        \
  const struct ProfileBlock PROF_CONCAT(prof_blk_, __LINE__)( \
      (uint32_t)(id), (uint8_t)PROF_T_COARSE)
#define PROF_BLOCK_FINE(id)                                   \
  const struct ProfileBlock PROF_CONCAT(prof_blk_, __LINE__)( \
      (uint32_t)(id), (uint8_t)PROF_T_FINE)

#else  // PROFILER == 0 — everything compiles away.

static inline void prof_init(void) {}
static inline void prof_systick_enable(void) {}
static inline void prof_frame_begin(void) {}
static inline void prof_frame_end(uint32_t frame, uint32_t frame_ms) {
  (void)frame;
  (void)frame_ms;
}
static inline void prof_report_final(void) {}
#define PROF_BLOCK(id) ((void)0)
#define PROF_BLOCK_FINE(id) ((void)0)

#endif  // PROFILER

#endif  // RDR_PROF_H
