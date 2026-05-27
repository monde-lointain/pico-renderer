// sched.cc — frame driver. Orthodox C++.
//
// sched is the SEAM between the renderer façade and the geom/raster stages.
// sched_geom runs the geometry front end over the whole command stream
// (SINGLE-CORE — C2 keeps the front end single-core). sched_rasterize sweeps
// every tile via sched_dispatch_tiles (sched/dispatch.h), a link-time-
// substituted primitive: serial on host/tests (dispatch_host.cc), dual-core on
// firmware (dispatch_pico.cc). The seam signatures stay frozen; only the tile
// distribution behind sched_dispatch_tiles changes between targets, and the
// framebuffer is bit-identical either way.
#include "sched/sched.h"

#include "geom/geom.h"
#include "rdr/frame.h"
#include "sched/dispatch.h"

// Geometry pass: interpret the recorded command stream into the Frame's
// transformed-vertex pool + per-tile bins. The stream pointer is threaded in by
// rdr_submit, which stashes it on the Frame for the driver.
RdrErr sched_geom(struct Frame* f) {
  if (f == 0) {
    return RDR_EINVAL;
  }
  if (f->cmds == 0) {
    return RDR_OK;  // nothing submitted (e.g. clear-only frame)
  }
  // Single-core: run the whole stream through the geometry front end. The
  // stream pointer was stashed on the Frame by rdr_submit so this frozen
  // (Frame*)-only entry can reach it. C2 replaces this body with the dual-core
  // queue/bin-merge driver, same interface.
  return geom_run(f->cmds, f);
}

RdrErr sched_rasterize(struct Frame* f) {
  if (f == 0 || f->fb == 0) {
    return RDR_EINVAL;
  }
  // The tile sweep is extracted into a link-time-substituted primitive
  // (sched/dispatch.h): serial on host/tests, dual-core on firmware. Both
  // produce a bit-identical framebuffer (per-tile-independent rasterization).
  sched_dispatch_tiles(f);
  return RDR_OK;
}
