// sched.h — module interface (Stream A contract). Orthodox C++.
#ifndef RDR_SCHED_H
#define RDR_SCHED_H
#include "rdr/types.h"

RdrErr sched_geom(struct Frame* f);
RdrErr sched_rasterize(struct Frame* f);

#endif  // RDR_SCHED_H
