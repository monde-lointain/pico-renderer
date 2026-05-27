// dispatch.h — link-time-substituted tile-rasterization primitive (C2).
//
// `sched_rasterize` (the frozen seam in sched.h) keeps its signature; its tile
// loop is extracted HERE so the distribution policy can be swapped at link
// time: dispatch_host.cc (serial, one worker, used by host + every test) vs
// dispatch_pico.cc (dual-core, used by firmware only).
//
// BIT-IDENTICAL INVARIANT (the core C2 contract):
//   raster_tile is already per-tile-independent — each tile writes a DISJOINT
//   framebuffer pixel-rect and uses a PRIVATE per-worker depth scratch
//   (f->zbuf[worker_id], see rdr/frame.h). Therefore the framebuffer produced
//   by sched_dispatch_tiles MUST be bit-identical to the single-core serial
//   sweep, for ANY tile-claim order across ANY number of workers. The host
//   determinism test (tests/sched) pins this: serial-vs-2-worker CRC32 must
//   match, and a (broken) shared-zbuf variant must differ — proving the
//   per-worker scratch isolation is what makes the parallelism correct.
//
// Orthodox C++: free module_verb(Receiver*) function, C headers.
#ifndef RDR_SCHED_DISPATCH_H
#define RDR_SCHED_DISPATCH_H

#include "rdr/types.h"

// Rasterize all GEOM_NUM_TILES of `f`, distributed across the implementation's
// workers, JOINING before return. Each worker rasterizes a claimed tile into
// f->zbuf[worker_id] (its private depth scratch) and the shared framebuffer
// f->fb (disjoint pixel-rects, no inter-tile overlap). Precondition: f and
// f->fb are non-null (the seam validated them).
void sched_dispatch_tiles(struct Frame* f);

#endif  // RDR_SCHED_DISPATCH_H
