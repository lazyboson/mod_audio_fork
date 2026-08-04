#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "audiofork/session_state.hpp"

namespace audiofork {
namespace {

using S = SessionState;
using E = SessionEvent;
using A = SessionAction;

// Models the driver contract: every action a transition emits eventually produces
// its completion event(s), and each pending item can resolve to any of its
// possible events. Exploring every delivery order of that pending set therefore
// covers every interleaving of shard callbacks vs. an externally injected
// teardown, including orders the real serialized driver can never produce.
enum class Item : std::uint8_t {
  kAttemptOutcome,
  kRetryTimer,
  kDrainOutcome,
  kCloseOutcome,
  kTeardown,
};
constexpr std::size_t kItemCount = 5;

std::vector<E> EventsFor(Item item) {
  switch (item) {
    case Item::kAttemptOutcome:
      return {E::kWsConnected, E::kWsError};
    case Item::kRetryTimer:
      return {E::kRetryTimerFired};
    case Item::kDrainOutcome:
      return {E::kDrainComplete, E::kWsError};
    case Item::kCloseOutcome:
      return {E::kCloseComplete, E::kWsError};
    case Item::kTeardown:
      return {E::kTeardown};
  }
  return {};
}

int TeardownRank(S state) {
  switch (state) {
    case S::kConnecting:
    case S::kActive:
    case S::kReconnecting:
      return 0;
    case S::kDraining:
      return 1;
    case S::kClosing:
      return 2;
    case S::kDead:
      return 3;
  }
  return -1;
}

struct Config {
  S state = S::kConnecting;
  int finalize_count = 0;
  int retry_budget = 3;
  std::array<int, kItemCount> pending{};

  void Add(Item item) { ++pending[static_cast<std::size_t>(item)]; }

  [[nodiscard]] bool NothingPending() const {
    for (const int count : pending) {
      if (count > 0) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] std::string Encode() const {
    std::string key;
    key += static_cast<char>('a' + static_cast<int>(state));
    key += static_cast<char>('a' + finalize_count);
    key += static_cast<char>('a' + retry_budget);
    for (const int count : pending) {
      key += static_cast<char>('a' + count);
    }
    return key;
  }
};

struct ExplorationStats {
  int configs = 0;
  int terminals = 0;
};

void Explore(const Config& config, std::set<std::string>& visited, ExplorationStats& stats) {
  if (!visited.insert(config.Encode()).second) {
    return;
  }
  ++stats.configs;

  if (config.NothingPending()) {
    ++stats.terminals;
    EXPECT_EQ(config.state, S::kDead) << "stuck in a non-terminal state with nothing pending";
    EXPECT_EQ(config.finalize_count, 1);
    return;
  }

  for (std::size_t i = 0; i < kItemCount; ++i) {
    if (config.pending[i] == 0) {
      continue;
    }
    for (const E event : EventsFor(static_cast<Item>(i))) {
      const SessionTransition transition = SessionTransitionFor(config.state, event);
      EXPECT_LE(TeardownRank(config.state), TeardownRank(transition.next))
          << "teardown progress must be monotonic";

      Config next = config;
      --next.pending[i];
      next.state = transition.next;

      switch (transition.action) {
        case A::kBeginConnect:
          next.Add(Item::kAttemptOutcome);
          break;
        case A::kScheduleRetry:
          if (next.retry_budget > 0) {
            --next.retry_budget;
            next.Add(Item::kRetryTimer);
          }
          break;
        case A::kBeginDrain:
          next.Add(Item::kDrainOutcome);
          break;
        case A::kBeginClose:
          next.Add(Item::kCloseOutcome);
          break;
        case A::kFinalize:
          ++next.finalize_count;
          EXPECT_EQ(transition.next, S::kDead) << "finalize outside of kDead entry";
          break;
        case A::kNone:
        case A::kNotifyConnected:
        case A::kAbandonSocket:
          break;
      }
      EXPECT_LE(next.finalize_count, 1) << "finalize fired twice";

      Explore(next, visited, stats);
    }
  }
}

TEST(SessionInterleavings, EveryDeliveryOrderTerminatesDeadWithOneFinalize) {
  Config initial;
  initial.Add(Item::kAttemptOutcome);
  initial.Add(Item::kTeardown);

  std::set<std::string> visited;
  ExplorationStats stats;
  Explore(initial, visited, stats);

  EXPECT_GT(stats.terminals, 0);
  RecordProperty("configs_explored", stats.configs);
  RecordProperty("terminal_configs", stats.terminals);
}

// Same exhaustive exploration, but with no reconnect budget at all — exercises
// the tail where teardown is the only way out of kReconnecting.
TEST(SessionInterleavings, TeardownAfterReconnectExhaustionStillTerminates) {
  Config initial;
  initial.retry_budget = 0;
  initial.Add(Item::kAttemptOutcome);
  initial.Add(Item::kTeardown);

  std::set<std::string> visited;
  ExplorationStats stats;
  Explore(initial, visited, stats);

  EXPECT_GT(stats.terminals, 0);
}

}  // namespace
}  // namespace audiofork
