#include "game/rng.h"

#include <stdint.h>

void rng_seed(Rng* r, uint32_t seed) { r->state = (seed == 0U) ? 1U : seed; }

uint32_t rng_next(Rng* r) {
  uint32_t x = r->state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  r->state = x;
  return x;
}
