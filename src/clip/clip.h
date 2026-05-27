// clip.h — module interface (Stream A contract). Orthodox C++.
#ifndef RDR_CLIP_H
#define RDR_CLIP_H
#include "rdr/types.h"

int clip_tri(const struct TVtx* in, int n, struct TVtx* out);

#endif  // RDR_CLIP_H
