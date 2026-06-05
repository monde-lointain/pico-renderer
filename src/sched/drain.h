// drain.h — shared tile-drain body (C2). Orthodox C++.
//
// The SINGLE body every dispatch path runs to rasterize one claimed tile into a
// given worker's private depth scratch. Host-sim (tests/sched), the serial host
// impl (dispatch_host.cc), and the dual-core impl (dispatch_pico.cc) all funnel
// through this so the bit-identical invariant (dispatch.h) has exactly one
// rasterize call site — there is no second, divergent copy to drift.
//
// `worker` selects the private per-worker depth scratch f->zbuf[worker] (see
// rdr/frame.h); `tile` is the linear tile index in [0, GEOM_NUM_TILES). The
// caller owns tile claiming (a shared counter, serial loop, etc.); this helper
// is claim-policy-agnostic.
#ifndef RDR_SCHED_DRAIN_H
#define RDR_SCHED_DRAIN_H

#include "raster/raster.h"
#include "rdr/frame.h"

// L6: this TU sees BOTH raster.h and frame.h, so it is the natural place to pin
// raster.cc's locally-defined RASTER_NUM_WORKERS (which sizes the per-worker
// XLU accumulator slices) in lockstep with frame.h's RDR_NUM_RASTER_WORKERS
// (which sizes zbuf/cov and bounds the `worker` id threaded below). raster.cc
// cannot include frame.h without a layering inversion, so the assert lives
// here.
static_assert(
    RASTER_NUM_WORKERS == RDR_NUM_RASTER_WORKERS,
    "raster XLU accumulator slice count must match the frame.h raster "
    "worker fan-out (zbuf/cov) — the threaded worker id indexes both");

static inline void sched_drain_tile(struct Frame* f, int worker, int tile) {
  // R.3-AA coordinated 1-line seam: pass the worker's private coverage scratch
  // (f->cov[worker], same per-worker independence as zbuf). raster_tile gates
  // on aa_enabled() internally, so when AA is OFF (default) this is byte-
  // identical to the pre-AA drain — the coverage path is fully skipped and the
  // resolve does not run. L6: `worker` is also threaded to raster_tile so the
  // per-worker XLU accumulator slice is selected the SAME way as zbuf/cov (no
  // separate get_core_num() path), so the host 2-worker sim exercises BOTH
  // accumulator slices, not just slice 0.
  raster_tile(tile, &f->geom.tiles[tile], f->geom.tverts, f->fb,
              f->zbuf[worker], &f->rstate_table[0], f->cov[worker], worker);
}

#endif  // RDR_SCHED_DRAIN_H
