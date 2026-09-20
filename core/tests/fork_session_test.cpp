#include "audiofork/fork_session.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <numeric>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace audiofork {
namespace {

using nlohmann::json;
using std::chrono::milliseconds;

constexpr std::uint32_t kRate = 16000;
constexpr std::uint8_t kChannels = 2;
constexpr std::size_t kBytesPerSecond = std::size_t{kRate} * kChannels * 2;

class FakeClock : public Clock {
 public:
  [[nodiscard]] std::chrono::steady_clock::time_point Now() const override { return now_; }
  void Advance(milliseconds delta) { now_ += delta; }

 private:
  std::chrono::steady_clock::time_point now_{std::chrono::steady_clock::duration::zero()};
};

class FakeConnection : public NetConnection {
 public:
  bool accept_sends = true;
  bool closed = false;
  bool receive_paused = false;
  std::vector<std::string> texts;
  std::vector<std::uint8_t> binary;

  [[nodiscard]] bool SendText(std::string_view text) override {
    if (!accept_sends) {
      return false;
    }
    texts.emplace_back(text);
    return true;
  }
  [[nodiscard]] bool SendBinary(ConstByteSpan bytes) override {
    if (!accept_sends) {
      return false;
    }
    binary.insert(binary.end(), bytes.begin(), bytes.end());
    return true;
  }
  void SetReceivePaused(bool paused) override { receive_paused = paused; }
  void Close() override { closed = true; }
};

class FakeNet : public NetPort {
 public:
  struct Timer {
    milliseconds delay;
    std::function<void()> task;
  };

  bool fail_connect = false;
  int connect_calls = 0;
  std::vector<std::unique_ptr<FakeConnection>> connections;
  std::vector<Timer> timers;
  std::vector<std::function<void()>> posted;

  [[nodiscard]] NetConnection* Connect(const Endpoint& /*endpoint*/, NetHandler& handler,
                                       std::size_t /*max_queued_bytes*/) override {
    ++connect_calls;
    handler_ = &handler;
    if (fail_connect) {
      return nullptr;
    }
    connections.push_back(std::make_unique<FakeConnection>());
    return connections.back().get();
  }
  void Post(std::function<void()> task) override { posted.push_back(std::move(task)); }
  void ScheduleTimer(milliseconds delay, std::function<void()> task) override {
    timers.push_back(Timer{delay, std::move(task)});
  }

  [[nodiscard]] FakeConnection& live() { return *connections.back(); }
  [[nodiscard]] NetHandler& handler() { return *handler_; }

  void RunPosted() {
    auto tasks = std::move(posted);
    posted.clear();
    for (auto& task : tasks) {
      task();
    }
  }
  [[nodiscard]] bool FireOneTimer() {
    if (timers.empty()) {
      return false;
    }
    auto timer = std::move(timers.front());
    timers.erase(timers.begin());
    timer.task();
    return true;
  }

 private:
  NetHandler* handler_ = nullptr;
};

class RecordingEvents : public EventSink {
 public:
  std::vector<ForkEvent> events;

  void Emit(const ForkEvent& event) override { events.push_back(event); }

  [[nodiscard]] std::size_t Count(ForkEventType type) const {
    return static_cast<std::size_t>(
        std::count_if(events.begin(), events.end(),
                      [type](const ForkEvent& event) { return event.type == type; }));
  }
  [[nodiscard]] const ForkEvent* Find(ForkEventType type) const {
    const auto it = std::find_if(events.begin(), events.end(),
                                 [type](const ForkEvent& event) { return event.type == type; });
    return it == events.end() ? nullptr : &*it;
  }
};

std::vector<std::uint8_t> Frame(std::size_t bytes, std::uint8_t seed = 0) {
  std::vector<std::uint8_t> pcm(bytes);
  std::iota(pcm.begin(), pcm.end(), seed);
  return pcm;
}

struct Fixture {
  FakeNet net;
  RecordingEvents events;
  FakeClock clock;
  std::optional<SlabPool> pool;
  std::shared_ptr<ForkSession> session;
  bool finished = false;

  explicit Fixture(std::chrono::milliseconds send_buffer = milliseconds{1000},
                   std::chrono::milliseconds handoff = milliseconds{500},
                   std::chrono::milliseconds emergency = milliseconds{0},
                   std::optional<SlabPool> shared_pool = std::nullopt) {
    pool =
        shared_pool.has_value() ? shared_pool : SlabPool::Create({4096, std::size_t{4096} * 512});
    ForkParams params;
    params.call_uuid = "call-1";
    params.fork_id = "fork-1";
    params.endpoint = {"127.0.0.1", 8080, "/", false};
    params.format = {kRate, kChannels};
    params.metadata_json = R"({"tenant":"acme"})";

    const AudioFormat format{kRate, kChannels};
    ForkSession::Tuning tuning;
    tuning.send_cap_bytes = format.BytesForDuration(send_buffer);
    tuning.handoff_bytes = format.BytesForDuration(handoff);
    tuning.coalesce_max_bytes = format.BytesForDuration(milliseconds{100});
    tuning.emergency_cap_bytes = format.BytesForDuration(emergency);
    tuning.drain_timeout = milliseconds{2000};
    tuning.backoff = {milliseconds{250}, milliseconds{5000}, 2.0, 0.0};

    session = ForkSession::Create(std::move(params), tuning, net, events, clock, *pool);
    EXPECT_NE(session, nullptr);
    session->set_on_finished([this] { finished = true; });
  }

  void Connect() {
    session->Start();
    net.handler().OnConnected();
  }
  void StopAndRun() {
    session->Stop();
    net.RunPosted();
  }
};

TEST(ForkSession, CreateRejectsZeroSizedTuning) {
  FakeNet net;
  RecordingEvents events;
  FakeClock clock;
  auto pool = SlabPool::Create({4096, std::size_t{4096} * 8});
  ASSERT_TRUE(pool.has_value());
  EXPECT_EQ(ForkSession::Create({}, {}, net, events, clock, *pool), nullptr);
}

TEST(ForkSession, HelloCarriesFormatAndMetadataOnConnect) {
  Fixture fx;
  fx.Connect();
  EXPECT_EQ(fx.session->state(), SessionState::kActive);
  ASSERT_EQ(fx.net.live().texts.size(), 1U);

  const json hello = json::parse(fx.net.live().texts[0]);
  EXPECT_EQ(hello["type"], "hello");
  EXPECT_EQ(hello["callSid"], "call-1");
  EXPECT_EQ(hello["rate"], kRate);
  EXPECT_EQ(hello["channels"], kChannels);
  EXPECT_EQ(hello["metadata"]["tenant"], "acme");
  EXPECT_EQ(fx.events.Count(ForkEventType::kConnect), 1U);
}

TEST(ForkSession, AudioPushedBeforeConnectIsSentAfterHello) {
  Fixture fx;
  const auto early = Frame(640, 1);
  EXPECT_TRUE(fx.session->PushAudio(ConstByteSpan(early)));

  fx.Connect();
  EXPECT_EQ(fx.net.live().binary, early);
}

TEST(ForkSession, AudioFlowsWhileActive) {
  Fixture fx;
  fx.Connect();
  const auto pcm = Frame(640, 5);
  EXPECT_TRUE(fx.session->PushAudio(ConstByteSpan(pcm)));
  fx.session->Pump();
  EXPECT_EQ(fx.net.live().binary, pcm);
  EXPECT_EQ(fx.session->stats().sent_bytes, pcm.size());
}

TEST(ForkSession, HandoffRingFullCountsMediaDropsWithoutBlocking) {
  Fixture fx(milliseconds{1000}, milliseconds{20});
  fx.session->Start();
  const auto big = Frame(kBytesPerSecond / 10);
  bool rejected = false;
  for (int i = 0; i < 20 && !rejected; ++i) {
    rejected = !fx.session->PushAudio(ConstByteSpan(big));
  }
  EXPECT_TRUE(rejected);
  EXPECT_GT(fx.session->stats().media_dropped_bytes, 0U);
}

TEST(ForkSession, StalledPeerDropsOldestAndEmitsOneOverrunPerEpisode) {
  Fixture fx(milliseconds{200}, milliseconds{500});
  fx.Connect();
  fx.net.live().accept_sends = false;

  const auto pcm = Frame(kBytesPerSecond / 10);
  for (int i = 0; i < 6; ++i) {
    EXPECT_TRUE(fx.session->PushAudio(ConstByteSpan(pcm)));
    fx.session->Pump();
  }
  EXPECT_GT(fx.session->stats().buffer_dropped_bytes, 0U);
  EXPECT_EQ(fx.events.Count(ForkEventType::kOverrun), 1U);

  const ForkEvent* overrun = fx.events.Find(ForkEventType::kOverrun);
  ASSERT_NE(overrun, nullptr);
  EXPECT_GT(overrun->dropped_ms, 0U);

  // recovery, then a second stall, is a new episode and reports again
  fx.net.live().accept_sends = true;
  fx.session->Pump();
  fx.net.live().accept_sends = false;
  for (int i = 0; i < 6; ++i) {
    EXPECT_TRUE(fx.session->PushAudio(ConstByteSpan(pcm)));
    fx.session->Pump();
  }
  EXPECT_EQ(fx.events.Count(ForkEventType::kOverrun), 2U);
}

TEST(ForkSession, PausedAudioIsDiscardedWithoutCountingAsAMediaDrop) {
  Fixture fx;
  fx.Connect();
  const auto before = Frame(640, 1);
  EXPECT_TRUE(fx.session->PushAudio(ConstByteSpan(before)));
  fx.session->Pump();

  fx.session->SetPaused(true);
  const auto muted = Frame(640, 2);
  // a paused push is accepted, not refused: the media bug has nothing to retry
  EXPECT_TRUE(fx.session->PushAudio(ConstByteSpan(muted)));
  fx.session->Pump();
  EXPECT_EQ(fx.net.live().binary, before);
  EXPECT_EQ(fx.session->stats().paused_bytes, muted.size());
  EXPECT_EQ(fx.session->stats().media_dropped_bytes, 0U);
  EXPECT_TRUE(fx.session->stats().paused);

  // pausing twice is not a second state change
  fx.session->SetPaused(true);
  EXPECT_TRUE(fx.session->PushAudio(ConstByteSpan(muted)));
  fx.session->Pump();
  EXPECT_EQ(fx.session->stats().paused_bytes, muted.size() * 2);

  fx.session->SetPaused(false);
  const auto after = Frame(640, 3);
  EXPECT_TRUE(fx.session->PushAudio(ConstByteSpan(after)));
  fx.session->Pump();
  EXPECT_FALSE(fx.session->stats().paused);
  EXPECT_EQ(fx.session->stats().paused_bytes, muted.size() * 2);

  std::vector<std::uint8_t> expected = before;
  expected.insert(expected.end(), after.begin(), after.end());
  EXPECT_EQ(fx.net.live().binary, expected);
  EXPECT_EQ(fx.session->stats().sent_bytes, expected.size());
}

TEST(ForkSession, PauseDoesNotTouchTheSocketOrPlayback) {
  Fixture fx;
  fx.Connect();
  const std::size_t texts_before = fx.net.live().texts.size();
  fx.session->SetPaused(true);
  fx.session->Pump();
  EXPECT_EQ(fx.net.live().texts.size(), texts_before);
  EXPECT_FALSE(fx.net.live().closed);
  EXPECT_EQ(fx.session->state(), SessionState::kActive);
}

TEST(ForkSession, OwnCapOverflowDropsWithoutDegrading) {
  Fixture fx(milliseconds{200}, milliseconds{500}, milliseconds{100});
  fx.Connect();
  fx.net.live().accept_sends = false;

  const auto pcm = Frame(kBytesPerSecond / 10);
  for (int i = 0; i < 6; ++i) {
    EXPECT_TRUE(fx.session->PushAudio(ConstByteSpan(pcm)));
    fx.session->Pump();
  }
  EXPECT_GT(fx.session->stats().buffer_dropped_bytes, 0U);
  EXPECT_FALSE(fx.session->stats().degraded);
  EXPECT_EQ(fx.events.Count(ForkEventType::kDegraded), 0U);
}

TEST(ForkSession, PoolExhaustionDegradesTheStarvedForkOnceThenRecovers) {
  auto shared = SlabPool::Create({4096, std::size_t{4096} * 12});
  ASSERT_TRUE(shared.has_value());
  Fixture hog(milliseconds{1000}, milliseconds{500}, milliseconds{0}, shared);
  Fixture starved(milliseconds{1000}, milliseconds{500}, milliseconds{200}, shared);
  hog.Connect();
  starved.Connect();
  hog.net.live().accept_sends = false;
  starved.net.live().accept_sends = false;

  const auto pcm = Frame(kBytesPerSecond / 10);
  for (int i = 0; i < 10; ++i) {
    EXPECT_TRUE(hog.session->PushAudio(ConstByteSpan(pcm)));
    hog.session->Pump();
  }
  // whatever the hog left over goes too, so the pool is dry to the byte
  std::vector<SlabLease> held;
  for (auto lease = shared->Acquire(); lease.has_value(); lease = shared->Acquire()) {
    held.push_back(*std::move(lease));
  }
  ASSERT_FALSE(shared->CanLease(1));
  // the hog has no emergency cap configured, so it drops but never degrades
  EXPECT_EQ(hog.events.Count(ForkEventType::kDegraded), 0U);
  EXPECT_FALSE(hog.session->stats().degraded);

  EXPECT_TRUE(starved.session->PushAudio(ConstByteSpan(pcm)));
  starved.session->Pump();
  EXPECT_TRUE(starved.session->stats().degraded);
  EXPECT_EQ(starved.events.Count(ForkEventType::kDegraded), 1U);
  EXPECT_EQ(starved.session->stats().buffer_dropped_bytes, pcm.size());

  // an empty buffer while the pool is still dry is not recovery: no flapping
  // back to the full cap, and no second event
  EXPECT_TRUE(starved.session->PushAudio(ConstByteSpan(pcm)));
  starved.session->Pump();
  EXPECT_TRUE(starved.session->stats().degraded);
  EXPECT_EQ(starved.events.Count(ForkEventType::kDegraded), 1U);
  EXPECT_EQ(starved.session->stats().buffer_dropped_bytes, pcm.size() * 2);

  held.clear();
  hog.net.live().accept_sends = true;
  for (int i = 0; i < 20; ++i) {
    hog.session->Pump();
  }
  ASSERT_EQ(hog.session->stats().buffered_bytes, 0U);

  // buffering again, but only to the emergency cap
  for (int i = 0; i < 4; ++i) {
    EXPECT_TRUE(starved.session->PushAudio(ConstByteSpan(pcm)));
    starved.session->Pump();
  }
  EXPECT_EQ(starved.session->stats().buffered_bytes, kBytesPerSecond / 5);
  EXPECT_TRUE(starved.session->stats().degraded);

  starved.net.live().accept_sends = true;
  for (int i = 0; i < 20; ++i) {
    starved.session->Pump();
  }
  EXPECT_EQ(starved.session->stats().buffered_bytes, 0U);
  EXPECT_FALSE(starved.session->stats().degraded);
  // recovery is silent: one event for the whole episode
  EXPECT_EQ(starved.events.Count(ForkEventType::kDegraded), 1U);

  starved.net.live().accept_sends = false;
  for (int i = 0; i < 4; ++i) {
    EXPECT_TRUE(starved.session->PushAudio(ConstByteSpan(pcm)));
    starved.session->Pump();
  }
  EXPECT_EQ(starved.session->stats().buffered_bytes, pcm.size() * 4);
}

TEST(ForkSession, MinimumSlabsHoldsOneCoalescedMessagePlusOne) {
  ForkSession::Tuning tuning;
  tuning.coalesce_max_bytes = 6400;
  EXPECT_EQ(ForkSession::MinimumSlabs(tuning, 4096), 3U);
  tuning.coalesce_max_bytes = 4096;
  EXPECT_EQ(ForkSession::MinimumSlabs(tuning, 4096), 2U);
  tuning.coalesce_max_bytes = 1;
  EXPECT_EQ(ForkSession::MinimumSlabs(tuning, 4096), 2U);
  EXPECT_EQ(ForkSession::MinimumSlabs(tuning, 0), 0U);
}

TEST(ForkSession, BufferedAudioSurvivesDisconnectAndResumeReportsGap) {
  Fixture fx;
  fx.Connect();

  const auto during_gap = Frame(640, 9);
  fx.net.handler().OnClosed(/*connect_failed=*/false);
  EXPECT_EQ(fx.session->state(), SessionState::kReconnecting);
  EXPECT_EQ(fx.events.Count(ForkEventType::kReconnecting), 1U);
  EXPECT_TRUE(fx.session->PushAudio(ConstByteSpan(during_gap)));

  fx.clock.Advance(milliseconds{1500});
  ASSERT_TRUE(fx.net.FireOneTimer());
  fx.net.handler().OnConnected();
  EXPECT_EQ(fx.session->state(), SessionState::kActive);

  ASSERT_GE(fx.net.live().texts.size(), 2U);
  EXPECT_EQ(json::parse(fx.net.live().texts[0])["type"], "hello");
  const json resume = json::parse(fx.net.live().texts[1]);
  EXPECT_EQ(resume["type"], "resume");
  EXPECT_EQ(resume["gapMs"], 1500);

  const ForkEvent* event = fx.events.Find(ForkEventType::kResume);
  ASSERT_NE(event, nullptr);
  EXPECT_EQ(event->gap_ms, 1500U);
  EXPECT_EQ(fx.net.live().binary, during_gap);
  EXPECT_EQ(fx.session->stats().reconnects, 1U);
}

TEST(ForkSession, ReconnectBackoffGrowsAcrossConsecutiveFailures) {
  Fixture fx;
  fx.session->Start();
  fx.net.handler().OnClosed(/*connect_failed=*/true);
  EXPECT_EQ(fx.events.Count(ForkEventType::kConnectFailed), 1U);

  std::vector<milliseconds> delays;
  for (int attempt = 0; attempt < 3; ++attempt) {
    ASSERT_EQ(fx.net.timers.size(), 1U);
    delays.push_back(fx.net.timers.front().delay);
    ASSERT_TRUE(fx.net.FireOneTimer());
    fx.net.handler().OnClosed(/*connect_failed=*/true);
  }
  EXPECT_EQ(delays[0], milliseconds{250});
  EXPECT_EQ(delays[1], milliseconds{500});
  EXPECT_EQ(delays[2], milliseconds{1000});
  // repeated failures report once, not once per attempt
  EXPECT_EQ(fx.events.Count(ForkEventType::kConnectFailed), 1U);
  EXPECT_EQ(fx.session->state(), SessionState::kReconnecting);
}

TEST(ForkSession, ConnectReturningNullSchedulesRetry) {
  Fixture fx;
  fx.net.fail_connect = true;
  fx.session->Start();
  EXPECT_EQ(fx.session->state(), SessionState::kReconnecting);
  EXPECT_EQ(fx.net.timers.size(), 1U);
  EXPECT_EQ(fx.events.Count(ForkEventType::kConnectFailed), 1U);
}

TEST(ForkSession, StopFromActiveDrainsSendsByeThenCloses) {
  Fixture fx;
  fx.Connect();
  const auto tail = Frame(640, 3);
  EXPECT_TRUE(fx.session->PushAudio(ConstByteSpan(tail)));

  fx.StopAndRun();
  EXPECT_EQ(fx.net.live().binary, tail);
  EXPECT_EQ(json::parse(fx.net.live().texts.back())["type"], "bye");
  EXPECT_TRUE(fx.net.live().closed);
  EXPECT_EQ(fx.session->state(), SessionState::kClosing);

  fx.net.handler().OnClosed(/*connect_failed=*/false);
  EXPECT_EQ(fx.session->state(), SessionState::kDead);
  EXPECT_TRUE(fx.finished);
  EXPECT_EQ(fx.events.Count(ForkEventType::kStop), 1U);
}

TEST(ForkSession, DrainGivesUpAtDeadlineWhenPeerNeverAccepts) {
  Fixture fx;
  fx.Connect();
  fx.net.live().accept_sends = false;
  const auto tail = Frame(640, 4);
  EXPECT_TRUE(fx.session->PushAudio(ConstByteSpan(tail)));

  fx.StopAndRun();
  EXPECT_EQ(fx.session->state(), SessionState::kDraining);

  fx.clock.Advance(milliseconds{2001});
  fx.session->Pump();
  EXPECT_EQ(fx.session->state(), SessionState::kClosing);
  EXPECT_TRUE(fx.net.live().closed);
}

TEST(ForkSession, StopWhileReconnectingFinalizesWithoutSocket) {
  Fixture fx;
  fx.Connect();
  fx.net.handler().OnClosed(/*connect_failed=*/false);
  ASSERT_EQ(fx.session->state(), SessionState::kReconnecting);

  fx.StopAndRun();
  EXPECT_EQ(fx.session->state(), SessionState::kDead);
  EXPECT_TRUE(fx.finished);

  // the pending retry timer must be inert now
  EXPECT_TRUE(fx.net.FireOneTimer());
  EXPECT_EQ(fx.session->state(), SessionState::kDead);
  EXPECT_EQ(fx.net.connect_calls, 1);
}

TEST(ForkSession, StopWhileConnectingClosesTheInFlightSocket) {
  Fixture fx;
  fx.session->Start();
  ASSERT_EQ(fx.session->state(), SessionState::kConnecting);

  fx.StopAndRun();
  EXPECT_EQ(fx.session->state(), SessionState::kClosing);
  EXPECT_TRUE(fx.net.live().closed);

  fx.net.handler().OnClosed(/*connect_failed=*/false);
  EXPECT_EQ(fx.session->state(), SessionState::kDead);
}

TEST(ForkSession, SocketCompletingAfterTeardownIsClosedNotAdopted) {
  Fixture fx;
  fx.session->Start();
  fx.StopAndRun();
  fx.net.handler().OnClosed(/*connect_failed=*/false);
  ASSERT_EQ(fx.session->state(), SessionState::kDead);

  // a late handshake completion must not resurrect the session, and must not
  // leak the socket either
  fx.net.handler().OnConnected();
  EXPECT_EQ(fx.session->state(), SessionState::kDead);
  EXPECT_TRUE(fx.net.live().closed);
}

TEST(ForkSession, PushAudioRejectedOnceTeardownStarts) {
  Fixture fx;
  fx.Connect();
  fx.StopAndRun();
  const auto pcm = Frame(320);
  EXPECT_FALSE(fx.session->PushAudio(ConstByteSpan(pcm)));
}

TEST(ForkSession, FinalizeReleasesSlabsEvenWithATimerStillHoldingTheSession) {
  Fixture fx(milliseconds{1000}, milliseconds{1000});
  fx.Connect();
  fx.net.live().accept_sends = false;
  const auto pcm = Frame(kBytesPerSecond / 5);
  EXPECT_TRUE(fx.session->PushAudio(ConstByteSpan(pcm)));
  fx.session->Pump();
  ASSERT_GT(fx.pool->stats().leased_slabs, 0U);

  fx.net.handler().OnClosed(/*connect_failed=*/false);
  fx.StopAndRun();
  ASSERT_EQ(fx.session->state(), SessionState::kDead);
  EXPECT_EQ(fx.pool->stats().leased_slabs, 0U);
}

TEST(ForkSession, ServerDisconnectTearsDownAndReports) {
  Fixture fx;
  fx.Connect();
  fx.net.handler().OnText(R"({"type":"disconnect"})");
  EXPECT_EQ(fx.events.Count(ForkEventType::kDisconnect), 1U);
  // nothing buffered, so drain completes within the same call and close begins
  EXPECT_EQ(fx.session->state(), SessionState::kClosing);
  EXPECT_EQ(json::parse(fx.net.live().texts.back())["type"], "bye");
  EXPECT_TRUE(fx.net.live().closed);
}

TEST(ForkSession, UnknownServerJsonIsRelayedAndBadJsonIsReported) {
  Fixture fx;
  fx.Connect();
  fx.net.handler().OnText(R"({"type":"transcript","text":"hi"})");
  const ForkEvent* relayed = fx.events.Find(ForkEventType::kJson);
  ASSERT_NE(relayed, nullptr);
  EXPECT_EQ(relayed->detail, R"({"type":"transcript","text":"hi"})");

  fx.net.handler().OnText("{not json");
  EXPECT_EQ(fx.events.Count(ForkEventType::kJsonError), 1U);
}

TEST(ForkSession, PlaybackTrafficReportsOnceWhilePlaybackIsDisabled) {
  Fixture fx;
  fx.Connect();
  const auto audio = Frame(320);
  fx.net.handler().OnBinary(ConstByteSpan(audio));
  fx.net.handler().OnBinary(ConstByteSpan(audio));
  fx.net.handler().OnText(R"({"type":"clear"})");
  EXPECT_EQ(fx.events.Count(ForkEventType::kError), 1U);
  EXPECT_EQ(fx.session->stats().unsupported_inbound, 3U);
}

TEST(ForkSession, HelloFailureTearsDownInsteadOfStreamingBlind) {
  Fixture fx;
  fx.session->Start();
  fx.net.live().accept_sends = false;
  fx.net.handler().OnConnected();
  EXPECT_EQ(fx.events.Count(ForkEventType::kError), 1U);
  EXPECT_NE(fx.session->state(), SessionState::kActive);
}

TEST(ForkSession, LongStallThenRecoveryKeepsNewestAudioAndAccountsDrops) {
  Fixture fx(milliseconds{200}, milliseconds{1000});
  fx.Connect();
  fx.net.live().accept_sends = false;

  const std::size_t chunk = kBytesPerSecond / 10;
  for (std::uint8_t round = 0; round < 8; ++round) {
    const auto pcm = Frame(chunk, round);
    EXPECT_TRUE(fx.session->PushAudio(ConstByteSpan(pcm)));
    fx.session->Pump();
  }
  const auto newest = Frame(chunk, 200);
  EXPECT_TRUE(fx.session->PushAudio(ConstByteSpan(newest)));
  fx.session->Pump();

  fx.net.live().accept_sends = true;
  for (int i = 0; i < 10; ++i) {
    fx.session->Pump();
  }

  const auto& sent = fx.net.live().binary;
  ASSERT_GE(sent.size(), newest.size());
  EXPECT_TRUE(std::equal(newest.begin(), newest.end(), sent.end() - newest.size()));
  EXPECT_GT(fx.session->stats().buffer_dropped_bytes, 0U);
  EXPECT_EQ(fx.session->stats().buffered_bytes, 0U);
}

TEST(ForkSession, SendTextIsForwardedVerbatimWhileActive) {
  Fixture fx;
  fx.Connect();
  const std::string payload = R"({"app":"say this","weird":"a b\tc"})";
  EXPECT_TRUE(fx.session->SendText(payload));
  fx.net.RunPosted();

  ASSERT_EQ(fx.net.live().texts.size(), 2U);
  EXPECT_EQ(fx.net.live().texts[1], payload);
  EXPECT_EQ(fx.session->stats().pending_texts_dropped, 0U);
}

TEST(ForkSession, SendTextBeforeConnectIsDeliveredAfterHelloInOrder) {
  Fixture fx;
  fx.session->Start();
  EXPECT_TRUE(fx.session->SendText(R"({"n":1})"));
  EXPECT_TRUE(fx.session->SendText(R"({"n":2})"));
  fx.net.RunPosted();
  EXPECT_TRUE(fx.net.live().texts.empty());

  fx.net.handler().OnConnected();
  ASSERT_EQ(fx.net.live().texts.size(), 3U);
  EXPECT_EQ(json::parse(fx.net.live().texts[0])["type"], "hello");
  EXPECT_EQ(fx.net.live().texts[1], R"({"n":1})");
  EXPECT_EQ(fx.net.live().texts[2], R"({"n":2})");
}

TEST(ForkSession, SendTextDuringReconnectIsDeliveredAfterResume) {
  Fixture fx;
  fx.Connect();
  fx.net.handler().OnClosed(/*connect_failed=*/false);
  ASSERT_EQ(fx.session->state(), SessionState::kReconnecting);
  EXPECT_TRUE(fx.session->SendText(R"({"n":7})"));
  fx.net.RunPosted();

  ASSERT_TRUE(fx.net.FireOneTimer());
  fx.net.handler().OnConnected();
  ASSERT_EQ(fx.net.live().texts.size(), 3U);
  EXPECT_EQ(json::parse(fx.net.live().texts[0])["type"], "hello");
  EXPECT_EQ(json::parse(fx.net.live().texts[1])["type"], "resume");
  EXPECT_EQ(fx.net.live().texts[2], R"({"n":7})");
}

TEST(ForkSession, PendingTextOverflowDropsOldestAndCounts) {
  Fixture fx;
  fx.session->Start();
  constexpr std::size_t kOverflow = 6;
  for (std::size_t i = 0; i < ForkSession::kMaxPendingTexts + kOverflow; ++i) {
    EXPECT_TRUE(fx.session->SendText(R"({"n":)" + std::to_string(i) + "}"));
  }
  fx.net.RunPosted();
  EXPECT_EQ(fx.session->stats().pending_texts_dropped, kOverflow);

  fx.net.handler().OnConnected();
  const auto& texts = fx.net.live().texts;
  ASSERT_EQ(texts.size(), ForkSession::kMaxPendingTexts + 1);
  EXPECT_EQ(texts[1], R"({"n":6})");
  EXPECT_EQ(texts.back(), R"({"n":69})");
}

TEST(ForkSession, SendTextRefusedByTheSocketIsCountedAsADrop) {
  Fixture fx;
  fx.Connect();
  fx.net.live().accept_sends = false;
  EXPECT_TRUE(fx.session->SendText(R"({"n":1})"));
  fx.net.RunPosted();

  EXPECT_EQ(fx.session->stats().pending_texts_dropped, 1U);
  EXPECT_EQ(fx.net.live().texts.size(), 1U);
}

TEST(ForkSession, SendTextAndSendDtmfRejectedOnceTeardownStarts) {
  Fixture fx;
  fx.Connect();
  fx.StopAndRun();
  ASSERT_EQ(fx.session->state(), SessionState::kClosing);
  EXPECT_FALSE(fx.session->SendText(R"({"n":1})"));
  EXPECT_FALSE(fx.session->SendDtmf('5', 160));
  EXPECT_TRUE(fx.net.posted.empty());

  fx.net.handler().OnClosed(/*connect_failed=*/false);
  ASSERT_EQ(fx.session->state(), SessionState::kDead);
  EXPECT_FALSE(fx.session->SendText(R"({"n":2})"));
  EXPECT_TRUE(fx.net.posted.empty());
}

TEST(ForkSession, TextStillQueuedAtFinalizeIsDiscardedAndCounted) {
  Fixture fx;
  fx.session->Start();
  EXPECT_TRUE(fx.session->SendText(R"({"n":1})"));
  EXPECT_TRUE(fx.session->SendText(R"({"n":2})"));
  fx.net.RunPosted();
  ASSERT_EQ(fx.session->stats().pending_texts_dropped, 0U);

  fx.StopAndRun();
  fx.net.handler().OnClosed(/*connect_failed=*/false);
  ASSERT_EQ(fx.session->state(), SessionState::kDead);
  EXPECT_EQ(fx.session->stats().pending_texts_dropped, 2U);
  EXPECT_TRUE(fx.net.live().texts.empty());
}

TEST(ForkSession, TextWhoseHopLandsAfterTeardownIsDropped) {
  Fixture fx;
  fx.Connect();
  EXPECT_TRUE(fx.session->SendText(R"({"n":1})"));
  ASSERT_EQ(fx.net.posted.size(), 1U);
  auto deliver = std::move(fx.net.posted.front());
  fx.net.posted.clear();

  fx.StopAndRun();
  ASSERT_EQ(fx.session->state(), SessionState::kClosing);
  deliver();

  EXPECT_EQ(fx.session->stats().pending_texts_dropped, 1U);
  ASSERT_EQ(fx.net.live().texts.size(), 2U);
  EXPECT_EQ(json::parse(fx.net.live().texts[1])["type"], "bye");
}

TEST(ForkSession, DtmfIsEncodedOnTheWire) {
  Fixture fx;
  fx.Connect();
  EXPECT_TRUE(fx.session->SendDtmf('5', 160));
  fx.net.RunPosted();

  ASSERT_EQ(fx.net.live().texts.size(), 2U);
  const json dtmf = json::parse(fx.net.live().texts[1]);
  EXPECT_EQ(dtmf["type"], "dtmf");
  EXPECT_EQ(dtmf["digit"], "5");
  EXPECT_EQ(dtmf["durationMs"], 160);
  EXPECT_EQ(dtmf.size(), 3U);
}

TEST(ForkSession, DtmfWithAnUnencodableDigitIsRejectedWithoutHopping) {
  Fixture fx;
  fx.Connect();
  EXPECT_FALSE(fx.session->SendDtmf('E', 160));
  EXPECT_TRUE(fx.net.posted.empty());
  EXPECT_EQ(fx.net.live().texts.size(), 1U);
}

TEST(ForkSession, DtmfAndAppTextShareOneQueueInOrder) {
  Fixture fx;
  fx.session->Start();
  EXPECT_TRUE(fx.session->SendText(R"({"n":1})"));
  EXPECT_TRUE(fx.session->SendDtmf('7', 80));
  EXPECT_TRUE(fx.session->SendText(R"({"n":2})"));
  fx.net.RunPosted();
  fx.net.handler().OnConnected();

  const auto& texts = fx.net.live().texts;
  ASSERT_EQ(texts.size(), 4U);
  EXPECT_EQ(texts[1], R"({"n":1})");
  EXPECT_EQ(json::parse(texts[2])["digit"], "7");
  EXPECT_EQ(json::parse(texts[2])["durationMs"], 80);
  EXPECT_EQ(texts[3], R"({"n":2})");
}

}  // namespace
}  // namespace audiofork
