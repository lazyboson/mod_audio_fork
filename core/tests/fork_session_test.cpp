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
  std::optional<SlabPool> pool = SlabPool::Create({4096, std::size_t{4096} * 512});
  std::shared_ptr<ForkSession> session;
  bool finished = false;

  explicit Fixture(std::chrono::milliseconds send_buffer = milliseconds{1000},
                   std::chrono::milliseconds handoff = milliseconds{500}) {
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

}  // namespace
}  // namespace audiofork
