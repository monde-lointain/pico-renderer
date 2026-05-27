// arena.h — module interface (Stream A contract). Orthodox C++.
#ifndef RDR_ARENA_H
#define RDR_ARENA_H
#include "rdr/types.h"

struct Arena {
  uint8_t* base;
  uint32_t used;
  uint32_t cap;
};
void arena_init(struct Arena* a, void* buf, uint32_t cap);
void* arena_alloc(struct Arena* a, uint32_t n);
void arena_reset(struct Arena* a);

#endif  // RDR_ARENA_H
