#include "cmd/cmd.h"

void cb_reset(struct CmdBuf* cb) {
  cb->count = 0;
  cb->dropped = 0;
}

RdrErr cb_push(struct CmdBuf* cb, const struct Command* c) {
  (void)cb;
  (void)c;
  return RDR_OK;
}

RdrErr cb_walk(const struct Command* start, CmdVisitFn visit, void* ctx) {
  (void)start;
  (void)visit;
  (void)ctx;
  return RDR_OK;
}
