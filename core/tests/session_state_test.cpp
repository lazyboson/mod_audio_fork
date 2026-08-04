#include "audiofork/session_state.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <random>
#include <thread>
#include <vector>

namespace audiofork {
namespace {

using S = SessionState;
using E = SessionEvent;
using A = SessionAction;

struct Row {
  S state;
  E event;
  S next;
  A action;
};

constexpr std::array<Row, 36> kExpectedTable{{
    {S::kConnecting, E::kWsConnected, S::kActive, A::kNotifyConnected},
    {S::kConnecting, E::kWsError, S::kReconnecting, A::kScheduleRetry},
    {S::kConnecting, E::kTeardown, S::kClosing, A::kBeginClose},
    {S::kConnecting, E::kRetryTimerFired, S::kConnecting, A::kNone},
    {S::kConnecting, E::kDrainComplete, S::kConnecting, A::kNone},
    {S::kConnecting, E::kCloseComplete, S::kConnecting, A::kNone},

    {S::kActive, E::kWsConnected, S::kActive, A::kAbandonSocket},
    {S::kActive, E::kWsError, S::kReconnecting, A::kScheduleRetry},
    {S::kActive, E::kTeardown, S::kDraining, A::kBeginDrain},
    {S::kActive, E::kRetryTimerFired, S::kActive, A::kNone},
    {S::kActive, E::kDrainComplete, S::kActive, A::kNone},
    {S::kActive, E::kCloseComplete, S::kActive, A::kNone},

    {S::kReconnecting, E::kWsConnected, S::kReconnecting, A::kAbandonSocket},
    {S::kReconnecting, E::kWsError, S::kReconnecting, A::kNone},
    {S::kReconnecting, E::kTeardown, S::kDead, A::kFinalize},
    {S::kReconnecting, E::kRetryTimerFired, S::kConnecting, A::kBeginConnect},
    {S::kReconnecting, E::kDrainComplete, S::kReconnecting, A::kNone},
    {S::kReconnecting, E::kCloseComplete, S::kReconnecting, A::kNone},

    {S::kDraining, E::kWsConnected, S::kDraining, A::kAbandonSocket},
    {S::kDraining, E::kWsError, S::kClosing, A::kBeginClose},
    {S::kDraining, E::kTeardown, S::kDraining, A::kNone},
    {S::kDraining, E::kRetryTimerFired, S::kDraining, A::kNone},
    {S::kDraining, E::kDrainComplete, S::kClosing, A::kBeginClose},
    {S::kDraining, E::kCloseComplete, S::kDraining, A::kNone},

    {S::kClosing, E::kWsConnected, S::kClosing, A::kAbandonSocket},
    {S::kClosing, E::kWsError, S::kDead, A::kFinalize},
    {S::kClosing, E::kTeardown, S::kClosing, A::kNone},
    {S::kClosing, E::kRetryTimerFired, S::kClosing, A::kNone},
    {S::kClosing, E::kDrainComplete, S::kClosing, A::kNone},
    {S::kClosing, E::kCloseComplete, S::kDead, A::kFinalize},

    {S::kDead, E::kWsConnected, S::kDead, A::kAbandonSocket},
    {S::kDead, E::kWsError, S::kDead, A::kNone},
    {S::kDead, E::kTeardown, S::kDead, A::kNone},
    {S::kDead, E::kRetryTimerFired, S::kDead, A::kNone},
    {S::kDead, E::kDrainComplete, S::kDead, A::kNone},
    {S::kDead, E::kCloseComplete, S::kDead, A::kNone},
}};

TEST(SessionTransitionTable, MatchesTheDesignedTableExactly) {
  for (const Row& row : kExpectedTable) {
    const SessionTransition got = SessionTransitionFor(row.state, row.event);
    EXPECT_EQ(got.next, row.next) << "state=" << static_cast<int>(row.state)
                                  << " event=" << static_cast<int>(row.event);
    EXPECT_EQ(got.action, row.action)
        << "state=" << static_cast<int>(row.state) << " event=" << static_cast<int>(row.event);
  }
}

void ExpectStep(SessionStateMachine& machine, E event, S previous, S current, A action) {
  const auto result = machine.OnEvent(event);
  EXPECT_EQ(result.previous, previous);
  EXPECT_EQ(result.current, current);
  EXPECT_EQ(result.action, action);
  EXPECT_EQ(machine.state(), current);
}

TEST(SessionStateMachine, NormalLifecycle) {
  SessionStateMachine machine;
  EXPECT_EQ(machine.state(), S::kConnecting);
  ExpectStep(machine, E::kWsConnected, S::kConnecting, S::kActive, A::kNotifyConnected);
  ExpectStep(machine, E::kTeardown, S::kActive, S::kDraining, A::kBeginDrain);
  ExpectStep(machine, E::kDrainComplete, S::kDraining, S::kClosing, A::kBeginClose);
  ExpectStep(machine, E::kCloseComplete, S::kClosing, S::kDead, A::kFinalize);
}

TEST(SessionStateMachine, TeardownIsIdempotent) {
  SessionStateMachine machine;
  ExpectStep(machine, E::kWsConnected, S::kConnecting, S::kActive, A::kNotifyConnected);
  ExpectStep(machine, E::kTeardown, S::kActive, S::kDraining, A::kBeginDrain);
  ExpectStep(machine, E::kTeardown, S::kDraining, S::kDraining, A::kNone);
  ExpectStep(machine, E::kDrainComplete, S::kDraining, S::kClosing, A::kBeginClose);
  ExpectStep(machine, E::kTeardown, S::kClosing, S::kClosing, A::kNone);
}

TEST(SessionStateMachine, TeardownDuringConnectingSkipsDrain) {
  SessionStateMachine machine;
  ExpectStep(machine, E::kTeardown, S::kConnecting, S::kClosing, A::kBeginClose);
  ExpectStep(machine, E::kCloseComplete, S::kClosing, S::kDead, A::kFinalize);
}

TEST(SessionStateMachine, TeardownDuringReconnectingFinalizesDirectly) {
  SessionStateMachine machine;
  ExpectStep(machine, E::kWsError, S::kConnecting, S::kReconnecting, A::kScheduleRetry);
  ExpectStep(machine, E::kTeardown, S::kReconnecting, S::kDead, A::kFinalize);
  ExpectStep(machine, E::kRetryTimerFired, S::kDead, S::kDead, A::kNone);
}

TEST(SessionStateMachine, ReconnectLoopThenRecovery) {
  SessionStateMachine machine;
  ExpectStep(machine, E::kWsConnected, S::kConnecting, S::kActive, A::kNotifyConnected);
  ExpectStep(machine, E::kWsError, S::kActive, S::kReconnecting, A::kScheduleRetry);
  ExpectStep(machine, E::kRetryTimerFired, S::kReconnecting, S::kConnecting, A::kBeginConnect);
  ExpectStep(machine, E::kWsError, S::kConnecting, S::kReconnecting, A::kScheduleRetry);
  ExpectStep(machine, E::kRetryTimerFired, S::kReconnecting, S::kConnecting, A::kBeginConnect);
  ExpectStep(machine, E::kWsConnected, S::kConnecting, S::kActive, A::kNotifyConnected);
}

TEST(SessionStateMachine, SocketErrorDuringDrainStillCloses) {
  SessionStateMachine machine;
  ExpectStep(machine, E::kWsConnected, S::kConnecting, S::kActive, A::kNotifyConnected);
  ExpectStep(machine, E::kTeardown, S::kActive, S::kDraining, A::kBeginDrain);
  ExpectStep(machine, E::kWsError, S::kDraining, S::kClosing, A::kBeginClose);
  ExpectStep(machine, E::kCloseComplete, S::kClosing, S::kDead, A::kFinalize);
}

TEST(SessionStateMachine, ErrorDuringCloseFinalizesOnceOnly) {
  SessionStateMachine machine;
  ExpectStep(machine, E::kTeardown, S::kConnecting, S::kClosing, A::kBeginClose);
  ExpectStep(machine, E::kWsError, S::kClosing, S::kDead, A::kFinalize);
  ExpectStep(machine, E::kCloseComplete, S::kDead, S::kDead, A::kNone);
}

TEST(SessionStateMachine, LateSocketAfterDeathIsAbandonedNotAdopted) {
  SessionStateMachine machine;
  ExpectStep(machine, E::kTeardown, S::kConnecting, S::kClosing, A::kBeginClose);
  ExpectStep(machine, E::kCloseComplete, S::kClosing, S::kDead, A::kFinalize);
  ExpectStep(machine, E::kWsConnected, S::kDead, S::kDead, A::kAbandonSocket);
}

TEST(SessionStateMachine, ConcurrentChaosFinalizesExactlyOnce) {
  SessionStateMachine machine;
  std::atomic<int> finalize_count{0};
  constexpr int kThreads = 4;
  constexpr int kEventsPerThread = 10000;

  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([&machine, &finalize_count, t] {
      std::mt19937 rng(static_cast<unsigned>(t) + 1);
      std::uniform_int_distribution<int> pick(0, 5);
      for (int i = 0; i < kEventsPerThread; ++i) {
        const auto result = machine.OnEvent(static_cast<E>(pick(rng)));
        if (result.action == A::kFinalize) {
          finalize_count.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }

  for (int round = 0; round < 2; ++round) {
    for (const E event : {E::kTeardown, E::kDrainComplete, E::kCloseComplete}) {
      const auto result = machine.OnEvent(event);
      if (result.action == A::kFinalize) {
        finalize_count.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }

  EXPECT_EQ(machine.state(), S::kDead);
  EXPECT_EQ(finalize_count.load(), 1);
}

}  // namespace
}  // namespace audiofork
