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

static inline void sched_drain_tile(struct Frame* f, int worker, int tile) {
  // R.3-AA coordinated 1-line seam: pass the worker's private coverage scratch
  // (f->cov[worker], same per-worker independence as zbuf). raster_tile gates
  // on aa_enabled() internally, so when AA is OFF (default) this is byte-
  // identical to the pre-AA drain — the coverage path is fully skipped and the
  // resolve does not run.
  raster_tile(tile, &f->geom.tiles[tile], f->geom.tverts, f->fb,
              f->zbuf[worker], &f->rstate_table[0], f->cov[worker]);
}

#endif  // RDR_SCHED_DRAIN_H
