// dispatch_host.cc — serial tile-dispatch primitive (host + ALL tests). C2.
//
// The single-worker implementation of sched_dispatch_tiles: sweep every tile in
// order on the calling thread, draining each through the shared body
// (sched/drain.h) into worker 0's depth scratch (f->zbuf[0]). This is
// behavior-preserving vs the pre-C2 single-core loop — every existing test
// stays green. The dual-core variant lives in dispatch_pico.cc (firmware only).
//
// Orthodox C++: free module_verb(Receiver*) function, C headers.
#include "geom/geom.h"  // GEOM_NUM_TILES
#include "rdr/frame.h"
#include "sched/dispatch.h"
#include "sched/drain.h"

void sched_dispatch_tiles(struct Frame* f) {
  for (int tile = 0; tile < GEOM_NUM_TILES; ++tile) {
    sched_drain_tile(f, 0, tile);
  }
}
