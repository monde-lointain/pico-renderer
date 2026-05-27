// cmd.h — module interface (Stream A contract). Orthodox C++.
#ifndef RDR_CMD_H
#define RDR_CMD_H
#include "rdr/types.h"

struct CmdBuf {
  struct Command* buf;
  uint32_t count;
  uint32_t cap;
};
void cb_reset(struct CmdBuf* cb);
RdrErr cb_push(struct CmdBuf* cb, const struct Command* c);

#endif  // RDR_CMD_H
