#ifndef GAME_RNG_H
#define GAME_RNG_H

#include <stdint.h>

/* Seedable xorshift32 RNG. */

typedef struct {
  uint32_t state;
} Rng;

/* Seed the RNG. seed must be nonzero; if 0, forced to 1. */
void rng_seed(Rng* r, uint32_t seed);

/* Produce next xorshift32 value. */
uint32_t rng_next(Rng* r);

#endif /* GAME_RNG_H */
