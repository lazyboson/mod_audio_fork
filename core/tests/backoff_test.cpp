#include "audiofork/backoff.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace audiofork {
namespace {

using std::chrono::milliseconds;

constexpr ReconnectBackoff::Options kNoJitter{milliseconds{250}, milliseconds{5000}, 2.0, 0.0};

TEST(ReconnectBackoff, GrowsExponentiallyAndCapsAtMax) {
  ReconnectBackoff backoff(kNoJitter, 1);
  EXPECT_EQ(backoff.NextDelay(), milliseconds{250});
  EXPECT_EQ(backoff.NextDelay(), milliseconds{500});
  EXPECT_EQ(backoff.NextDelay(), milliseconds{1000});
  EXPECT_EQ(backoff.NextDelay(), milliseconds{2000});
  EXPECT_EQ(backoff.NextDelay(), milliseconds{4000});
  EXPECT_EQ(backoff.NextDelay(), milliseconds{5000});
  EXPECT_EQ(backoff.NextDelay(), milliseconds{5000});
}

TEST(ReconnectBackoff, ResetRestartsTheSchedule) {
  ReconnectBackoff backoff(kNoJitter, 1);
  (void)backoff.NextDelay();
  (void)backoff.NextDelay();
  backoff.Reset();
  EXPECT_EQ(backoff.NextDelay(), milliseconds{250});
}

TEST(ReconnectBackoff, JitterStaysWithinFraction) {
  ReconnectBackoff backoff({milliseconds{1000}, milliseconds{1000}, 2.0, 0.25}, 42);
  for (int i = 0; i < 1000; ++i) {
    const auto delay = backoff.NextDelay();
    EXPECT_GE(delay, milliseconds{750});
    EXPECT_LE(delay, milliseconds{1250});
  }
}

TEST(ReconnectBackoff, SameSeedSameSchedule) {
  const ReconnectBackoff::Options options{milliseconds{250}, milliseconds{5000}, 2.0, 0.25};
  ReconnectBackoff first(options, 7);
  ReconnectBackoff second(options, 7);
  for (int i = 0; i < 20; ++i) {
    EXPECT_EQ(first.NextDelay(), second.NextDelay());
  }
}

TEST(ReconnectBackoff, DifferentSeedsDiverge) {
  const ReconnectBackoff::Options options{milliseconds{1000}, milliseconds{5000}, 2.0, 0.25};
  ReconnectBackoff first(options, 1);
  ReconnectBackoff second(options, 2);
  bool diverged = false;
  for (int i = 0; i < 20; ++i) {
    diverged = diverged || (first.NextDelay() != second.NextDelay());
  }
  EXPECT_TRUE(diverged);
}

TEST(ReconnectBackoff, DegenerateOptionsAreClampedSafely) {
  ReconnectBackoff backoff({milliseconds{0}, milliseconds{0}, 0.5, 5.0}, 1);
  for (int i = 0; i < 10; ++i) {
    EXPECT_GE(backoff.NextDelay(), milliseconds{1});
  }
}

}  // namespace
}  // namespace audiofork
