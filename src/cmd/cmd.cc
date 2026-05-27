#include "cmd/cmd.h"

#include <string.h>

// ---- command buffer (record) ----------------------------------------------

void cb_reset(struct CmdBuf* cb) {
  cb->count = 0;
  cb->dropped = 0;
}

RdrErr cb_push(struct CmdBuf* cb, const struct Command* c) {
  if (cb->count >= cb->cap) {
    // Drop-with-count: surface the loss, never write past `cap`.
    cb->dropped++;
    return RDR_EOVERFLOW;
  }
  cb->buf[cb->count] = *c;  // by-value snapshot
  cb->count++;
  return RDR_OK;
}

// ---- display-list walker (replay) -----------------------------------------
// Iterative walk with a bounded return stack (no host recursion → safe on the
// RP2040's small stack and bounded by CMD_WALK_MAX_DEPTH). Control flow follows
// the N64 GBI: CALL_LIST pushes the resume pointer and jumps; RETURN/END pop;
// END at the top level terminates; BRANCH_LESS_Z is a no-push tail-jump taken
// only when the visitor's predicate is true.

RdrErr cb_walk(const struct Command* start, CmdVisitFn visit, void* ctx) {
  if (start == 0 || visit == 0) {
    return RDR_EINVAL;
  }

  const struct Command* ret_stack[CMD_WALK_MAX_DEPTH];
  int sp = 0;  // return-stack depth
  const struct Command* pc = start;

  for (;;) {
    switch (pc->op) {
      case CMD_CALL_LIST: {
        const struct Command* target = pc->u.call_list.ptr;
        if (target == 0) {
          return RDR_EINVAL;
        }
        if (sp >= CMD_WALK_MAX_DEPTH) {
          return RDR_EOVERFLOW;
        }
        ret_stack[sp] = pc + 1;  // resume after the call
        sp++;
        pc = target;
        break;
      }
      case CMD_BRANCH_LESS_Z: {
        const struct Command* target = pc->u.branch_less_z.dl;
        if (target == 0) {
          return RDR_EINVAL;
        }
        // Visitor owns the depth predicate; nonzero => take the no-push jump.
        if (visit(ctx, pc) != 0) {
          pc = target;  // tail-jump, no return pushed
        } else {
          pc++;
        }
        break;
      }
      case CMD_RETURN:
      case CMD_END: {
        if (sp == 0) {
          return RDR_OK;  // END at top level ends the walk
        }
        sp--;
        pc = ret_stack[sp];
        break;
      }
      default: {
        visit(ctx, pc);
        pc++;
        break;
      }
    }
  }
}
