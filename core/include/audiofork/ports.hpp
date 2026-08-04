#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "audiofork/bytes.hpp"
#include "audiofork/config.hpp"

namespace audiofork {

// Non-owning handle to a live connection, valid only between Connect() and the
// handler's OnClosed. Implementations are loop/shard-thread only.
class NetConnection {
 public:
  virtual ~NetConnection() = default;
  NetConnection() = default;
  NetConnection(const NetConnection&) = delete;
  NetConnection& operator=(const NetConnection&) = delete;
  NetConnection(NetConnection&&) = delete;
  NetConnection& operator=(NetConnection&&) = delete;

  [[nodiscard]] virtual bool SendText(std::string_view text) = 0;
  [[nodiscard]] virtual bool SendBinary(ConstByteSpan bytes) = 0;
  virtual void Close() = 0;
};

class NetHandler {
 public:
  virtual ~NetHandler() = default;
  virtual void OnConnected() = 0;
  virtual void OnText(std::string_view text) = 0;
  virtual void OnBinary(ConstByteSpan bytes) = 0;
  virtual void OnClosed(bool connect_failed) = 0;
};

// One shard's IO surface. Connect/Schedule are shard-thread only; Post is the
// only entry point callable from any thread.
class NetPort {
 public:
  virtual ~NetPort() = default;
  NetPort() = default;
  NetPort(const NetPort&) = delete;
  NetPort& operator=(const NetPort&) = delete;
  NetPort(NetPort&&) = delete;
  NetPort& operator=(NetPort&&) = delete;

  [[nodiscard]] virtual NetConnection* Connect(const Endpoint& endpoint, NetHandler& handler,
                                               std::size_t max_queued_bytes) = 0;
  virtual void Post(std::function<void()> task) = 0;
  virtual void ScheduleTimer(std::chrono::milliseconds delay, std::function<void()> task) = 0;
};

enum class ForkEventType : std::uint8_t {
  kConnect,
  kConnectFailed,
  kReconnecting,
  kResume,
  kOverrun,
  kDegraded,
  kJson,
  kJsonError,
  kDisconnect,
  kError,
  kStop,
};

struct ForkEvent {
  ForkEventType type = ForkEventType::kError;
  std::string call_uuid;
  std::string fork_id;
  std::string detail;
  std::uint64_t gap_ms = 0;
  std::uint64_t dropped_ms = 0;
};

class EventSink {
 public:
  virtual ~EventSink() = default;
  EventSink() = default;
  EventSink(const EventSink&) = delete;
  EventSink& operator=(const EventSink&) = delete;
  EventSink(EventSink&&) = delete;
  EventSink& operator=(EventSink&&) = delete;

  virtual void Emit(const ForkEvent& event) = 0;
};

// Monotonic time, injected so reconnect-gap accounting is deterministic in tests.
class Clock {
 public:
  virtual ~Clock() = default;
  Clock() = default;
  Clock(const Clock&) = delete;
  Clock& operator=(const Clock&) = delete;
  Clock(Clock&&) = delete;
  Clock& operator=(Clock&&) = delete;

  [[nodiscard]] virtual std::chrono::steady_clock::time_point Now() const = 0;
};

class SystemClock : public Clock {
 public:
  [[nodiscard]] std::chrono::steady_clock::time_point Now() const override {
    return std::chrono::steady_clock::now();
  }
};

}  // namespace audiofork
