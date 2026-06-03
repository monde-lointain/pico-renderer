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

// Per-tile TriRef segment capacity (thin-slice C1 value). The bin is a fixed
// per-tile segment; overflow drops-with-count (never corrupts) per the binning
// contract. Sized small for the bring-up demo's tri budget; the real ~3000-tri
// tune lands with the S0.5 on-device confirmation (config.h note). Total bin
// storage = GEOM_NUM_TILES * RDR_REFS_PER_TILE * sizeof(TriRef).
#define RDR_REFS_PER_TILE 256

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

  // Transformed-vertex pool + per-tile TriRef bin storage (the sort-middle).
  struct TVtx pool[RDR_MAX_TVERTS];
  struct TriRef refs[GEOM_NUM_TILES * RDR_REFS_PER_TILE];
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
};

#endif  // RDR_FRAME_H
