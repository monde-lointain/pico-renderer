// rdr/frame.h — INTERNAL renderer frame definition (C1 integration).
//
// `struct Frame` is forward-declared in the FROZEN rdr/types.h and
// intentionally left undefined there ("defined where the renderer owns it"). C1
// owns the concrete layout: it bundles the per-frame backing memory the
// single-core pipeline fills (arena + transformed-vertex pool + per-tile TriRef
// segments + GeomOut + matrix/viewport/render state + a one-tile depth
// scratch). This header is shared by the renderer façade (rdr.cc), the geometry
// front end (geom.cc), and the single-core driver (sched.cc) so the layout
// lives in ONE place; it is NOT part of the cross-module frozen contract (not
// in types.h).
//
// Orthodox C++: POD struct, free module_verb() functions over it, C headers.
#ifndef RDR_FRAME_H
#define RDR_FRAME_H

#include <stdint.h>

#include "arena/arena.h"
#include "geom/geom.h"
#include "rdr/config.h"
#include "rdr/types.h"

// Per-tile TriRef segment capacity (thin-slice C1 value). The bin is a fixed
// per-tile segment; overflow drops-with-count (never corrupts) per the binning
// contract. Sized small for the bring-up demo's tri budget; the real ~3000-tri
// tune lands with the S0.5 on-device confirmation (config.h note). Total bin
// storage = GEOM_NUM_TILES * RDR_REFS_PER_TILE * sizeof(TriRef).
#define RDR_REFS_PER_TILE 256

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

  // Command stream submitted this frame (stashed by rdr_submit so the frozen
  // sched_geom(Frame*) driver — which takes no stream argument — can reach it).
  const struct Command* cmds;

  // Clear color recorded by CMD_CLEAR (RGB565); begin_frame seeds the default.
  uint16_t clear_color;
  uint8_t clear_pending;  // nonzero if a CLEAR was recorded this frame

  // One-tile depth scratch reused across tiles by the rasterize driver.
  uint16_t zbuf[RDR_TILE_W * RDR_TILE_H];
};

#endif  // RDR_FRAME_H
