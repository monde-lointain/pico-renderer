// sched.cc — single-core frame driver (C1). Orthodox C++.
//
// C1 scope: sched is the SEAM between the renderer façade and the geom/raster
// stages, implemented single-core. sched_geom runs the geometry front end over
// the whole command stream; sched_rasterize sweeps every tile, rasterizing its
// bin into the framebuffer with one reused per-tile depth scratch. C2 swaps
// these single-core bodies for the dual-core queue/bin-merge/watermark sched
// WITHOUT changing this interface (the seam stays put).
#include "sched/sched.h"

#include "geom/geom.h"
#include "raster/raster.h"
#include "rdr/frame.h"

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
  for (int tile = 0; tile < GEOM_NUM_TILES; ++tile) {
    raster_tile(tile, &f->geom.tiles[tile], f->geom.tverts, f->fb, f->zbuf);
  }
  return RDR_OK;
}
