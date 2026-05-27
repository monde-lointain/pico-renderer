#include "arena/arena.h"

// Bump allocator. Allocations are 8-byte aligned (covers every POD the
// renderer hands out: pointers on host, fx*/struct fields on target). Reset is
// an O(1) rewind. Allocation past `cap` fails cleanly (returns NULL) and never
// corrupts the arena — see the renderer design spec (overflow = drop, never
// silent corruption).

enum { ARENA_ALIGN = 8 };

static uint32_t arena_align_up(uint32_t n) {
  return (n + (ARENA_ALIGN - 1)) & ~(uint32_t)(ARENA_ALIGN - 1);
}

void arena_init(struct Arena* a, void* buf, uint32_t cap) {
  a->base = (uint8_t*)buf;
  a->used = 0;
  a->cap = cap;
}

void* arena_alloc(struct Arena* a, uint32_t n) {
  uint32_t base = arena_align_up(a->used);
  // Overflow-safe bound check: never let base + n wrap past cap.
  if (base > a->cap || n > a->cap - base) {
    return (void*)0;
  }
  void* p = a->base + base;
  a->used = base + n;
  return p;
}

void arena_reset(struct Arena* a) { a->used = 0; }
