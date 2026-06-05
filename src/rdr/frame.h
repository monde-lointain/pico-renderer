// rdr/frame.h — INTERNAL renderer frame definition (C1 integration).
//
// `struct Frame` is forward-declared in the FROZEN rdr/types.h and
// intentionally left undefined there ("defined where the renderer owns it"). C1
// owns the concrete layout: it bundles the per-frame backing memory the
// single-core pipeline fills (arena + transformed-vertex pool + per-tile TriRef
// segments + GeomOut + matrix/viewport/render state + per-worker one-tile depth
// + coverage scratch + the 2D sky-blit list). This header is shared by the
// renderer façade (rdr.cc), the geometry
// front end (geom.cc), and the single-core driver (sched.cc) so the layout
// lives in ONE place; it is NOT part of the cross-module frozen contract (not
// in types.h).
//
// Orthodox C++: POD struct, free module_verb() functions over it, C headers.
#ifndef RDR_FRAME_H
#define RDR_FRAME_H

#include <stdint.h>

// C2: number of back-end raster workers. The dual-core dispatch
// (src/sched/dispatch_pico.cc) runs the tile sweep across both RP2040 cores;
// each core gets its OWN per-tile depth scratch (zbuf[core_id]) so tiles stay
// independent and the output is bit-identical to the single-core serial sweep.
// The host/serial dispatch uses only worker 0. Front-end geometry stays
// single-core; this is a back-end-raster-only fan-out.
#define RDR_NUM_RASTER_WORKERS 2

#include "arena/arena.h"
#include "blit2d/blit2d.h"
#include "geom/geom.h"
#include "rdr/config.h"
#include "rdr/types.h"

// Arena-backed variable bins (Wave-E, T0 finding). The OLD model pre-sliced one
// fixed RDR_REFS_PER_TILE segment per tile — but real-density terrain packs
// >256 tris into a hot 60x60 tile (measured worst single-tile demand = 759), so
// fixed segments dropped real geometry every frame, and sizing them up (759*16
// tiles = ~96-128 KB) blew the SRAM budget. Instead the bins draw from ONE
// shared pool, sized to the worst-frame TOTAL tile-ref demand (measured max =
// 1606 over the full scripted loop), so a hot tile takes what cold tiles don't.
// Binning DEFERS: geom_bin_tri buffers each surviving tri (a TriRef) into
// bin_jobs[] and tallies per-tile demand; geom_bin_finalize prefix-sums the
// demands into contiguous per-tile segments of bin_pool[] and replays the jobs
// IN EMISSION ORDER — so the per-tile ref order (hence the rasterized output)
// is identical to the old append order: the dual-core bit-identical invariant
// holds. Pool/job overflow drops-with-count (never corrupts), same contract but
// GLOBAL not per-tile. Both are tunables (S0.5/scene-tuned), like
// RDR_MAX_TVERTS.
#define RDR_BIN_POOL_REFS 2048  // shared contiguous per-tile segments (16 KB)
#define RDR_BIN_MAX_JOBS 1536   // deferred surviving-tri list (12 KB)

// Φ (terrain wave): per-frame interned render-state table. TriRef.material
// indexes it (resolves C2 latent #2 — C1/C2 wrote 0). The scene's distinct
// RenderStates are FEW (terrain / 3 tree sprites / cloud / panorama ≈ 6); the
// 128 terrain mesh-cells differ ONLY in their texture pointer, which is a
// SEPARATE per-cell axis (flash-resident TexDescs keyed off the material/tri),
// NOT 128 RenderStates — so 16 is comfortable. Interned single-core in geom
// before the dual-core raster reads it (immutable during raster → bit-identical
// invariant preserved).
#define RDR_MAX_MATERIALS 16

// Φ2 (terrain wave): screen-space 2D background sky blits. rdr owns ORDERING
// via this list on Frame (keeps rdr platform-free; the demo fills it). The
// scene needs panorama + clouds = 2 (run full-frame BEFORE the 3D sweep at T3 —
// no depth, does not seed zbuf); 4 leaves headroom. Layout-coupled (sizes the
// blits[] array) so it lives here beside the struct, not in config.h.
#define RDR_MAX_BLITS 4

struct Frame {
  // Output target (caller-owned full-screen RGB565, row-major).
  uint16_t* fb;
  int width;
  int height;

  // Per-frame scratch arena (8-byte aligned backing; required on target). Used
  // for any transient pipeline allocation; reset each begin_frame.
  struct Arena arena;
  alignas(8) uint8_t arena_buf[RDR_CMD_ARENA_BYTES];

  // Transformed-vertex pool + arena-backed bin storage (the sort-middle).
  // bin_jobs: deferred surviving-tri list (buffered during the geom pass).
  // bin_pool: the shared ref pool finalize packs into contiguous per-tile
  // segments. See RDR_BIN_* above.
  struct TVtx pool[RDR_MAX_TVERTS];
  struct TriRef bin_jobs[RDR_BIN_MAX_JOBS];
  struct TriRef bin_pool[RDR_BIN_POOL_REFS];
  struct GeomOut geom;

  // Front-end state mutated by the command stream.
  struct MtxStack mtx;
  struct Viewport vp;
  struct RenderState rstate;

  // Φ: interned per-frame render states; TriRef.material indexes
  // [0,rstate_count).
  struct RenderState rstate_table[RDR_MAX_MATERIALS];
  uint16_t rstate_count;

  // Command stream submitted this frame (stashed by rdr_submit so the frozen
  // sched_geom(Frame*) driver — which takes no stream argument — can reach it).
  const struct Command* cmds;

  // Clear color recorded by CMD_CLEAR (RGB565); begin_frame seeds the default.
  uint16_t clear_color;
  uint8_t clear_pending;  // nonzero if a CLEAR was recorded this frame

  // Per-worker one-tile depth scratch reused across the tiles a worker claims
  // (raster_tile clears it on entry). One independent scratch per raster worker
  // so dual-core tiles never share depth state — the bit-identical invariant.
  uint16_t zbuf[RDR_NUM_RASTER_WORKERS][RDR_TILE_W * RDR_TILE_H];

  // Φ2: screen-space 2D sky blits drawn full-frame BEFORE the 3D sweep. The
  // demo fills [0,blit_count); rdr owns ordering (T3 wires blit2d_render into
  // rdr.cc — storing Blit2dRect by value here adds only a header-type
  // dependency, no link until then).
  struct Blit2dRect blits[RDR_MAX_BLITS];
  uint16_t blit_count;

  // Φ2: per-worker one-tile coverage scratch for the RDP-adapted AA resolve
  // (R.3-AA). One byte/pixel analytic coverage, one independent scratch per
  // raster worker — the same dual-core independence as zbuf, preserving the
  // bit-identical invariant. Born here; written + consumed by R.3-AA's per-tile
  // resolve (the 565 + per-tile-coverage build, config.h). Untouched until R.3
  // lands — like TVtx.fog, defined where born so there are no stray bytes.
  uint8_t cov[RDR_NUM_RASTER_WORKERS][RDR_TILE_W * RDR_TILE_H];

  // L6 CONTRACT-BARRIER NOTE (flagged for the Lead — Frame layout UNCHANGED):
  // L6 (XLU front-to-back premultiplied UNDER + saturation early-out) needs a
  // per-worker per-tile accumulator pair — premultiplied color (565) + 8-bit
  // accumulated alpha — exactly parallel to the per-worker zbuf/cov above. The
  // spec asked for these as Frame members (C_acc/acc_alpha) plumbed through
  // raster_tile/raster_tile_noclear like zbuf/cov. That plumbing is BLOCKED for
  // the L6 stream: raster_tile's parameter list is Q1-owned (the
  // __not_in_flash_func wrap) and the sole call site (src/sched/drain.h) is not
  // in the L6 lane's grant — adding parameters/members can't be reached. Since
  // the accumulators are PURE within-tile scratch (cleared, filled, consumed
  // entirely inside ONE raster_tile_noclear call; never read across calls), L6
  // stores them as file-scope-static-per-worker arrays in src/raster/raster.cc
  // (g_xlu_c_acc / g_xlu_acc_alpha) — the same encapsulation pattern as the
  // PROFILER overdraw probe there, indexed by the runtime core id, preserving
  // dual==serial identically. SRAM cost is the SAME wherever it lives: C_acc =
  // NW * 3600 * 2 B = 14.4 KB and acc_alpha = NW * 3600 * 1 B = 7.2 KB (NW =
  // RDR_NUM_RASTER_WORKERS, 3600 = RDR_TILE_W*RDR_TILE_H), ~21.6 KB total. 565
  // is mandatory (RGB888 at 28.8 KB won't fit). If the Lead prefers a Frame
  // member, it is a 2-line signature change + the drain.h call site, reconciled
  // at the barrier merge after Q1 lands.
};

#endif  // RDR_FRAME_H
