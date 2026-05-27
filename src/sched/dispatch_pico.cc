// dispatch_pico.cc — dual-core tile-dispatch primitive (FIRMWARE only). C2.
//
// Parallelizes ONLY the back-end raster across both RP2040 cores. The geometry
// front end stays single-core (this primitive is invoked from sched_rasterize,
// after geom). Output is BIT-IDENTICAL to the serial host sweep for any
// tile-claim order (dispatch.h invariant): tiles are per-tile-independent —
// disjoint framebuffer pixel-rects + a PRIVATE per-core depth scratch
// (f->zbuf[core_id]) — so the only shared mutable state is the tile counter.
//
// Protocol per sched_dispatch_tiles call:
//   - core1 is launched ONCE (idempotent guard); thereafter it PARKS popping
//   the
//     inter-core FIFO. multicore_launch_core1 is one-shot, so we reuse the
//     parked core every frame rather than relaunching.
//   - core0 publishes the Frame, resets the shared tile counter to 0, wakes
//     core1 via the FIFO, then both cores DRAIN: each claims the next tile from
//     a spinlock-guarded `s_next_tile` (the RP2040 M0+ has no native atomics,
//     so a pico-sdk hardware spinlock serializes the increment) and rasterizes
//     it into ITS OWN zbuf[core_id]. When the counter is exhausted each core
//     stops; core1 signals done back over the FIFO; core0 JOINS on that signal
//     and returns. core1 re-parks for the next frame.
//
// pico-sdk idioms (multicore / hardware spinlock) are non-Orthodox by nature;
// the surface is kept narrow and lives only in this firmware-only file.
#include <stdint.h>

#include "geom/geom.h"  // GEOM_NUM_TILES
#include "hardware/sync.h"
#include "pico/multicore.h"
#include "rdr/frame.h"
#include "sched/dispatch.h"
#include "sched/drain.h"

// FIFO tokens (arbitrary, distinct).
enum { DISPATCH_TOKEN_GO = 0x60U, DISPATCH_TOKEN_DONE = 0xD1U };

// Shared state published by core0 before waking core1. `s_frame` is read by
// both cores once core1 is woken; `s_next_tile` is the claim counter guarded by
// the claimed hardware spinlock.
static struct Frame* volatile s_frame = 0;
static volatile uint32_t s_next_tile = 0;
static spin_lock_t* s_tile_lock = 0;
static volatile uint32_t s_core1_launched = 0;

// Claim the next unrasterized tile index under the spinlock. Returns the tile
// index, or GEOM_NUM_TILES once the sweep is exhausted. The lock is held only
// for the increment (brief, per the spinlock contract).
static int dispatch_claim_tile(void) {
  uint32_t const save = spin_lock_blocking(s_tile_lock);
  uint32_t const t = s_next_tile;
  if (t < (uint32_t)GEOM_NUM_TILES) {
    s_next_tile = t + 1;
  }
  spin_unlock(s_tile_lock, save);
  return (int)t;
}

// Drain loop run by BOTH cores: claim tiles until exhausted, each rasterizing
// into its own per-core depth scratch (zbuf[core]).
static void dispatch_run(struct Frame* f, int core) {
  for (;;) {
    int const tile = dispatch_claim_tile();
    if (tile >= GEOM_NUM_TILES) {
      break;
    }
    sched_drain_tile(f, core, tile);
  }
}

// core1 entry: park on the FIFO, run a drain pass on each GO token, signal
// DONE, re-park. Runs forever (the firmware never tears core1 down).
static void dispatch_core1_main(void) {
  for (;;) {
    uint32_t const tok = multicore_fifo_pop_blocking();
    if (tok != (uint32_t)DISPATCH_TOKEN_GO) {
      continue;
    }
    dispatch_run(s_frame, 1);
    multicore_fifo_push_blocking((uint32_t)DISPATCH_TOKEN_DONE);
  }
}

void sched_dispatch_tiles(struct Frame* f) {
  // One-time setup: claim a hardware spinlock and launch core1 into its park
  // loop. multicore_launch_core1 is one-shot — guard so re-entry reuses the
  // already-parked core.
  if (s_core1_launched == 0) {
    s_tile_lock = spin_lock_init((uint)spin_lock_claim_unused(true));
    multicore_launch_core1(dispatch_core1_main);
    s_core1_launched = 1;
  }

  // Publish the frame and reset the claim counter BEFORE waking core1.
  s_frame = f;
  s_next_tile = 0;

  // Wake core1, then core0 drains alongside it (core 0 -> zbuf[0]).
  multicore_fifo_push_blocking((uint32_t)DISPATCH_TOKEN_GO);
  dispatch_run(f, 0);

  // JOIN: wait for core1's done signal before returning (framebuffer complete).
  uint32_t tok = multicore_fifo_pop_blocking();
  while (tok != (uint32_t)DISPATCH_TOKEN_DONE) {
    tok = multicore_fifo_pop_blocking();
  }
}
