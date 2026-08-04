// Playback path through ForkSession: server audio in, media-thread frames out,
// barge-in, marks, and watermark flow control (DESIGN.md §7, decision 13).
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "audiofork/fork_session.hpp"

namespace audiofork {
namespace {

using std::chrono::milliseconds;

constexpr std::uint32_t kRate = 16000;
constexpr std::size_t kFrameBytes = 640;

class FakeClock : public Clock {
 public:
  [[nodiscard]] std::chrono::steady_clock::time_point Now() const override { return now_; }
  void Advance(milliseconds delta) { now_ += delta; }

 private:
  std::chrono::steady_clock::time_point now_{std::chrono::steady_clock::duration::zero()};
};

class FakeConnection : public NetConnection {
 public:
  bool receive_paused = false;
  int pause_transitions = 0;
  std::vector<std::string> texts;

  [[nodiscard]] bool SendText(std::string_view text) override {
    texts.emplace_back(text);
    return true;
  }
  [[nodiscard]] bool SendBinary(ConstByteSpan /*bytes*/) override { return true; }
  void SetReceivePaused(bool paused) override {
    if (paused != receive_paused) {
      ++pause_transitions;
    }
    receive_paused = paused;
  }
  void Close() override {}
};

class FakeNet : public NetPort {
 public:
  std::vector<std::unique_ptr<FakeConnection>> connections;

  [[nodiscard]] NetConnection* Connect(const Endpoint& /*endpoint*/, NetHandler& handler,
                                       std::size_t /*max_queued_bytes*/) override {
    handler_ = &handler;
    connections.push_back(std::make_unique<FakeConnection>());
    return connections.back().get();
  }
  void Post(std::function<void()> task) override { task(); }
  void ScheduleTimer(milliseconds /*delay*/, std::function<void()> /*task*/) override {}

  [[nodiscard]] FakeConnection& live() { return *connections.back(); }
  [[nodiscard]] NetHandler& handler() { return *handler_; }

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
  [[nodiscard]] std::vector<std::string> Details(ForkEventType type) const {
    std::vector<std::string> details;
    for (const ForkEvent& event : events) {
      if (event.type == type) {
        details.push_back(event.detail);
      }
    }
    return details;
  }
};

std::vector<std::uint8_t> Tone(std::size_t bytes, std::uint8_t seed = 0) {
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

  explicit Fixture(milliseconds high = milliseconds{1000}, milliseconds low = milliseconds{200},
                   bool playback = true) {
    ForkParams params;
    params.call_uuid = "call-pb";
    params.fork_id = "fork-pb";
    params.endpoint = {"127.0.0.1", 9099, "/", false};
    params.format = {kRate, 1};

    const AudioFormat format{kRate, 1};
    ForkSession::Tuning tuning;
    tuning.send_cap_bytes = format.BytesForDuration(milliseconds{1000});
    tuning.handoff_bytes = format.BytesForDuration(milliseconds{500});
    tuning.coalesce_max_bytes = format.BytesForDuration(milliseconds{100});
    tuning.backoff = {milliseconds{250}, milliseconds{5000}, 2.0, 0.0};
    if (playback) {
      tuning.playback_high_watermark_bytes = format.BytesForDuration(high);
      tuning.playback_low_watermark_bytes = format.BytesForDuration(low);
      tuning.playback_handoff_bytes = format.BytesForDuration(milliseconds{200});
    }

    session = ForkSession::Create(std::move(params), tuning, net, events, clock, *pool);
    EXPECT_NE(session, nullptr);
    session->Start();
    net.handler().OnConnected();
  }

  // one media-thread frame slot
  [[nodiscard]] std::vector<std::uint8_t> ReadFrame(std::size_t bytes = kFrameBytes) {
    std::vector<std::uint8_t> frame(bytes);
    const std::size_t filled = session->ReadPlayback(MutableByteSpan(frame));
    frame.resize(filled);
    return frame;
  }

  [[nodiscard]] std::vector<std::uint8_t> ReadAllFrames(int max_frames = 200) {
    std::vector<std::uint8_t> all;
    for (int i = 0; i < max_frames; ++i) {
      const auto frame = ReadFrame();
      if (frame.empty()) {
        break;
      }
      all.insert(all.end(), frame.begin(), frame.end());
    }
    return all;
  }
};

TEST(Playback, DisabledByDefaultTuningReportsUnsupportedOnce) {
  Fixture fx(milliseconds{1000}, milliseconds{200}, /*playback=*/false);
  EXPECT_FALSE(fx.session->playback_enabled());

  const auto audio = Tone(kFrameBytes);
  fx.net.handler().OnBinary(ConstByteSpan(audio));
  fx.net.handler().OnBinary(ConstByteSpan(audio));
  EXPECT_EQ(fx.events.Count(ForkEventType::kError), 1U);
  EXPECT_TRUE(fx.ReadFrame().empty());
}

TEST(Playback, ServerAudioReachesTheMediaThreadByteExact) {
  Fixture fx;
  ASSERT_TRUE(fx.session->playback_enabled());
  const auto audio = Tone(kFrameBytes * 3, 5);
  fx.net.handler().OnBinary(ConstByteSpan(audio));

  EXPECT_EQ(fx.ReadAllFrames(), audio);
  EXPECT_EQ(fx.session->stats().playback_bytes_received, audio.size());
  EXPECT_EQ(fx.session->stats().playback_bytes_played, audio.size());
  EXPECT_EQ(fx.events.Count(ForkEventType::kPlaybackStart), 1U);
}

TEST(Playback, StarvedFrameReturnsNothingSoLiveAudioIsNotOverwritten) {
  Fixture fx;
  EXPECT_TRUE(fx.ReadFrame().empty());
  EXPECT_GE(fx.session->stats().playback_underruns, 1U);

  const auto audio = Tone(kFrameBytes);
  fx.net.handler().OnBinary(ConstByteSpan(audio));
  EXPECT_EQ(fx.ReadFrame(), audio);

  // and starving again after playing is still a clean no-op
  EXPECT_TRUE(fx.ReadFrame().empty());
}

TEST(Playback, PartialFrameReturnsOnlyWhatExists) {
  Fixture fx;
  const auto audio = Tone(100, 3);
  fx.net.handler().OnBinary(ConstByteSpan(audio));
  const auto frame = fx.ReadFrame(kFrameBytes);
  EXPECT_EQ(frame, audio);
}

TEST(Playback, StartPlaybackDeclaresFormat) {
  Fixture fx;
  fx.net.handler().OnText(R"({"type":"start_playback","rate":8000,"channels":1})");
  EXPECT_EQ(fx.session->playback_format().sample_rate, 8000U);
  EXPECT_EQ(fx.session->playback_format().channels, 1);
  const auto details = fx.events.Details(ForkEventType::kPlaybackStart);
  ASSERT_FALSE(details.empty());
  EXPECT_EQ(details.back(), "8000/1");
}

TEST(Playback, FormatDefaultsToTheForkRateUntilDeclared) {
  Fixture fx;
  EXPECT_EQ(fx.session->playback_format().sample_rate, kRate);
  EXPECT_EQ(fx.session->playback_format().channels, 1);
}

TEST(Playback, ClearDiscardsAudioAlreadyHandedToTheMediaThread) {
  Fixture fx;
  // three frames buffered and pushed into the handoff ring
  const auto audio = Tone(kFrameBytes * 3, 9);
  fx.net.handler().OnBinary(ConstByteSpan(audio));

  // caller barges in after hearing only the first frame
  const auto played = fx.ReadFrame();
  EXPECT_EQ(played.size(), kFrameBytes);

  fx.net.handler().OnText(R"({"type":"clear"})");
  EXPECT_EQ(fx.events.Count(ForkEventType::kPlaybackCleared), 1U);

  // the very next frame must already be silent: barge-in within one frame
  EXPECT_TRUE(fx.ReadFrame().empty());
  EXPECT_TRUE(fx.ReadFrame().empty());
  EXPECT_EQ(fx.session->stats().barge_ins, 1U);
}

TEST(Playback, AudioAfterClearIsMutedUntilAMarkArrives) {
  Fixture fx;
  fx.net.handler().OnText(R"({"type":"clear"})");

  // in-flight TTS the server had already sent must not play
  const auto residue = Tone(kFrameBytes, 21);
  fx.net.handler().OnBinary(ConstByteSpan(residue));
  EXPECT_TRUE(fx.ReadFrame().empty());

  fx.net.handler().OnText(R"({"type":"mark","name":"turn-2"})");
  const auto fresh = Tone(kFrameBytes, 33);
  fx.net.handler().OnBinary(ConstByteSpan(fresh));
  EXPECT_EQ(fx.ReadFrame(), fresh);
}

TEST(Playback, MarkFiresAnEventOnceItsAudioHasBeenHandedOver) {
  Fixture fx;
  const auto first = Tone(kFrameBytes, 1);
  fx.net.handler().OnBinary(ConstByteSpan(first));
  fx.net.handler().OnText(R"({"type":"mark","name":"sentence-1"})");
  fx.session->Pump();

  const auto details = fx.events.Details(ForkEventType::kMark);
  ASSERT_EQ(details.size(), 1U);
  EXPECT_EQ(details[0], "sentence-1");
}

TEST(Playback, WatermarkPausesAndResumesTheSocket) {
  Fixture fx(milliseconds{200}, milliseconds{50});
  const AudioFormat format{kRate, 1};
  const std::size_t high_bytes = format.BytesForDuration(milliseconds{200});

  // a bursty TTS server: more than the high watermark in one go, with the media
  // thread not yet consuming
  const auto burst = Tone(high_bytes * 2);
  fx.net.handler().OnBinary(ConstByteSpan(burst));
  EXPECT_TRUE(fx.net.live().receive_paused);

  (void)fx.ReadAllFrames();
  fx.session->Pump();
  (void)fx.ReadAllFrames();
  fx.session->Pump();
  EXPECT_FALSE(fx.net.live().receive_paused);
  EXPECT_GE(fx.net.live().pause_transitions, 2);
}

TEST(Playback, ClearReleasesReceivePauseSoTheNextTurnFlowsImmediately) {
  Fixture fx(milliseconds{200}, milliseconds{50});
  const AudioFormat format{kRate, 1};
  fx.net.handler().OnBinary(ConstByteSpan(Tone(format.BytesForDuration(milliseconds{500}))));
  ASSERT_TRUE(fx.net.live().receive_paused);

  fx.net.handler().OnText(R"({"type":"clear"})");
  EXPECT_FALSE(fx.net.live().receive_paused);
}

TEST(Playback, RepeatedBargeInsStayConsistent) {
  Fixture fx;
  for (int turn = 0; turn < 5; ++turn) {
    fx.net.handler().OnText(R"({"type":"mark","name":"turn"})");
    const auto audio = Tone(kFrameBytes * 2, static_cast<std::uint8_t>(turn));
    fx.net.handler().OnBinary(ConstByteSpan(audio));
    EXPECT_FALSE(fx.ReadFrame().empty());
    fx.net.handler().OnText(R"({"type":"clear"})");
    EXPECT_TRUE(fx.ReadFrame().empty());
  }
  EXPECT_EQ(fx.session->stats().barge_ins, 5U);
  EXPECT_EQ(fx.events.Count(ForkEventType::kPlaybackCleared), 5U);
}

TEST(Playback, TeardownReportsPlaybackStopAndReleasesSlabs) {
  Fixture fx;
  fx.net.handler().OnBinary(ConstByteSpan(Tone(kFrameBytes * 4)));
  ASSERT_GT(fx.pool->stats().leased_slabs, 0U);

  fx.session->Stop();
  fx.net.handler().OnClosed(/*connect_failed=*/false);
  ASSERT_EQ(fx.session->state(), SessionState::kDead);
  EXPECT_EQ(fx.events.Count(ForkEventType::kPlaybackStop), 1U);
  EXPECT_EQ(fx.pool->stats().leased_slabs, 0U);
}

TEST(Playback, ReadAfterTeardownIsSafeAndSilent) {
  Fixture fx;
  fx.net.handler().OnBinary(ConstByteSpan(Tone(kFrameBytes * 2)));
  fx.session->Stop();
  fx.net.handler().OnClosed(/*connect_failed=*/false);

  // the media bug can still be mid-callback when teardown completes
  for (int i = 0; i < 4; ++i) {
    (void)fx.ReadFrame();
  }
  EXPECT_EQ(fx.session->state(), SessionState::kDead);
}

TEST(Playback, EmptyBinaryFrameIsIgnored) {
  Fixture fx;
  fx.net.handler().OnBinary(ConstByteSpan());
  EXPECT_EQ(fx.session->stats().playback_bytes_received, 0U);
  EXPECT_EQ(fx.events.Count(ForkEventType::kPlaybackStart), 0U);
}

}  // namespace
}  // namespace audiofork
