// cmd.h — module interface (Stream A contract). Orthodox C++.
//
// Tagged command buffer (record), plus a display-list walker (replay) with
// CALL_LIST / RETURN / END / BRANCH_LESS_Z control flow, modelled on the N64
// GBI (gSPDisplayList = push+call, gsSPBranchList = no-push tail-jump,
// gSPEndDisplayList = pop/terminate). Two lifetimes per the design spec:
// transient per-frame buffers and retained display lists replayed via
// CALL_LIST. Overflow policy: drop-with-count, never silently corrupt.
#ifndef RDR_CMD_H
#define RDR_CMD_H
#include "rdr/types.h"

// ---- command buffer (record) ----------------------------------------------
// `buf` is a caller-owned array of `cap` Commands (typically arena-backed).
// On overflow cb_push drops the command and bumps `dropped` (surfaced for
// debug) — it never writes past `cap`.
struct CmdBuf {
  struct Command* buf;
  uint32_t count;    // commands recorded
  uint32_t cap;      // capacity (in Commands)
  uint32_t dropped;  // commands dropped on overflow (drop-with-count)
};
void cb_reset(struct CmdBuf* cb);  // count := 0, dropped := 0 (O(1) rewind)
RdrErr cb_push(struct CmdBuf* cb, const struct Command* c);

// ---- display-list walker (replay) -----------------------------------------
// Visitor invoked once per *non-control* command in stream order. For
// CMD_BRANCH_LESS_Z the visitor is consulted for the branch predicate (it owns
// the transformed-vertex depth): return nonzero to TAKE the branch (no-push
// tail-jump to dl), zero to fall through. For all other commands the return
// value is ignored. `ctx` is opaque caller state (e.g. the geom front end).
typedef int (*CmdVisitFn)(void* ctx, const struct Command* c);

enum { CMD_WALK_MAX_DEPTH = 16 };  // CALL_LIST nesting limit (return stack)

// Walk `start` until an END pops the empty return stack (or a malformed/over-
// deep stream is rejected). Returns:
//   RDR_OK         — walked to completion (END at top level)
//   RDR_EINVAL     — null start/visit, or a control op referenced a null list
//   RDR_EOVERFLOW  — CALL_LIST nesting exceeded CMD_WALK_MAX_DEPTH
RdrErr cb_walk(const struct Command* start, CmdVisitFn visit, void* ctx);

#endif  // RDR_CMD_H
