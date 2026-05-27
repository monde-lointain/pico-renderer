// tex.h — module interface (Stream A contract). Orthodox C++.
#ifndef RDR_TEX_H
#define RDR_TEX_H
#include "rdr/types.h"

uint16_t tex_sample(const struct TexDesc* t, fx16_16 u, fx16_16 v, int lod);

#endif  // RDR_TEX_H
