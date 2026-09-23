#pragma once

// A tiny deterministic generator for seeded randomized tests.
//
// The generator is deliberately simple and fully specified: a failing case can be
// reproduced from the seed printed by the test.

#include <cstdint>
#include <string>

namespace dpu::fabric::test {

class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed == 0 ? 0x9E3779B97F4A7C15ull : seed) {}

  [[nodiscard]] std::uint64_t next() {
    // xorshift64*: reproducible on every platform, no library dependence.
    state_ ^= state_ >> 12;
    state_ ^= state_ << 25;
    state_ ^= state_ >> 27;
    return state_ * 0x2545F4914F6CDD1Dull;
  }

  /// Uniform value in [0, bound).
  [[nodiscard]] std::uint32_t below(std::uint32_t bound) {
    if (bound == 0) return 0;
    return static_cast<std::uint32_t>(next() % bound);
  }

  [[nodiscard]] bool chance(std::uint32_t percent) { return below(100) < percent; }

  [[nodiscard]] std::string label() const { return std::to_string(seed_); }

 private:
  std::uint64_t state_{0};
  std::uint64_t seed_{state_};
};

}  // namespace dpu::fabric::test
