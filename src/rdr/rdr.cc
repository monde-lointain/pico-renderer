// rdr.cc — top-level renderer façade (C1 single-core wiring). Orthodox C++.
//
// Frame lifecycle: begin_frame resets the per-frame arena + sinks and clears
// the framebuffer; submit stashes the command stream and runs the geometry
// front end (sched_geom); end_frame rasterizes every tile (sched_rasterize).
// rdr stays PLATFORM-FREE — it never calls plat_*; the demo/app owns present so
// host tests link without a platform. See rdr/frame.h for the Frame layout.
#include "rdr/rdr.h"

#include <string.h>

#include "rdr/frame.h"
#include "sched/sched.h"

// Default render state for a fresh frame: opaque, back-face cull, pre-lit RGBA
// (thin slice). Cleared to zero then the few non-zero defaults set explicitly.
static void rstate_defaults(struct RenderState* rs) {
  memset(rs, 0, sizeof *rs);
  rs->zmode = ZMODE_OPAQUE;
  rs->cull = CULL_BACK;
  rs->lit = 0;
}

RdrErr rdr_begin_frame(struct Frame* f) {
  if (f == 0 || f->fb == 0) {
    return RDR_EINVAL;
  }
  arena_init(&f->arena, f->arena_buf, (uint32_t)sizeof f->arena_buf);
  arena_reset(&f->arena);

  // Bind the geometry sinks to the Frame's pool + arena-backed bins (shared ref
  // pool + deferred-job buffer; geom_bin_finalize packs the per-tile segments).
  geom_out_init(&f->geom, f->pool, RDR_MAX_TVERTS, f->bin_pool,
                RDR_BIN_POOL_REFS, f->bin_jobs, RDR_BIN_MAX_JOBS);

  mtx_init(&f->mtx);
  vp_from_cmd(&f->vp, 0, 0, f->width, f->height);
  rstate_defaults(&f->rstate);

  f->cmds = 0;
  f->clear_color = 0;  // black
  f->clear_pending = 0;
  return RDR_OK;
}

RdrErr rdr_submit(struct Frame* f, const struct Command* cmds) {
  if (f == 0 || cmds == 0) {
    return RDR_EINVAL;
  }
  f->cmds = cmds;
  return sched_geom(f);
}

RdrErr rdr_end_frame(struct Frame* f) {
  if (f == 0 || f->fb == 0) {
    return RDR_EINVAL;
  }
  // Clear the framebuffer to the recorded CLEAR color (or black). Done here, at
  // end_frame, so the geometry pass (which records CLEAR) runs first; the clear
  // precedes rasterization of the opaque scene over it.
  int const px = f->width * f->height;
  uint16_t const c = f->clear_color;
  for (int i = 0; i < px; ++i) {
    f->fb[i] = c;
  }
  return sched_rasterize(f);
}
