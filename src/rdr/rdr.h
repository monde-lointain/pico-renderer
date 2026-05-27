// rdr.h — top-level renderer façade (Stream A contract). Orthodox C++.
#ifndef RDR_RDR_H
#define RDR_RDR_H
#include "rdr/types.h"

RdrErr rdr_begin_frame(struct Frame* f);
RdrErr rdr_submit(struct Frame* f, const struct Command* cmds);
RdrErr rdr_end_frame(struct Frame* f);

#endif  // RDR_RDR_H
