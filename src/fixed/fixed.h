// fixed.h — module interface (Stream A contract). Orthodox C++.
#ifndef RDR_FIXED_H
#define RDR_FIXED_H
#include "rdr/types.h"

fx16_16 fx_mul(fx16_16 a, fx16_16 b);
fx16_16 fx_div(fx16_16 a, fx16_16 b);
void mat4_identity(struct Mat4fx* m);

#endif  // RDR_FIXED_H
