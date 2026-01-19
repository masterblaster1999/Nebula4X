#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace nebula4x::util {

// splitmix64: fast deterministic mixing / RNG step.
//
// This is a tiny, high-quality 64-bit mixer by Sebastiano Vigna. It is widely
// used for seeding larger generators and for deterministic procedural noise.
//
// IMPORTANT: This is *not* a cryptographically secure RNG.
inline std::uint64_t splitmix64(std::uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

// Convert a 64-bit word into a double in [0,1) using the top 53 bits
// (IEEE-754 double precision mantissa).
inline double u01_from_u64(std::uint64_t x) {
  const std::uint64_t v = x >> 11; // keep top 53 bits
  return static_cast<double>(v) * (1.0 / 9007199254740992.0); // 2^53
}

// Step a splitmix64 state and return the new state.
inline std::uint64_t next_splitmix64(std::uint64_t& state) {
  state = splitmix64(state);
  return state;
}

// Unbiased bounded random integer in [0, bound_exclusive).
//
// Uses rejection sampling to avoid modulo bias.
inline std::uint64_t bounded_u64(std::uint64_t& state, std::uint64_t bound_exclusive) {
  if (bound_exclusive <= 1) return 0;
  // Largest multiple of bound that fits in 2^64, expressed as a threshold.
  const std::uint64_t threshold = (std::uint64_t(0) - bound_exclusive) % bound_exclusive;
  for (;;) {
    const std::uint64_t r = next_splitmix64(state);
    if (r >= threshold) return r % bound_exclusive;
  }
}

struct HashRng {
  std::uint64_t s{0};

  explicit HashRng(std::uint64_t seed) : s(seed) {}

  std::uint64_t next_u64() { return next_splitmix64(s); }

  double next_u01() { return u01_from_u64(next_u64()); }

  // Inclusive integer range.
  int range_int(int lo_incl, int hi_incl) {
    int lo = lo_incl;
    int hi = hi_incl;
    if (hi < lo) std::swap(lo, hi);
    const std::uint64_t span = static_cast<std::uint64_t>(hi - lo) + 1ULL;
    return lo + static_cast<int>(bounded_u64(s, span));
  }

  // Inclusive size_t index.
  std::size_t index(std::size_t n) {
    if (n <= 1) return 0;
    return static_cast<std::size_t>(bounded_u64(s, static_cast<std::uint64_t>(n)));
  }

  double range(double lo_incl, double hi_incl) {
    double lo = lo_incl;
    double hi = hi_incl;
    if (hi < lo) std::swap(lo, hi);
    return lo + (hi - lo) * next_u01();
  }

  // Back-compat alias: some call sites use range_real().
  double range_real(double lo_incl, double hi_incl) { return range(lo_incl, hi_incl); }
};

} // namespace nebula4x::util
