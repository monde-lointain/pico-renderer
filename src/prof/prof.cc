// prof.cc — platform-agnostic profiler core: per-core anchor state, the
// frame reset, the cross-core merge, and the report. The timer + core
// primitives live in prof_pico.cc / prof_host.cc (link-time substituted, like
// sched's dispatch_host/pico split). Output goes through printf, which on the
// firmware routes to the SAME USB-CDC stdio stream as plat_log (vprintf), so
// the `prof` block interleaves coherently between the per-frame `frame=` lines.
//
// Merge model (load-bearing — see the T5 plan):
//   (a) WALL stages, core0 ONLY, % of ROOT: cmdgen/geom/clear_blit/
//       raster_dispatch/present. raster_dispatch is a WALL span (core0 blocks
//       on the dual-core join) so it is NOT summed across cores.
//   (b) BACK-END core-time, SUMMED both cores, % of cores x dispatch-wall:
//       raster_tile/sweep_opaque/sweep_xlu/aa_resolve.
//   (c) DISCARDED scaffold: PROF_NONE (orphan parent-subtract bucket) and
//       core1's raster_dispatch (parent-only).
// core0 raster_dispatch.excl = the join-idle (core0 done, waiting on core1) =
// the free work-steal imbalance metric (prof_core0_idle_us).
#include "prof/prof.h"

#if PROFILER

#include <stdio.h>
#include <string.h>

// Cumulative run accumulator + frame count (core0-private; folded each frame
// after the join, when g_prof[1] is complete and visible — M0+ has no D-cache,
// the FIFO DONE handshake is the barrier). Averaged at report.
struct ProfileState g_prof[PROF_NUM_CORES];
static struct ProfileState g_acc[PROF_NUM_CORES];
static uint32_t g_acc_frames;

// Opaque-sweep filled-pixel counter: per-core per-frame (g_prof_opaque_px,
// declared in prof.h, written by raster_one), folded into a run total
// (g_acc_opaque_px) each frame after warm-up. ns/pixel = summed
// PROF_OPAQUE_FILL inclusive ns / g_acc_opaque_px (the direct cyc/pixel
// correlation).
uint32_t g_prof_opaque_px[PROF_NUM_CORES];
static uint64_t g_acc_opaque_px;
uint32_t g_prof_xlu_px[PROF_NUM_CORES];
static uint64_t g_acc_xlu_px;

// T5 overdraw probe (per-core per-frame, written by raster_one/raster_one_xlu;
// folded into run totals each frame after warm-up). See prof.h for semantics.
uint32_t g_prof_opaque_zrej[PROF_NUM_CORES];
uint32_t g_prof_xlu_frags[PROF_NUM_CORES];
uint32_t g_prof_xlu_distinct[PROF_NUM_CORES];
uint32_t g_prof_xlu_ge2[PROF_NUM_CORES];
static uint64_t g_acc_opaque_zrej;
static uint64_t g_acc_xlu_frags;
static uint64_t g_acc_xlu_distinct;
static uint64_t g_acc_xlu_ge2;

// Snapshot cadence + warm-up discard (the device loop is infinite at
// DEMO_FRAMES=0, so a periodic snapshot is the only on-device readout; the
// final averaged block fires at the bounded pico-prof exit). Overridable -D.
#ifndef PROF_DUMP_EVERY
#define PROF_DUMP_EVERY 120U  // emit a snapshot every N ACCUMULATED frames
#endif
#ifndef PROF_WARMUP
#define PROF_WARMUP 30U  // discard the first K frames (cold XIP / first-touch)
#endif

// Human labels (the enum is the index). PROF_NONE/PROF_ROOT are not printed as
// stage/back rows; kept here so indices line up 1:1 with ProfAnchor.
static const char* const PROF_NAMES[PROF_ANCHOR_COUNT] = {
    "none",       "root",        "cmdgen",
    "geom",       "clear_blit",  "raster_dispatch",
    "present",    "raster_tile", "sweep_opaque",
    "sweep_xlu",  "aa_resolve",  "opaque_setup",
    "opaque_fill"};

void prof_frame_begin(void) {
  // core0 zeroes ALL cores' rows: core1 is parked (FIFO) between frames, so
  // g_prof[1] is quiescent here; the next GO wakes it after this returns.
  memset(g_prof, 0, sizeof g_prof);
  memset(g_prof_opaque_px, 0, sizeof g_prof_opaque_px);
  memset(g_prof_xlu_px, 0, sizeof g_prof_xlu_px);
  memset(g_prof_opaque_zrej, 0, sizeof g_prof_opaque_zrej);
  memset(g_prof_xlu_frags, 0, sizeof g_prof_xlu_frags);
  memset(g_prof_xlu_distinct, 0, sizeof g_prof_xlu_distinct);
  memset(g_prof_xlu_ge2, 0, sizeof g_prof_xlu_ge2);
  for (uint32_t c = 0; c < PROF_NUM_CORES; ++c) {
    g_prof[c].parent =
        PROF_NONE;  // outermost block's parent = the discard bucket
  }
}

// Integer centi-percent: part/denom * 100, x100 for two decimals. Guard
// denom==0 (the SIO hardware divider returns garbage on /0). No %f (no FPU).
static uint64_t prof_pct_x100(uint64_t part_ns, uint64_t denom_ns) {
  if (denom_ns == 0) {
    return 0;
  }
  return part_ns * 10000ULL / denom_ns;
}

// Average a per-core-summed anchor inclusive over the accumulated frame count.
static uint64_t prof_avg_back_incl_ns(int anchor) {
  uint64_t sum = 0;
  for (uint32_t c = 0; c < PROF_NUM_CORES; ++c) {
    sum += g_acc[c].a[anchor].incl_ns;
  }
  return (g_acc_frames != 0) ? sum / g_acc_frames : 0;
}

static uint64_t prof_avg_back_hits(int anchor) {
  uint64_t sum = 0;
  for (uint32_t c = 0; c < PROF_NUM_CORES; ++c) {
    sum += g_acc[c].a[anchor].hits;
  }
  return (g_acc_frames != 0) ? sum / g_acc_frames : 0;
}

static uint8_t prof_back_wrapped(int anchor) {
  uint8_t w = 0;
  for (uint32_t c = 0; c < PROF_NUM_CORES; ++c) {
    w |= g_acc[c].a[anchor].wrapped;
  }
  return w;
}

// core0 wall-stage inclusive average (ns).
static uint64_t prof_avg_stage_incl_ns(int anchor) {
  return (g_acc_frames != 0) ? g_acc[0].a[anchor].incl_ns / g_acc_frames : 0;
}

static void prof_emit_stage(int anchor, uint64_t root_ns) {
  uint64_t const ns = prof_avg_stage_incl_ns(anchor);
  uint64_t const p = prof_pct_x100(ns, root_ns);
  printf("prof stage=%-14s us=%u pct=%u.%02u\n", PROF_NAMES[anchor],
         (unsigned)(ns / 1000ULL), (unsigned)(p / 100ULL),
         (unsigned)(p % 100ULL));
}

static void prof_emit_back(int anchor, uint64_t disp_denom_ns) {
  uint64_t const ns = prof_avg_back_incl_ns(anchor);
  uint64_t const p = prof_pct_x100(ns, disp_denom_ns);
  printf("prof back=%-13s core_us=%u hits=%u pct_disp=%u.%02u wrap=%u\n",
         PROF_NAMES[anchor], (unsigned)(ns / 1000ULL),
         (unsigned)prof_avg_back_hits(anchor), (unsigned)(p / 100ULL),
         (unsigned)(p % 100ULL), (unsigned)prof_back_wrapped(anchor));
}

// Emit one contiguous prof block: a human-readable two-section view (wall
// stages % of root; back-end raw core-us + % of cores x dispatch) followed by
// flat `prof_<x>=<int>` scalars for run_on_target.py --check (clean key=int, NO
// trailing comments — the parser float()s the value).
static void prof_emit(const char* tag) {
  if (g_acc_frames == 0) {
    return;
  }
  uint64_t const root_ns = prof_avg_stage_incl_ns(PROF_ROOT);
  // Dual-core back-end denominator: cores x the raster_dispatch WALL span
  // (core0). When balanced, summed raster_tile.incl ~ this.
  uint64_t const disp_ns = prof_avg_stage_incl_ns(PROF_RASTER_DISPATCH);
  uint64_t const disp_denom_ns = disp_ns * (uint64_t)PROF_NUM_CORES;

  // core0 join-idle = raster_dispatch exclusive (clamp transient negative).
  int64_t const idle_signed =
      (g_acc_frames != 0)
          ? g_acc[0].a[PROF_RASTER_DISPATCH].excl_ns / (int64_t)g_acc_frames
          : 0;
  uint64_t const idle_ns = (idle_signed > 0) ? (uint64_t)idle_signed : 0;

  uint8_t any_wrap = 0;
  for (int i = PROF_RASTER_TILE; i <= PROF_OPAQUE_FILL; ++i) {
    any_wrap |= prof_back_wrapped(i);
  }
  // ns/pixel for the opaque fill: summed (both cores) inclusive ns of the
  // per-tri fill anchor, over the summed filled-pixel count. The cyc/pixel
  // correlation = ns_per_px * clk_GHz (e.g. 0.25 at 250MHz). Guarded /0.
  uint64_t opaque_fill_ns_sum = 0;
  for (uint32_t c = 0; c < PROF_NUM_CORES; ++c) {
    opaque_fill_ns_sum += g_acc[c].a[PROF_OPAQUE_FILL].incl_ns;
  }
  uint64_t const opaque_ns_per_px =
      (g_acc_opaque_px != 0) ? opaque_fill_ns_sum / g_acc_opaque_px : 0;
  uint64_t const opaque_px_per_frame =
      (g_acc_frames != 0) ? g_acc_opaque_px / g_acc_frames : 0;
  // XLU ns/px: the (now COARSE, wrap-free) sweep_xlu inclusive over its inside-
  // pixel count. Coarse sweep_xlu includes the per-tile sort + tri_setup, but
  // those are tiny vs fill, so this approximates XLU per-pixel cost — directly
  // comparable to opaque_ns_per_px (same inside-pixel denominator).
  uint64_t xlu_ns_sum = 0;
  for (uint32_t c = 0; c < PROF_NUM_CORES; ++c) {
    xlu_ns_sum += g_acc[c].a[PROF_SWEEP_XLU].incl_ns;
  }
  uint64_t const xlu_ns_per_px =
      (g_acc_xlu_px != 0) ? xlu_ns_sum / g_acc_xlu_px : 0;
  uint64_t const xlu_px_per_frame =
      (g_acc_frames != 0) ? g_acc_xlu_px / g_acc_frames : 0;
  // Overdraw probe: Q3 sizing (opaque z-rejects/frame) + L6 gate (XLU mean
  // overdraw = frags/distinct, x100 for 2 decimals; depth>=2 centi-pct). /0
  // guarded.
  uint64_t const opaque_zrej_per_frame =
      (g_acc_frames != 0) ? g_acc_opaque_zrej / g_acc_frames : 0;
  uint64_t const xlu_frags_per_frame =
      (g_acc_frames != 0) ? g_acc_xlu_frags / g_acc_frames : 0;
  uint64_t const xlu_distinct_per_frame =
      (g_acc_frames != 0) ? g_acc_xlu_distinct / g_acc_frames : 0;
  uint64_t const xlu_overdraw_x100 =
      (g_acc_xlu_distinct != 0) ? g_acc_xlu_frags * 100ULL / g_acc_xlu_distinct
                                : 0;
  uint64_t const xlu_ge2_pct_x100 =
      (g_acc_xlu_distinct != 0) ? g_acc_xlu_ge2 * 10000ULL / g_acc_xlu_distinct
                                : 0;

  printf("prof_begin tag=%s frames=%u cores=%u root_us=%u\n", tag,
         (unsigned)g_acc_frames, (unsigned)PROF_NUM_CORES,
         (unsigned)(root_ns / 1000ULL));

  prof_emit_stage(PROF_CMDGEN, root_ns);
  prof_emit_stage(PROF_GEOM, root_ns);
  prof_emit_stage(PROF_CLEAR_BLIT, root_ns);
  prof_emit_stage(PROF_RASTER_DISPATCH, root_ns);
  prof_emit_stage(PROF_PRESENT, root_ns);

  prof_emit_back(PROF_RASTER_TILE, disp_denom_ns);
  prof_emit_back(PROF_SWEEP_OPAQUE, disp_denom_ns);
  // Opaque sweep broken down: setup (per-tri tri_setup) vs fill (per-tri
  // raster_one). pct_disp here is vs cores x dispatch, same denom as the parent
  // sweep_opaque, so setup+fill ~ sweep_opaque's pct (minus loop glue).
  prof_emit_back(PROF_OPAQUE_SETUP, disp_denom_ns);
  prof_emit_back(PROF_OPAQUE_FILL, disp_denom_ns);
  prof_emit_back(PROF_SWEEP_XLU, disp_denom_ns);
  prof_emit_back(PROF_AA_RESOLVE, disp_denom_ns);
  // Derived opaque-fill intensity: pixels/frame and ns per filled pixel.
  printf("prof opaque   px_per_frame=%u ns_per_px=%u (cyc/px@250MHz=%u.%02u)\n",
         (unsigned)opaque_px_per_frame, (unsigned)opaque_ns_per_px,
         (unsigned)(opaque_ns_per_px / 4ULL),
         (unsigned)((opaque_ns_per_px % 4ULL) * 25ULL));
  printf("prof xlu      px_per_frame=%u ns_per_px=%u (cyc/px@250MHz=%u.%02u)\n",
         (unsigned)xlu_px_per_frame, (unsigned)xlu_ns_per_px,
         (unsigned)(xlu_ns_per_px / 4ULL),
         (unsigned)((xlu_ns_per_px % 4ULL) * 25ULL));
  // Overdraw: XLU mean depth + depth>=2 fraction (L6 gate) and opaque z-rejects
  // (Q3 sizing). mean>>1.00 => L6 has killable overdraw; ~1.00 => ship L5/cap
  // only.
  printf(
      "prof overdraw xlu_mean=%u.%02u xlu_ge2_pct=%u.%02u xlu_frags/f=%u "
      "opaque_zrej/f=%u\n",
      (unsigned)(xlu_overdraw_x100 / 100ULL),
      (unsigned)(xlu_overdraw_x100 % 100ULL),
      (unsigned)(xlu_ge2_pct_x100 / 100ULL),
      (unsigned)(xlu_ge2_pct_x100 % 100ULL), (unsigned)xlu_frags_per_frame,
      (unsigned)opaque_zrej_per_frame);

  // Flat parser scalars (--check). Bare key=int; no inline comments.
  printf("prof_frames=%u\n", (unsigned)g_acc_frames);
  printf("prof_root_us=%u\n", (unsigned)(root_ns / 1000ULL));
  printf("prof_cmdgen_us=%u\n",
         (unsigned)(prof_avg_stage_incl_ns(PROF_CMDGEN) / 1000ULL));
  printf("prof_geom_us=%u\n",
         (unsigned)(prof_avg_stage_incl_ns(PROF_GEOM) / 1000ULL));
  printf("prof_clear_blit_us=%u\n",
         (unsigned)(prof_avg_stage_incl_ns(PROF_CLEAR_BLIT) / 1000ULL));
  printf("prof_raster_dispatch_us=%u\n", (unsigned)(disp_ns / 1000ULL));
  printf("prof_present_us=%u\n",
         (unsigned)(prof_avg_stage_incl_ns(PROF_PRESENT) / 1000ULL));
  printf("prof_core0_idle_us=%u\n", (unsigned)(idle_ns / 1000ULL));
  printf("prof_raster_tile_core_us=%u\n",
         (unsigned)(prof_avg_back_incl_ns(PROF_RASTER_TILE) / 1000ULL));
  printf("prof_sweep_opaque_core_us=%u\n",
         (unsigned)(prof_avg_back_incl_ns(PROF_SWEEP_OPAQUE) / 1000ULL));
  printf("prof_sweep_xlu_core_us=%u\n",
         (unsigned)(prof_avg_back_incl_ns(PROF_SWEEP_XLU) / 1000ULL));
  printf("prof_aa_resolve_core_us=%u\n",
         (unsigned)(prof_avg_back_incl_ns(PROF_AA_RESOLVE) / 1000ULL));
  printf("prof_opaque_setup_core_us=%u\n",
         (unsigned)(prof_avg_back_incl_ns(PROF_OPAQUE_SETUP) / 1000ULL));
  printf("prof_opaque_fill_core_us=%u\n",
         (unsigned)(prof_avg_back_incl_ns(PROF_OPAQUE_FILL) / 1000ULL));
  printf("prof_opaque_px_per_frame=%u\n", (unsigned)opaque_px_per_frame);
  printf("prof_opaque_ns_per_px=%u\n", (unsigned)opaque_ns_per_px);
  printf("prof_xlu_px_per_frame=%u\n", (unsigned)xlu_px_per_frame);
  printf("prof_xlu_ns_per_px=%u\n", (unsigned)xlu_ns_per_px);
  printf("prof_opaque_zrej_per_frame=%u\n", (unsigned)opaque_zrej_per_frame);
  printf("prof_xlu_frags_per_frame=%u\n", (unsigned)xlu_frags_per_frame);
  printf("prof_xlu_distinct_per_frame=%u\n", (unsigned)xlu_distinct_per_frame);
  printf("prof_xlu_overdraw_mean_x100=%u\n", (unsigned)xlu_overdraw_x100);
  printf("prof_xlu_overdraw_ge2_pct_x100=%u\n", (unsigned)xlu_ge2_pct_x100);
  printf("prof_opaque_setup_wrap=%u\n",
         (unsigned)prof_back_wrapped(PROF_OPAQUE_SETUP));
  printf("prof_opaque_fill_wrap=%u\n",
         (unsigned)prof_back_wrapped(PROF_OPAQUE_FILL));
  printf("prof_wrap=%u\n", (unsigned)any_wrap);
  printf("prof_end\n");
}

void prof_frame_end(uint32_t frame, uint32_t frame_ms) {
  (void)frame_ms;  // available for an external root_us vs frame_ms cross-check
  if (frame < PROF_WARMUP) {
    return;  // discard cold warm-up frames (first-touch flash / cold XIP)
  }
  // Fold this frame's per-core anchors into the cumulative run accumulator.
  for (uint32_t c = 0; c < PROF_NUM_CORES; ++c) {
    for (int i = 0; i < PROF_ANCHOR_COUNT; ++i) {
      g_acc[c].a[i].excl_ns += g_prof[c].a[i].excl_ns;
      g_acc[c].a[i].incl_ns += g_prof[c].a[i].incl_ns;
      g_acc[c].a[i].hits += g_prof[c].a[i].hits;
      g_acc[c].a[i].wrapped |= g_prof[c].a[i].wrapped;
    }
  }
  for (uint32_t c = 0; c < PROF_NUM_CORES; ++c) {
    g_acc_opaque_px += g_prof_opaque_px[c];
    g_acc_xlu_px += g_prof_xlu_px[c];
    g_acc_opaque_zrej += g_prof_opaque_zrej[c];
    g_acc_xlu_frags += g_prof_xlu_frags[c];
    g_acc_xlu_distinct += g_prof_xlu_distinct[c];
    g_acc_xlu_ge2 += g_prof_xlu_ge2[c];
  }
  ++g_acc_frames;
  // Periodic snapshot (outside any PROF_ROOT scope — this runs after the frame=
  // log, so the multi-line USB burst does NOT inflate the measured window).
  if (PROF_DUMP_EVERY != 0U && (g_acc_frames % PROF_DUMP_EVERY) == 0U) {
    prof_emit("snapshot");
  }
}

void prof_report_final(void) { prof_emit("final"); }

#endif  // PROFILER
