#include "arena/arena.h"

#include <stdint.h>

#include "gtest/gtest.h"

// Backing buffer aligned for any POD the renderer allocates.
static uint8_t g_buf[1024] __attribute__((aligned(8)));

TEST(Arena, InitZeroesUsedAndStoresCap) {
  struct Arena a;
  arena_init(&a, g_buf, sizeof g_buf);
  EXPECT_EQ(a.base, g_buf);
  EXPECT_EQ(a.used, 0u);
  EXPECT_EQ(a.cap, (uint32_t)sizeof g_buf);
}

TEST(Arena, AllocReturnsBaseAndBumpsUsed) {
  struct Arena a;
  arena_init(&a, g_buf, sizeof g_buf);
  void* p = arena_alloc(&a, 16);
  EXPECT_EQ(p, g_buf);
  EXPECT_EQ(a.used, 16u);
}

TEST(Arena, AllocIsSequential) {
  struct Arena a;
  arena_init(&a, g_buf, sizeof g_buf);
  void* p0 = arena_alloc(&a, 16);
  void* p1 = arena_alloc(&a, 16);
  EXPECT_EQ((uint8_t*)p1 - (uint8_t*)p0, 16);
  EXPECT_EQ(a.used, 32u);
}

TEST(Arena, AllocAlignsToEightBytes) {
  struct Arena a;
  arena_init(&a, g_buf, sizeof g_buf);
  (void)arena_alloc(&a, 1);      // odd size leaves used unaligned-if-naive
  void* p = arena_alloc(&a, 8);  // must be 8-byte aligned
  EXPECT_EQ(((uintptr_t)p) & 7u, 0u);
  EXPECT_EQ(a.used, 16u);  // 1 rounded up to 8, then +8
}

TEST(Arena, AllocPastCapReturnsNullAndDoesNotBump) {
  struct Arena a;
  arena_init(&a, g_buf, 32);
  (void)arena_alloc(&a, 24);
  uint32_t before = a.used;
  void* p = arena_alloc(&a, 16);  // align(24)=24, +16 = 40 > 32
  EXPECT_EQ(p, (void*)0);
  EXPECT_EQ(a.used, before);  // failed alloc must not corrupt the arena
}

TEST(Arena, AllocExactlyFillsToCap) {
  struct Arena a;
  arena_init(&a, g_buf, 32);
  void* p = arena_alloc(&a, 32);
  EXPECT_EQ(p, g_buf);
  EXPECT_EQ(a.used, 32u);
  EXPECT_EQ(arena_alloc(&a, 1), (void*)0);  // now full
}

TEST(Arena, ResetRewindsOToOneAndReusesSpace) {
  struct Arena a;
  arena_init(&a, g_buf, sizeof g_buf);
  void* p0 = arena_alloc(&a, 64);
  arena_reset(&a);
  EXPECT_EQ(a.used, 0u);
  void* p1 = arena_alloc(&a, 64);
  EXPECT_EQ(p1, p0);  // reset is O(1) rewind; same address reused
}

TEST(Arena, ZeroSizeAllocSucceedsAndDoesNotBump) {
  struct Arena a;
  arena_init(&a, g_buf, sizeof g_buf);
  void* p = arena_alloc(&a, 0);
  EXPECT_EQ(p, g_buf);
  EXPECT_EQ(a.used, 0u);
}
