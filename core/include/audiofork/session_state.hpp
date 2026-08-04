#pragma once

#include <atomic>
#include <cstdint>

namespace audiofork {

enum class SessionState : std::uint8_t {
  kConnecting,
  kActive,
  kReconnecting,
  kDraining,
  kClosing,
  kDead,
};

enum class SessionEvent : std::uint8_t {
  kWsConnected,
  kWsError,
  kTeardown,
  kRetryTimerFired,
  kDrainComplete,
  kCloseComplete,
};

enum class SessionAction : std::uint8_t {
  kNone,
  kNotifyConnected,
  kScheduleRetry,
  kBeginConnect,
  kBeginDrain,
  kBeginClose,
  kAbandonSocket,
  kFinalize,
};

struct SessionTransition {
  SessionState next;
  SessionAction action;
};

// Full transition table (DESIGN.md §4). kNone rows are deliberate ignores:
// idempotent teardown, events whose phase has already passed, kDead absorbing.
// A kWsConnected in any state but kConnecting is a socket nobody tracks and is
// abandoned rather than adopted — even from kDead, or it leaks.
// kFinalize is emitted only on transitions into kDead; since kDead is absorbing
// and OnEvent's CAS admits one winner, it fires exactly once per session.
[[nodiscard]] constexpr SessionTransition SessionTransitionFor(SessionState state,
                                                               SessionEvent event) noexcept {
  switch (state) {
    case SessionState::kConnecting:
      switch (event) {
        case SessionEvent::kWsConnected:
          return {SessionState::kActive, SessionAction::kNotifyConnected};
        case SessionEvent::kWsError:
          return {SessionState::kReconnecting, SessionAction::kScheduleRetry};
        case SessionEvent::kTeardown:
          return {SessionState::kClosing, SessionAction::kBeginClose};
        case SessionEvent::kRetryTimerFired:
        case SessionEvent::kDrainComplete:
        case SessionEvent::kCloseComplete:
          return {state, SessionAction::kNone};
      }
      break;
    case SessionState::kActive:
      switch (event) {
        case SessionEvent::kWsConnected:
          return {state, SessionAction::kAbandonSocket};
        case SessionEvent::kWsError:
          return {SessionState::kReconnecting, SessionAction::kScheduleRetry};
        case SessionEvent::kTeardown:
          return {SessionState::kDraining, SessionAction::kBeginDrain};
        case SessionEvent::kRetryTimerFired:
        case SessionEvent::kDrainComplete:
        case SessionEvent::kCloseComplete:
          return {state, SessionAction::kNone};
      }
      break;
    case SessionState::kReconnecting:
      switch (event) {
        case SessionEvent::kWsConnected:
          return {state, SessionAction::kAbandonSocket};
        case SessionEvent::kTeardown:
          return {SessionState::kDead, SessionAction::kFinalize};
        case SessionEvent::kRetryTimerFired:
          return {SessionState::kConnecting, SessionAction::kBeginConnect};
        case SessionEvent::kWsError:
        case SessionEvent::kDrainComplete:
        case SessionEvent::kCloseComplete:
          return {state, SessionAction::kNone};
      }
      break;
    case SessionState::kDraining:
      switch (event) {
        case SessionEvent::kWsConnected:
          return {state, SessionAction::kAbandonSocket};
        case SessionEvent::kWsError:
        case SessionEvent::kDrainComplete:
          return {SessionState::kClosing, SessionAction::kBeginClose};
        case SessionEvent::kTeardown:
        case SessionEvent::kRetryTimerFired:
        case SessionEvent::kCloseComplete:
          return {state, SessionAction::kNone};
      }
      break;
    case SessionState::kClosing:
      switch (event) {
        case SessionEvent::kWsConnected:
          return {state, SessionAction::kAbandonSocket};
        case SessionEvent::kWsError:
        case SessionEvent::kCloseComplete:
          return {SessionState::kDead, SessionAction::kFinalize};
        case SessionEvent::kTeardown:
        case SessionEvent::kRetryTimerFired:
        case SessionEvent::kDrainComplete:
          return {state, SessionAction::kNone};
      }
      break;
    case SessionState::kDead:
      switch (event) {
        case SessionEvent::kWsConnected:
          return {state, SessionAction::kAbandonSocket};
        case SessionEvent::kWsError:
        case SessionEvent::kTeardown:
        case SessionEvent::kRetryTimerFired:
        case SessionEvent::kDrainComplete:
        case SessionEvent::kCloseComplete:
          return {state, SessionAction::kNone};
      }
      break;
  }
  return {state, SessionAction::kNone};
}

// Thread-safe: any thread may deliver events; the CAS admits exactly one winner
// per state change, so racing teardown paths cannot double-fire an action.
class SessionStateMachine {
 public:
  struct Result {
    SessionState previous;
    SessionState current;
    SessionAction action;
  };

  [[nodiscard]] Result OnEvent(SessionEvent event) noexcept {
    SessionState current = state_.load(std::memory_order_acquire);
    for (;;) {
      const SessionTransition transition = SessionTransitionFor(current, event);
      if (transition.next == current) {
        return {current, current, transition.action};
      }
      // acq_rel on success: release publishes this thread's phase work to whoever
      // next observes the state; acquire syncs with the writes of prior transitions
      if (state_.compare_exchange_weak(current, transition.next, std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
        return {current, transition.next, transition.action};
      }
    }
  }

  [[nodiscard]] SessionState state() const noexcept {
    // acquire pairs with OnEvent's acq_rel so observers see the work behind the state
    return state_.load(std::memory_order_acquire);
  }

 private:
  std::atomic<SessionState> state_{SessionState::kConnecting};
};

}  // namespace audiofork
