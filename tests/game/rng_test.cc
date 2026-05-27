#include "game/rng.h"

#include <gtest/gtest.h>
#include <stdint.h>

TEST(Rng, XorshiftNonZero) {
  Rng r;
  rng_seed(&r, 42u);
  uint32_t v = rng_next(&r);
  EXPECT_NE(v, 0u);
}

TEST(Rng, SeedZeroIsForcedNonZero) {
  Rng r;
  rng_seed(&r, 0u);
  /* xorshift32 would be stuck at 0 forever if seeded with 0. */
  EXPECT_NE(rng_next(&r), 0u);
}

TEST(Rng, XorshiftDeterministicFromSeed) {
  Rng a, b;
  rng_seed(&a, 12345u);
  rng_seed(&b, 12345u);
  for (int i = 0; i < 100; ++i) {
    EXPECT_EQ(rng_next(&a), rng_next(&b));
  }
}

TEST(Rng, DifferentSeedsProduceDifferentSequences) {
  Rng a, b;
  rng_seed(&a, 1u);
  rng_seed(&b, 2u);
  /* Very likely to differ in the first 10 outputs */
  bool any_diff = false;
  for (int i = 0; i < 10; ++i) {
    if (rng_next(&a) != rng_next(&b)) {
      any_diff = true;
      break;
    }
  }
  EXPECT_TRUE(any_diff);
}
