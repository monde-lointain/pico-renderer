#include "arena/arena.h"

void arena_init(struct Arena* a, void* buf, uint32_t cap) { a->base=(uint8_t*)buf; a->used=0; a->cap=cap; }
void* arena_alloc(struct Arena* a, uint32_t n) { (void)n; return a->base; }
void arena_reset(struct Arena* a) { a->used=0; }
