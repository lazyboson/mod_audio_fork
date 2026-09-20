#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "audiofork/backoff.hpp"
#include "audiofork/bytes.hpp"
#include "audiofork/config.hpp"
#include "audiofork/jitter_buffer.hpp"
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
//   Stop, SendText, SendDtmf — any thread; hop to the shard via NetPort::Post.
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
    // zero disables degradation: the send cap never shrinks (DESIGN.md §5)
    std::size_t emergency_cap_bytes = 0;
    std::chrono::milliseconds drain_timeout{2000};
    ReconnectBackoff::Options backoff;
    std::uint64_t backoff_seed = 0;
    // zero disables playback: the fork is then send-only
    std::size_t playback_high_watermark_bytes = 0;
    std::size_t playback_low_watermark_bytes = 0;
    std::size_t playback_handoff_bytes = 0;
  };

  struct Stats {
    std::uint64_t media_dropped_bytes = 0;
    std::uint64_t buffer_dropped_bytes = 0;
    std::uint64_t sent_bytes = 0;
    std::uint64_t reconnects = 0;
    std::uint64_t unsupported_inbound = 0;
    std::size_t buffered_bytes = 0;
    std::uint64_t playback_bytes_received = 0;
    std::uint64_t playback_bytes_played = 0;
    std::uint64_t playback_underruns = 0;
    std::uint64_t barge_ins = 0;
    std::size_t playback_buffered_bytes = 0;
    std::uint64_t pending_texts_dropped = 0;
    std::uint64_t playback_bytes_dropped = 0;
    bool degraded = false;
  };

  static constexpr std::size_t kMaxPendingTexts = 64;

  // A fork that cannot hold one coalesced message plus a slab for playback has
  // no working set: it could only drop. The module refuses to start one rather
  // than let the global cap produce forks that are born broken (DESIGN.md §5).
  [[nodiscard]] static std::size_t MinimumSlabs(const Tuning& tuning, std::size_t slab_bytes);

  [[nodiscard]] static std::shared_ptr<ForkSession> Create(ForkParams params, Tuning tuning,
                                                           NetPort& net, EventSink& events,
                                                           Clock& clock, SlabPool pool);

  ForkSession(PrivateTag, ForkParams params, Tuning tuning, NetPort& net, EventSink& events,
              Clock& clock, SlabPool pool);

  void Start();
  void Pump();
  void Stop();
  // False means the text was refused outright (teardown started, or a digit the
  // wire protocol has no encoding for); true only means it was handed to the
  // shard, which may still drop it if the queue overflows or the socket refuses.
  [[nodiscard]] bool SendText(std::string text);
  [[nodiscard]] bool SendDtmf(char digit, std::uint32_t duration_ms);
  void set_on_finished(std::function<void()> callback) { on_finished_ = std::move(callback); }

  [[nodiscard]] bool PushAudio(ConstByteSpan pcm) noexcept;

  // MEDIA THREAD, lock-free: fills dest with playback audio and returns how many
  // bytes were written. A short (or zero) return means the caller must leave the
  // rest of the frame as it was — never inject silence over live call audio.
  [[nodiscard]] std::size_t ReadPlayback(MutableByteSpan dest) noexcept;
  [[nodiscard]] bool playback_enabled() const { return playback_.has_value(); }
  [[nodiscard]] AudioFormat playback_format() const noexcept;

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
  void DeliverText(std::string text);
  void QueueText(std::string text);
  void FlushPendingTexts();
  void PumpPlayback();
  void HandlePlaybackStart(std::uint32_t sample_rate, std::uint8_t channels);
  void HandleClear();
  void HandleMark(std::string name);
  void Emit(ForkEventType type, std::string detail = {});
  void EmitOverrunIfNewEpisode(std::size_t dropped);
  // Returns the bytes the shrunken cap shed, 0 when already degraded.
  [[nodiscard]] std::size_t DegradeIfNewEpisode();
  void RestoreCapIfDrained();
  [[nodiscard]] std::uint64_t BytesToMs(std::uint64_t bytes) const;

  ForkParams params_;
  Tuning tuning_;
  NetPort& net_;
  EventSink& events_;
  Clock& clock_;

  SessionStateMachine state_;
  std::unique_ptr<SpscByteRing> ring_;
  SendBuffer buffer_;
  // playback state: the buffer is shard-owned, the ring hands frames to the
  // media thread, and the generation lets a barge-in discard what is already in
  // flight without either side taking a lock
  std::optional<JitterBuffer> playback_;
  std::unique_ptr<SpscByteRing> playback_ring_;
  std::atomic<std::uint64_t> playback_generation_{0};
  std::atomic<std::uint64_t> media_playback_generation_{0};
  std::atomic<std::uint32_t> playback_rate_{0};
  std::atomic<std::uint8_t> playback_channels_{0};
  bool playback_started_ = false;
  bool receive_paused_ = false;
  ReconnectBackoff backoff_;
  NetConnection* connection_ = nullptr;
  std::function<void()> on_finished_;

  // app text and DTMF that arrived before the socket was usable; flushed after
  // hello (and resume) so the server never sees them ahead of the handshake
  std::deque<std::string> pending_texts_;

  bool reconnecting_ = false;
  bool dropping_ = false;
  bool pool_starved_ = false;
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
  std::atomic<std::uint64_t> playback_bytes_received_{0};
  std::atomic<std::uint64_t> playback_bytes_played_{0};
  std::atomic<std::uint64_t> playback_underruns_{0};
  std::atomic<std::uint64_t> barge_ins_{0};
  std::atomic<std::size_t> playback_buffered_bytes_{0};
  std::atomic<std::uint64_t> pending_texts_dropped_{0};
  std::atomic<std::uint64_t> playback_bytes_dropped_{0};
  std::atomic<bool> degraded_{false};
};

}  // namespace audiofork
