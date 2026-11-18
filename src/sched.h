// Small PRNG and scheduling helpers for timed tools. Pure: no Arduino headers.
#pragma once

#include <stdint.h>

#include "event.h"

namespace mp {

// xorshift32. Seeded once at boot from the hardware RNG.
struct Rng {
  uint32_t s = 0x9E3779B9u;
  void seed(uint32_t v) { s = v ? v : 0x9E3779B9u; }
  uint32_t next() {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
  }
  // Uniform in [lo, hi], both inclusive.
  uint32_t between(uint32_t lo, uint32_t hi) {
    if (hi <= lo) return lo;
    return lo + next() % (hi - lo + 1);
  }
};

}  // namespace mp
