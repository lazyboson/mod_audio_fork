#include "audiofork/fork_session.hpp"

#include <algorithm>
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
      buffer_(std::move(pool), tuning.send_cap_bytes),
      backoff_(tuning.backoff, tuning.backoff_seed) {}

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
  if (!ring_->Push(pcm)) {
    media_dropped_bytes_.fetch_add(pcm.size(), std::memory_order_relaxed);
    return false;
  }
  return true;
}

void ForkSession::Stop() {
  // may be called from the media or a control thread: never touch the
  // connection here, only hop to the shard
  auto self = shared_from_this();
  net_.Post([self] { self->Apply(self->state_.OnEvent(SessionEvent::kTeardown).action); });
}

void ForkSession::Pump() {
  DrainHandoffRing();
  if (state_.state() == SessionState::kActive) {
    FlushToConnection();
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
  // playback control arrives only from a server expecting M4 behaviour
  if (unsupported_inbound_.fetch_add(1, std::memory_order_relaxed) == 0) {
    Emit(ForkEventType::kError, "playback control received but playback is not enabled");
  }
}

void ForkSession::OnBinary(ConstByteSpan bytes) {
  if (bytes.empty()) {
    return;
  }
  if (unsupported_inbound_.fetch_add(1, std::memory_order_relaxed) == 0) {
    Emit(ForkEventType::kError, "inbound audio received but playback is not enabled");
  }
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
    const std::size_t dropped = buffer_.Append(ConstByteSpan(scratch.data(), count));
    buffered_bytes_.store(buffer_.size(), std::memory_order_relaxed);
    if (dropped > 0) {
      buffer_dropped_bytes_.fetch_add(dropped, std::memory_order_relaxed);
      dropped_since_resume_ += dropped;
      EmitOverrunIfNewEpisode(dropped);
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
               buffered_bytes_.load(std::memory_order_relaxed)};
}

}  // namespace audiofork
