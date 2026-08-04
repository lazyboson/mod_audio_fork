#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "audiofork/bytes.hpp"

struct lws;
struct lws_context;

namespace audiofork::net {

class WsEventLoop;

// Guards lws context creation/destruction, which mutate non-atomic globals
// shared by every context in the process. Anything else in this process that
// creates lws contexts (FreeSWITCH's mod_verto does) is outside this guard —
// see DESIGN.md §13.
[[nodiscard]] std::mutex& LwsLifecycleMutex();

// Owns the lws scheduler entry backing the repeating tick; defined in the
// implementation so lws types stay out of this header.
struct TickHandle;
[[nodiscard]] TickHandle* CreateTickHandle(WsEventLoop& loop, lws_context* context,
                                           std::chrono::milliseconds interval);
void DestroyTickHandle(TickHandle* handle);

void EnsureLwsLogPolicy();

// All handler callbacks arrive on the loop thread. OnClosed is delivered
// exactly once per connection; connect_failed is true only when the connection
// never reached OnConnected.
class WsConnectionHandler {
 public:
  virtual ~WsConnectionHandler() = default;
  virtual void OnConnected() = 0;
  virtual void OnText(std::string_view text) = 0;
  virtual void OnBinary(ConstByteSpan bytes) = 0;
  virtual void OnClosed(bool connect_failed) = 0;
};

struct WsEndpoint {
  std::string host;
  std::uint16_t port = 0;
  std::string path = "/";
};

class WsEventLoop;

// Owned by its WsEventLoop and destroyed after OnClosed returns; callers hold
// it as a non-owning pointer and must not use it past OnClosed. Every method
// is loop-thread only.
class WsConnection {
  struct PrivateTag {};

 public:
  WsConnection(PrivateTag, WsConnectionHandler& handler, std::size_t max_queued_bytes);

  [[nodiscard]] bool SendText(std::string_view text);
  [[nodiscard]] bool SendBinary(ConstByteSpan bytes);
  void Close();

 private:
  friend class WsEventLoop;
  struct Outgoing {
    std::vector<std::uint8_t> padded_payload;
    bool binary = false;
  };

  [[nodiscard]] bool Enqueue(ConstByteSpan bytes, bool binary);

  WsConnectionHandler& handler_;
  const std::size_t max_queued_bytes_;
  lws* wsi_ = nullptr;
  std::deque<Outgoing> outgoing_;
  std::size_t queued_bytes_ = 0;
  std::vector<std::uint8_t> incoming_;
  bool established_ = false;
  bool close_requested_ = false;
  bool closed_delivered_ = false;
};

// One instance per shard thread. Run() blocks on the owning thread; Stop() and
// Post() are the only thread-safe entry points, everything else is loop-thread
// only (the lws context must never be touched from another thread).
class WsEventLoop {
  struct PrivateTag {};

 public:
  [[nodiscard]] static std::unique_ptr<WsEventLoop> Create();

  explicit WsEventLoop(PrivateTag);
  ~WsEventLoop();
  WsEventLoop(const WsEventLoop&) = delete;
  WsEventLoop& operator=(const WsEventLoop&) = delete;
  WsEventLoop(WsEventLoop&&) = delete;
  WsEventLoop& operator=(WsEventLoop&&) = delete;

  void Run();
  void Stop();
  void Post(std::function<void()> task);

  // Repeating wake-up that drives per-session pumping; also the granularity at
  // which ScheduleTimer deadlines are honoured. Loop-thread or pre-Run only.
  void SetTick(std::chrono::milliseconds interval, std::function<void()> on_tick);
  void ScheduleTimer(std::chrono::milliseconds delay, std::function<void()> task);

  [[nodiscard]] WsConnection* Connect(const WsEndpoint& endpoint, WsConnectionHandler& handler,
                                      std::size_t max_queued_bytes);

  [[nodiscard]] int HandleLws(lws* wsi, int reason, void* user, void* in, std::size_t len);
  void OnTick();

 private:
  struct PendingTimer {
    std::chrono::steady_clock::time_point deadline;
    std::function<void()> task;
  };

  void DrainPosted();
  void DrainDueTimers();
  void FinishConnection(WsConnection& connection, bool connect_failed);

  lws_context* context_ = nullptr;
  std::atomic<bool> stop_{false};
  std::mutex posted_mutex_;
  std::vector<std::function<void()>> posted_;
  std::unordered_map<WsConnection*, std::unique_ptr<WsConnection>> connections_;
  std::function<void()> on_tick_;
  std::chrono::milliseconds tick_interval_{0};
  std::vector<PendingTimer> timers_;
  std::unique_ptr<TickHandle, decltype(&DestroyTickHandle)> tick_{nullptr, &DestroyTickHandle};
};

}  // namespace audiofork::net
