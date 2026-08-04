#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "audiofork/backoff.hpp"
#include "audiofork/bytes.hpp"
#include "audiofork/config.hpp"
#include "audiofork/ports.hpp"
#include "audiofork/send_buffer.hpp"
#include "audiofork/session_state.hpp"
#include "audiofork/slab_pool.hpp"
#include "audiofork/spsc_ring.hpp"

namespace audiofork {

// One fork of one call: owns the state machine, the media→shard handoff ring,
// the outbound buffer, and the connection.
//
// Thread contract (DESIGN.md §3):
//   PushAudio  — media thread only; lock-free, allocation-free, never blocks.
//   Stop       — any thread; hops to the shard via NetPort::Post.
//   everything else — shard thread only.
//
// Lifetime (DESIGN.md §4): two strong refs, one per owner (media bug, shard
// registry). Buffers are released at finalize rather than at destruction, so a
// stray timer ref cannot pin megabytes after the call is gone.
class ForkSession : public NetHandler, public std::enable_shared_from_this<ForkSession> {
  struct PrivateTag {};

 public:
  struct Tuning {
    std::size_t send_cap_bytes = 0;
    std::size_t handoff_bytes = 0;
    std::size_t coalesce_max_bytes = 0;
    std::chrono::milliseconds drain_timeout{2000};
    ReconnectBackoff::Options backoff;
    std::uint64_t backoff_seed = 0;
  };

  struct Stats {
    std::uint64_t media_dropped_bytes = 0;
    std::uint64_t buffer_dropped_bytes = 0;
    std::uint64_t sent_bytes = 0;
    std::uint64_t reconnects = 0;
    std::uint64_t unsupported_inbound = 0;
    std::size_t buffered_bytes = 0;
  };

  [[nodiscard]] static std::shared_ptr<ForkSession> Create(ForkParams params, Tuning tuning,
                                                           NetPort& net, EventSink& events,
                                                           Clock& clock, SlabPool pool);

  ForkSession(PrivateTag, ForkParams params, Tuning tuning, NetPort& net, EventSink& events,
              Clock& clock, SlabPool pool);

  void Start();
  void Pump();
  void Stop();
  void set_on_finished(std::function<void()> callback) { on_finished_ = std::move(callback); }

  [[nodiscard]] bool PushAudio(ConstByteSpan pcm) noexcept;

  void OnConnected() override;
  void OnText(std::string_view text) override;
  void OnBinary(ConstByteSpan bytes) override;
  void OnClosed(bool connect_failed) override;

  [[nodiscard]] SessionState state() const { return state_.state(); }
  [[nodiscard]] const ForkParams& params() const { return params_; }
  [[nodiscard]] Stats stats() const;

 private:
  void Apply(SessionAction action);
  void BeginConnect();
  void BeginDrain();
  void BeginClose();
  void Finalize();
  void ScheduleRetry();
  void DrainHandoffRing();
  void FlushToConnection();
  void Emit(ForkEventType type, std::string detail = {});
  void EmitOverrunIfNewEpisode(std::size_t dropped);
  [[nodiscard]] std::uint64_t BytesToMs(std::uint64_t bytes) const;

  ForkParams params_;
  Tuning tuning_;
  NetPort& net_;
  EventSink& events_;
  Clock& clock_;

  SessionStateMachine state_;
  std::unique_ptr<SpscByteRing> ring_;
  SendBuffer buffer_;
  ReconnectBackoff backoff_;
  NetConnection* connection_ = nullptr;
  std::function<void()> on_finished_;

  bool reconnecting_ = false;
  bool dropping_ = false;
  bool bye_sent_ = false;
  std::optional<std::chrono::steady_clock::time_point> disconnected_at_;
  std::optional<std::chrono::steady_clock::time_point> drain_deadline_;
  std::uint64_t dropped_since_resume_ = 0;

  // Counters are written only on the shard thread but read from whichever
  // thread services `audio_fork status`, so they are relaxed atomics: no
  // ordering is implied between them, only per-field tear-freedom.
  std::atomic<std::uint64_t> media_dropped_bytes_{0};
  std::atomic<std::uint64_t> buffer_dropped_bytes_{0};
  std::atomic<std::uint64_t> sent_bytes_{0};
  std::atomic<std::uint64_t> reconnects_{0};
  std::atomic<std::uint64_t> unsupported_inbound_{0};
  std::atomic<std::size_t> buffered_bytes_{0};
};

}  // namespace audiofork
