#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <random>

namespace audiofork {

// Jittered exponential backoff for reconnects (DESIGN.md §7). Jitter spreads a
// reconnect storm — 1,000 forks losing one vendor endpoint must not retry in
// lockstep. Seed is injected so schedules are reproducible in tests.
class ReconnectBackoff {
 public:
  struct Options {
    std::chrono::milliseconds initial{250};
    std::chrono::milliseconds max{5000};
    double multiplier = 2.0;
    double jitter_fraction = 0.25;
  };

  ReconnectBackoff(Options options, std::uint64_t seed) : options_(options), rng_(seed) {
    options_.multiplier = std::max(options_.multiplier, 1.0);
    options_.jitter_fraction = std::clamp(options_.jitter_fraction, 0.0, 0.9);
    options_.initial = std::max(options_.initial, std::chrono::milliseconds{1});
    options_.max = std::max(options_.max, options_.initial);
  }

  [[nodiscard]] std::chrono::milliseconds NextDelay() {
    const double base = std::min(current_, static_cast<double>(options_.max.count()));
    current_ = base * options_.multiplier;
    std::uniform_real_distribution<double> jitter(1.0 - options_.jitter_fraction,
                                                  1.0 + options_.jitter_fraction);
    const double jittered = std::max(base * jitter(rng_), 1.0);
    return std::chrono::milliseconds{static_cast<std::int64_t>(jittered)};
  }

  void Reset() { current_ = static_cast<double>(options_.initial.count()); }

 private:
  Options options_;
  std::mt19937_64 rng_;
  double current_ = static_cast<double>(options_.initial.count());
};

}  // namespace audiofork
