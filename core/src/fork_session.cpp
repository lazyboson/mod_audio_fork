#include "audiofork/fork_session.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "audiofork/protocol.hpp"

namespace audiofork {
namespace {

constexpr std::size_t kDrainChunkBytes = std::size_t{8} * 1024;

}  // namespace

std::shared_ptr<ForkSession> ForkSession::Create(ForkParams params, Tuning tuning, NetPort& net,
                                                 EventSink& events, Clock& clock, SlabPool pool) {
  if (tuning.send_cap_bytes == 0 || tuning.handoff_bytes == 0 || tuning.coalesce_max_bytes == 0) {
    return nullptr;
  }
  auto session = std::make_shared<ForkSession>(PrivateTag{}, std::move(params), tuning, net, events,
                                               clock, std::move(pool));
  if (session->ring_ == nullptr) {
    return nullptr;
  }
  if (session->playback_.has_value() && session->playback_ring_ == nullptr) {
    return nullptr;
  }
  return session;
}

ForkSession::ForkSession(PrivateTag, ForkParams params, Tuning tuning, NetPort& net,
                         EventSink& events, Clock& clock, SlabPool pool)
    : params_(std::move(params)),
      tuning_(tuning),
      net_(net),
      events_(events),
      clock_(clock),
      ring_(SpscByteRing::Create(tuning.handoff_bytes)),
      buffer_(pool, tuning.send_cap_bytes),
      backoff_(tuning.backoff, tuning.backoff_seed) {
  if (tuning.playback_high_watermark_bytes > 0 && tuning.playback_handoff_bytes > 0) {
    playback_.emplace(std::move(pool),
                      JitterBuffer::Options{tuning.playback_high_watermark_bytes * 2});
    playback_ring_ = SpscByteRing::Create(tuning.playback_handoff_bytes);
    playback_rate_.store(params_.format.sample_rate, std::memory_order_relaxed);
    playback_channels_.store(1, std::memory_order_relaxed);
  }
}

void ForkSession::Start() { BeginConnect(); }

bool ForkSession::PushAudio(ConstByteSpan pcm) noexcept {
  switch (state_.state()) {
    case SessionState::kConnecting:
    case SessionState::kActive:
    case SessionState::kReconnecting:
      break;
    case SessionState::kDraining:
    case SessionState::kClosing:
    case SessionState::kDead:
      return false;
  }
  if (paused_.load(std::memory_order_relaxed)) {
    paused_bytes_.fetch_add(pcm.size(), std::memory_order_relaxed);
    return true;
  }
  if (!ring_->Push(pcm)) {
    media_dropped_bytes_.fetch_add(pcm.size(), std::memory_order_relaxed);
    return false;
  }
  return true;
}

std::size_t ForkSession::ReadPlayback(MutableByteSpan dest) noexcept {
  if (playback_ring_ == nullptr || dest.empty()) {
    return 0;
  }
  // a barge-in bumped the generation: discard audio already handed over, or the
  // caller would hear up to a ring's worth of interrupted speech
  const std::uint64_t generation = playback_generation_.load(std::memory_order_acquire);
  if (generation != media_playback_generation_.load(std::memory_order_relaxed)) {
    media_playback_generation_.store(generation, std::memory_order_relaxed);
    while (playback_ring_->Pop(dest) > 0) {
    }
    return 0;
  }
  const std::size_t filled = playback_ring_->Pop(dest);
  if (filled == 0) {
    playback_underruns_.fetch_add(1, std::memory_order_relaxed);
  } else {
    playback_bytes_played_.fetch_add(filled, std::memory_order_relaxed);
  }
  return filled;
}

AudioFormat ForkSession::playback_format() const noexcept {
  return AudioFormat{playback_rate_.load(std::memory_order_relaxed),
                     playback_channels_.load(std::memory_order_relaxed)};
}

void ForkSession::SetPaused(bool paused) { paused_.store(paused, std::memory_order_relaxed); }

void ForkSession::Stop() {
  // may be called from the media or a control thread: never touch the
  // connection here, only hop to the shard
  auto self = shared_from_this();
  net_.Post([self] { self->Apply(self->state_.OnEvent(SessionEvent::kTeardown).action); });
}

bool ForkSession::SendText(std::string text) {
  switch (state_.state()) {
    case SessionState::kConnecting:
    case SessionState::kActive:
    case SessionState::kReconnecting:
      break;
    case SessionState::kDraining:
    case SessionState::kClosing:
    case SessionState::kDead:
      return false;
  }
  // may be called from a control or FreeSWITCH session thread: never touch the
  // connection here, only hop to the shard
  auto self = shared_from_this();
  net_.Post([self, text = std::move(text)]() mutable { self->DeliverText(std::move(text)); });
  return true;
}

bool ForkSession::SendDtmf(char digit, std::uint32_t duration_ms) {
  auto encoded = EncodeDtmf(digit, duration_ms);
  if (!encoded.has_value()) {
    return false;
  }
  return SendText(std::move(*encoded));
}

void ForkSession::Pump() {
  DrainHandoffRing();
  PumpPlayback();
  if (state_.state() == SessionState::kActive) {
    FlushToConnection();
    RestoreCapIfDrained();
    return;
  }
  if (state_.state() != SessionState::kDraining) {
    return;
  }
  FlushToConnection();
  const bool drained = buffer_.empty();
  const bool expired = drain_deadline_.has_value() && clock_.Now() >= *drain_deadline_;
  if (drained && !bye_sent_ && connection_ != nullptr) {
    bye_sent_ = static_cast<bool>(connection_->SendText(EncodeBye()));
  }
  if ((drained && bye_sent_) || expired) {
    Apply(state_.OnEvent(SessionEvent::kDrainComplete).action);
  }
}

void ForkSession::OnConnected() {
  const auto result = state_.OnEvent(SessionEvent::kWsConnected);
  if (result.action == SessionAction::kAbandonSocket) {
    // teardown already ran: this socket has no owner, so close it or it leaks
    if (connection_ != nullptr) {
      connection_->Close();
      connection_ = nullptr;
    }
    return;
  }
  Apply(result.action);
}

void ForkSession::OnText(std::string_view text) {
  const ServerMessage message = ParseServerMessage(text);
  if (std::holds_alternative<ServerDisconnect>(message)) {
    Emit(ForkEventType::kDisconnect);
    Apply(state_.OnEvent(SessionEvent::kTeardown).action);
    return;
  }
  if (const auto* relay = std::get_if<RelayToApp>(&message)) {
    Emit(ForkEventType::kJson, relay->payload);
    return;
  }
  if (const auto* invalid = std::get_if<InvalidMessage>(&message)) {
    Emit(ForkEventType::kJsonError, invalid->reason);
    return;
  }
  if (!playback_.has_value()) {
    if (unsupported_inbound_.fetch_add(1, std::memory_order_relaxed) == 0) {
      Emit(ForkEventType::kError, "playback control received but playback is not enabled");
    }
    return;
  }
  if (const auto* start = std::get_if<StartPlayback>(&message)) {
    HandlePlaybackStart(start->sample_rate, start->channels);
    return;
  }
  if (std::holds_alternative<ClearPlayback>(message)) {
    HandleClear();
    return;
  }
  if (const auto* mark = std::get_if<PlaybackMark>(&message)) {
    HandleMark(mark->name);
    return;
  }
}

void ForkSession::OnBinary(ConstByteSpan bytes) {
  if (bytes.empty()) {
    return;
  }
  if (!playback_.has_value()) {
    if (unsupported_inbound_.fetch_add(1, std::memory_order_relaxed) == 0) {
      Emit(ForkEventType::kError, "inbound audio received but playback is not enabled");
    }
    return;
  }
  playback_bytes_received_.fetch_add(bytes.size(), std::memory_order_relaxed);
  const std::size_t refused = playback_->Append(bytes);
  // audio refused while muted was discarded on purpose by a barge-in, so only
  // the rest is a loss worth counting
  if (refused > 0 && !playback_->muted()) {
    playback_bytes_dropped_.fetch_add(refused, std::memory_order_relaxed);
    Emit(ForkEventType::kError, "playback buffer overflowed, audio discarded");
  }
  if (!playback_started_ && !playback_->empty()) {
    playback_started_ = true;
    Emit(ForkEventType::kPlaybackStart);
  }
  PumpPlayback();
}

void ForkSession::OnClosed(bool connect_failed) {
  connection_ = nullptr;
  if (!disconnected_at_.has_value()) {
    disconnected_at_ = clock_.Now();
  }
  if (connect_failed && state_.state() == SessionState::kConnecting && !reconnecting_) {
    Emit(ForkEventType::kConnectFailed);
  }
  Apply(state_.OnEvent(SessionEvent::kWsError).action);
}

void ForkSession::Apply(SessionAction action) {
  switch (action) {
    case SessionAction::kNotifyConnected: {
      backoff_.Reset();
      const auto hello = EncodeHello({params_.call_uuid, params_.format.sample_rate,
                                      params_.format.channels, params_.metadata_json});
      if (!hello.has_value()) {
        Emit(ForkEventType::kError, "invalid fork parameters for hello");
        Apply(state_.OnEvent(SessionEvent::kTeardown).action);
        return;
      }
      if (connection_ == nullptr || !connection_->SendText(*hello)) {
        Emit(ForkEventType::kError, "hello could not be sent");
        Apply(state_.OnEvent(SessionEvent::kWsError).action);
        return;
      }
      if (reconnecting_) {
        const std::uint64_t gap_ms =
            disconnected_at_.has_value()
                ? static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 clock_.Now() - *disconnected_at_)
                                                 .count())
                : 0;
        const std::uint64_t dropped_ms = BytesToMs(dropped_since_resume_);
        (void)connection_->SendText(EncodeResume(gap_ms, dropped_ms));
        ForkEvent event{
            ForkEventType::kResume, params_.call_uuid, params_.fork_id, {}, gap_ms, dropped_ms};
        events_.Emit(event);
        reconnecting_ = false;
        dropped_since_resume_ = 0;
        reconnects_.fetch_add(1, std::memory_order_relaxed);
      } else {
        Emit(ForkEventType::kConnect);
      }
      disconnected_at_.reset();
      FlushPendingTexts();
      Pump();
      return;
    }
    case SessionAction::kScheduleRetry:
      reconnecting_ = true;
      Emit(ForkEventType::kReconnecting);
      ScheduleRetry();
      return;
    case SessionAction::kBeginConnect:
      BeginConnect();
      return;
    case SessionAction::kBeginDrain:
      BeginDrain();
      return;
    case SessionAction::kBeginClose:
      BeginClose();
      return;
    case SessionAction::kAbandonSocket:
      if (connection_ != nullptr) {
        connection_->Close();
        connection_ = nullptr;
      }
      return;
    case SessionAction::kFinalize:
      Finalize();
      return;
    case SessionAction::kNone:
      return;
  }
}

void ForkSession::BeginConnect() {
  connection_ = net_.Connect(params_.endpoint, *this, tuning_.coalesce_max_bytes * 4);
  if (connection_ == nullptr) {
    if (!reconnecting_) {
      Emit(ForkEventType::kConnectFailed);
    }
    Apply(state_.OnEvent(SessionEvent::kWsError).action);
  }
}

void ForkSession::BeginDrain() {
  drain_deadline_ = clock_.Now() + tuning_.drain_timeout;
  if (connection_ == nullptr) {
    Apply(state_.OnEvent(SessionEvent::kDrainComplete).action);
    return;
  }
  Pump();
}

void ForkSession::BeginClose() {
  if (connection_ == nullptr) {
    Apply(state_.OnEvent(SessionEvent::kCloseComplete).action);
    return;
  }
  connection_->Close();
}

void ForkSession::Finalize() {
  // free slabs now rather than at destruction: a pending retry timer may hold
  // the last reference for seconds after the call is gone
  buffer_.Clear();
  pending_texts_dropped_.fetch_add(static_cast<std::uint64_t>(pending_texts_.size()),
                                   std::memory_order_relaxed);
  pending_texts_.clear();
  if (playback_.has_value()) {
    playback_->Clear();
    if (playback_started_) {
      Emit(ForkEventType::kPlaybackStop);
    }
  }
  connection_ = nullptr;
  Emit(ForkEventType::kStop);
  if (on_finished_) {
    auto callback = std::move(on_finished_);
    on_finished_ = nullptr;
    callback();
  }
}

void ForkSession::ScheduleRetry() {
  const auto delay = backoff_.NextDelay();
  std::weak_ptr<ForkSession> weak = weak_from_this();
  net_.ScheduleTimer(delay, [weak] {
    if (auto self = weak.lock()) {
      self->Apply(self->state_.OnEvent(SessionEvent::kRetryTimerFired).action);
    }
  });
}

void ForkSession::DrainHandoffRing() {
  std::vector<std::uint8_t> scratch(kDrainChunkBytes);
  for (;;) {
    const std::size_t count = ring_->Pop(MutableByteSpan(scratch));
    if (count == 0) {
      return;
    }
    const SendBuffer::AppendResult result = buffer_.Append(ConstByteSpan(scratch.data(), count));
    pool_starved_ = result.pool_exhausted;
    if (result.pool_exhausted) {
      const std::size_t shed = DegradeIfNewEpisode();
      buffer_dropped_bytes_.fetch_add(shed, std::memory_order_relaxed);
      dropped_since_resume_ += shed;
    }
    buffered_bytes_.store(buffer_.size(), std::memory_order_relaxed);
    if (result.dropped_bytes > 0) {
      buffer_dropped_bytes_.fetch_add(result.dropped_bytes, std::memory_order_relaxed);
      dropped_since_resume_ += result.dropped_bytes;
      EmitOverrunIfNewEpisode(result.dropped_bytes);
    } else {
      dropping_ = false;
    }
  }
}

void ForkSession::FlushToConnection() {
  if (connection_ == nullptr) {
    return;
  }
  std::size_t sent_this_tick = 0;
  while (!buffer_.empty() && sent_this_tick < tuning_.coalesce_max_bytes) {
    const ConstByteSpan chunk = buffer_.Peek(tuning_.coalesce_max_bytes - sent_this_tick);
    if (chunk.empty() || !connection_->SendBinary(chunk)) {
      return;
    }
    buffer_.Consume(chunk.size());
    sent_this_tick += chunk.size();
    sent_bytes_.fetch_add(chunk.size(), std::memory_order_relaxed);
  }
  buffered_bytes_.store(buffer_.size(), std::memory_order_relaxed);
}

void ForkSession::DeliverText(std::string text) {
  bool sendable = false;
  switch (state_.state()) {
    case SessionState::kActive:
      // Active with no socket: a late handshake abandoned this one, and the
      // reconnect that follows is what will flush the queue
      sendable = connection_ != nullptr;
      break;
    case SessionState::kConnecting:
    case SessionState::kReconnecting:
      break;
    case SessionState::kDraining:
    case SessionState::kClosing:
    case SessionState::kDead:
      pending_texts_dropped_.fetch_add(1, std::memory_order_relaxed);
      return;
  }
  if (!sendable) {
    QueueText(std::move(text));
    return;
  }
  if (!connection_->SendText(text)) {
    pending_texts_dropped_.fetch_add(1, std::memory_order_relaxed);
  }
}

void ForkSession::QueueText(std::string text) {
  if (pending_texts_.size() >= kMaxPendingTexts) {
    pending_texts_.pop_front();
    pending_texts_dropped_.fetch_add(1, std::memory_order_relaxed);
  }
  pending_texts_.push_back(std::move(text));
}

void ForkSession::FlushPendingTexts() {
  // a refused send is a drop, not a retry: nothing re-flushes this queue before
  // the next reconnect, so leaving entries behind would only delay the loss
  for (const auto& text : pending_texts_) {
    if (connection_ == nullptr || !connection_->SendText(text)) {
      pending_texts_dropped_.fetch_add(1, std::memory_order_relaxed);
    }
  }
  pending_texts_.clear();
}

void ForkSession::Emit(ForkEventType type, std::string detail) {
  ForkEvent event{type, params_.call_uuid, params_.fork_id, std::move(detail), 0, 0};
  events_.Emit(event);
}

void ForkSession::EmitOverrunIfNewEpisode(std::size_t dropped) {
  // one event per overrun episode, not per frame: 1,000 stalled forks would
  // otherwise flood the event bus at 50 events/sec each
  if (dropping_) {
    return;
  }
  dropping_ = true;
  ForkEvent event{ForkEventType::kOverrun,
                  params_.call_uuid,
                  params_.fork_id,
                  "send buffer full, dropping oldest audio",
                  0,
                  BytesToMs(dropped)};
  events_.Emit(event);
}

std::size_t ForkSession::DegradeIfNewEpisode() {
  if (tuning_.emergency_cap_bytes == 0 || degraded_.load(std::memory_order_relaxed)) {
    return 0;
  }
  degraded_.store(true, std::memory_order_relaxed);
  const std::size_t shed = buffer_.SetCap(tuning_.emergency_cap_bytes);
  Emit(ForkEventType::kDegraded, "global memory cap reached, send buffer cut to the emergency cap");
  return shed;
}

void ForkSession::RestoreCapIfDrained() {
  // An empty buffer is recovery only once the pool feeds this fork again:
  // while the pool is dry every frame is dropped, so emptiness alone would
  // restore the full cap once per tick and re-emit `degraded` each time.
  if (!degraded_.load(std::memory_order_relaxed) || pool_starved_ || !buffer_.empty()) {
    return;
  }
  (void)buffer_.SetCap(tuning_.send_cap_bytes);
  degraded_.store(false, std::memory_order_relaxed);
}

void ForkSession::PumpPlayback() {
  if (!playback_.has_value()) {
    return;
  }
  // hand as much as the ring will take; the media thread paces actual playback
  while (!playback_->empty()) {
    const ConstByteSpan chunk = playback_->Peek(tuning_.playback_handoff_bytes);
    if (chunk.empty() || !playback_ring_->Push(chunk)) {
      break;
    }
    playback_->Consume(chunk.size());
  }
  for (auto& name : playback_->TakeReachedMarks()) {
    Emit(ForkEventType::kMark, name);
  }
  playback_buffered_bytes_.store(playback_->size(), std::memory_order_relaxed);

  if (connection_ == nullptr) {
    return;
  }
  // Watermarks count everything unplayed, buffer AND handoff ring: the ring can
  // hold a sizeable slice, and ignoring it would let total buffering overshoot
  // the configured ceiling without ever pausing. While paused the peer's audio
  // waits in TCP buffers instead of ours, so nothing is lost.
  const std::size_t in_flight = playback_->size() + playback_ring_->size();
  if (!receive_paused_ && in_flight >= tuning_.playback_high_watermark_bytes) {
    receive_paused_ = true;
    connection_->SetReceivePaused(true);
  } else if (receive_paused_ && in_flight <= tuning_.playback_low_watermark_bytes) {
    receive_paused_ = false;
    connection_->SetReceivePaused(false);
  }
}

void ForkSession::HandlePlaybackStart(std::uint32_t sample_rate, std::uint8_t channels) {
  if (sample_rate != 0) {
    playback_rate_.store(sample_rate, std::memory_order_relaxed);
  }
  if (channels != 0) {
    playback_channels_.store(channels, std::memory_order_relaxed);
  }
  const AudioFormat format = playback_format();
  Emit(ForkEventType::kPlaybackStart, std::to_string(format.sample_rate) + "/" +
                                          std::to_string(static_cast<int>(format.channels)));
  playback_started_ = true;
}

void ForkSession::HandleClear() {
  playback_->Clear();
  // the media thread may already hold frames: bumping the generation makes it
  // discard them on its next read, which is what keeps barge-in inside one frame
  playback_generation_.fetch_add(1, std::memory_order_release);
  playback_buffered_bytes_.store(0, std::memory_order_relaxed);
  barge_ins_.fetch_add(1, std::memory_order_relaxed);
  playback_started_ = false;
  if (receive_paused_ && connection_ != nullptr) {
    receive_paused_ = false;
    connection_->SetReceivePaused(false);
  }
  Emit(ForkEventType::kPlaybackCleared);
}

void ForkSession::HandleMark(std::string name) { playback_->AddMark(std::move(name)); }

std::uint64_t ForkSession::BytesToMs(std::uint64_t bytes) const {
  const std::size_t per_second = params_.format.BytesPerSecond();
  if (per_second == 0) {
    return 0;
  }
  return bytes * 1000 / per_second;
}

ForkSession::Stats ForkSession::stats() const {
  return Stats{media_dropped_bytes_.load(std::memory_order_relaxed),
               buffer_dropped_bytes_.load(std::memory_order_relaxed),
               sent_bytes_.load(std::memory_order_relaxed),
               reconnects_.load(std::memory_order_relaxed),
               unsupported_inbound_.load(std::memory_order_relaxed),
               buffered_bytes_.load(std::memory_order_relaxed),
               playback_bytes_received_.load(std::memory_order_relaxed),
               playback_bytes_played_.load(std::memory_order_relaxed),
               playback_underruns_.load(std::memory_order_relaxed),
               barge_ins_.load(std::memory_order_relaxed),
               playback_buffered_bytes_.load(std::memory_order_relaxed),
               pending_texts_dropped_.load(std::memory_order_relaxed),
               playback_bytes_dropped_.load(std::memory_order_relaxed),
               paused_bytes_.load(std::memory_order_relaxed),
               degraded_.load(std::memory_order_relaxed),
               paused_.load(std::memory_order_relaxed)};
}

std::size_t ForkSession::MinimumSlabs(const Tuning& tuning, std::size_t slab_bytes) {
  if (slab_bytes == 0) {
    return 0;
  }
  return (tuning.coalesce_max_bytes + slab_bytes - 1) / slab_bytes + 1;
}

}  // namespace audiofork
