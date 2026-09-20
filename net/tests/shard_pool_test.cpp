#include "audiofork_net/shard_pool.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <numeric>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "test_ws_server.hpp"

namespace audiofork::net {
namespace {

using namespace std::chrono_literals;

class ThreadSafeEvents : public EventSink {
 public:
  void Emit(const ForkEvent& event) override {
    const std::scoped_lock lock(mutex_);
    types_.push_back(event.type);
  }

  [[nodiscard]] std::size_t Count(ForkEventType type) const {
    const std::scoped_lock lock(mutex_);
    std::size_t count = 0;
    for (const ForkEventType seen : types_) {
      count += static_cast<std::size_t>(seen == type);
    }
    return count;
  }

 private:
  mutable std::mutex mutex_;
  std::vector<ForkEventType> types_;
};

ForkSession::Tuning MakeTuning(bool playback = false) {
  const AudioFormat format{16000, 2};
  ForkSession::Tuning tuning;
  tuning.send_cap_bytes = format.BytesForDuration(1000ms);
  tuning.handoff_bytes = format.BytesForDuration(500ms);
  tuning.coalesce_max_bytes = format.BytesForDuration(100ms);
  tuning.drain_timeout = 500ms;
  tuning.backoff = {20ms, 100ms, 2.0, 0.0};
  if (playback) {
    const AudioFormat mono{16000, 1};
    tuning.playback_high_watermark_bytes = mono.BytesForDuration(1000ms);
    tuning.playback_low_watermark_bytes = mono.BytesForDuration(200ms);
    tuning.playback_handoff_bytes = mono.BytesForDuration(200ms);
  }
  return tuning;
}

ForkParams MakeParams(std::uint16_t port, std::string fork_id, bool tls = false) {
  ForkParams params;
  params.call_uuid = "call-e2e";
  params.fork_id = std::move(fork_id);
  params.endpoint = {"127.0.0.1", port, "/", tls};
  params.format = {16000, 2};
  return params;
}

template <typename Predicate>
[[nodiscard]] bool WaitUntil(Predicate predicate, std::chrono::milliseconds timeout = 10s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(2ms);
  }
  return predicate();
}

struct PoolFixture {
  ThreadSafeEvents events;
  SystemClock clock;
  std::optional<SlabPool> slabs = SlabPool::Create({4096, std::size_t{4096} * 512});
  std::unique_ptr<ShardPool> pool;

  explicit PoolFixture(std::size_t shard_count = 2, const TlsOptions& tls = {}) {
    ModuleConfig config;
    config.shard_count = shard_count;
    config.tls = tls;
    pool = ShardPool::Start(config, *slabs);
    EXPECT_NE(pool, nullptr);
  }
};

TEST(ShardPool, StartsRequestedShardCount) {
  PoolFixture fx(3);
  EXPECT_EQ(fx.pool->shard_count(), 3U);
  EXPECT_EQ(fx.pool->active_forks(), 0U);
}

TEST(ShardPool, ShardLoadsReportOneEntryPerShardAndSumToTheActiveForks) {
  auto server = TestWsServer::Start();
  ASSERT_NE(server, nullptr);
  PoolFixture fx(3);
  EXPECT_EQ(fx.pool->shard_loads(), std::vector<std::size_t>(3, 0U));

  constexpr int kForks = 6;
  std::vector<std::shared_ptr<ForkSession>> sessions;
  for (int i = 0; i < kForks; ++i) {
    auto session = fx.pool->StartFork(MakeParams(server->port(), "load-" + std::to_string(i)),
                                      MakeTuning(), fx.events, fx.clock);
    ASSERT_NE(session, nullptr);
    sessions.push_back(std::move(session));
  }
  ASSERT_TRUE(WaitUntil([&] { return fx.pool->active_forks() == kForks; }));

  const std::vector<std::size_t> loads = fx.pool->shard_loads();
  ASSERT_EQ(loads.size(), 3U);
  EXPECT_EQ(std::accumulate(loads.begin(), loads.end(), std::size_t{0}), fx.pool->active_forks());
  // least-loaded placement, so no shard may carry more than its even share
  EXPECT_EQ(*std::max_element(loads.begin(), loads.end()), kForks / 3);

  for (auto& session : sessions) {
    session->Stop();
  }
  ASSERT_TRUE(WaitUntil([&] { return fx.pool->active_forks() == 0U; }));
  EXPECT_EQ(fx.pool->shard_loads(), std::vector<std::size_t>(3, 0U));
}

TEST(ShardPool, ForkStreamsAudioThroughRealSocketsAndTearsDownCleanly) {
  auto server = TestWsServer::Start();
  ASSERT_NE(server, nullptr);
  PoolFixture fx;

  auto session =
      fx.pool->StartFork(MakeParams(server->port(), "fork-a"), MakeTuning(), fx.events, fx.clock);
  ASSERT_NE(session, nullptr);
  // wait on the event, not the state: the state machine reaches kActive just
  // before the connect event is emitted
  ASSERT_TRUE(WaitUntil([&] { return fx.events.Count(ForkEventType::kConnect) == 1U; }));
  EXPECT_EQ(session->state(), SessionState::kActive);

  std::vector<std::uint8_t> pcm(1280);
  std::iota(pcm.begin(), pcm.end(), 0);
  for (int frame = 0; frame < 10; ++frame) {
    EXPECT_TRUE(session->PushAudio(ConstByteSpan(pcm)));
  }
  ASSERT_TRUE(WaitUntil([&] { return session->stats().sent_bytes >= pcm.size() * 10; }));
  EXPECT_EQ(session->stats().media_dropped_bytes, 0U);
  EXPECT_EQ(session->stats().buffer_dropped_bytes, 0U);

  session->Stop();
  ASSERT_TRUE(WaitUntil([&] { return session->state() == SessionState::kDead; }));
  EXPECT_EQ(fx.events.Count(ForkEventType::kStop), 1U);
  ASSERT_TRUE(WaitUntil([&] { return fx.pool->active_forks() == 0U; }));
  EXPECT_EQ(fx.slabs->stats().leased_slabs, 0U);
}

TEST(ShardPool, ForkSurvivesServerDropByReconnecting) {
  auto server = TestWsServer::Start();
  ASSERT_NE(server, nullptr);
  PoolFixture fx(1);

  auto session =
      fx.pool->StartFork(MakeParams(server->port(), "fork-b"), MakeTuning(), fx.events, fx.clock);
  ASSERT_NE(session, nullptr);
  ASSERT_TRUE(WaitUntil([&] { return session->state() == SessionState::kActive; }));

  server->CloseAllConnections();
  ASSERT_TRUE(WaitUntil([&] { return session->stats().reconnects >= 1; }));
  EXPECT_EQ(session->state(), SessionState::kActive);
  EXPECT_GE(fx.events.Count(ForkEventType::kReconnecting), 1U);
  EXPECT_GE(fx.events.Count(ForkEventType::kResume), 1U);
  EXPECT_EQ(server->total_connections(), 2);

  session->Stop();
  ASSERT_TRUE(WaitUntil([&] { return session->state() == SessionState::kDead; }));
}

TEST(ShardPool, DeadEndpointRetriesWithoutLeakingOrWedging) {
  const std::uint16_t dead_port = [] {
    auto probe = TestWsServer::Start();
    EXPECT_NE(probe, nullptr);
    return probe->port();
  }();
  PoolFixture fx(1);

  auto session =
      fx.pool->StartFork(MakeParams(dead_port, "fork-c"), MakeTuning(), fx.events, fx.clock);
  ASSERT_NE(session, nullptr);
  ASSERT_TRUE(WaitUntil([&] { return fx.events.Count(ForkEventType::kConnectFailed) >= 1; }));
  ASSERT_TRUE(WaitUntil([&] { return fx.events.Count(ForkEventType::kReconnecting) >= 3; }));

  session->Stop();
  ASSERT_TRUE(WaitUntil([&] { return session->state() == SessionState::kDead; }));
  ASSERT_TRUE(WaitUntil([&] { return fx.pool->active_forks() == 0U; }));
}

TEST(ShardPool, ManyForksSpreadAcrossShardsAndAllRetire) {
  auto server = TestWsServer::Start();
  ASSERT_NE(server, nullptr);
  PoolFixture fx(4);

  constexpr int kForks = 24;
  std::vector<std::shared_ptr<ForkSession>> sessions;
  sessions.reserve(kForks);
  for (int i = 0; i < kForks; ++i) {
    auto session = fx.pool->StartFork(MakeParams(server->port(), "fork-" + std::to_string(i)),
                                      MakeTuning(), fx.events, fx.clock);
    ASSERT_NE(session, nullptr);
    sessions.push_back(std::move(session));
  }
  ASSERT_TRUE(WaitUntil([&] {
    return fx.events.Count(ForkEventType::kConnect) == static_cast<std::size_t>(kForks);
  }));
  EXPECT_EQ(fx.pool->active_forks(), static_cast<std::size_t>(kForks));

  std::vector<std::uint8_t> pcm(640);
  std::iota(pcm.begin(), pcm.end(), 3);
  for (auto& session : sessions) {
    EXPECT_TRUE(session->PushAudio(ConstByteSpan(pcm)));
  }
  ASSERT_TRUE(WaitUntil([&] {
    return std::all_of(sessions.begin(), sessions.end(),
                       [&](const std::shared_ptr<ForkSession>& session) {
                         return session->stats().sent_bytes >= pcm.size();
                       });
  }));

  for (auto& session : sessions) {
    session->Stop();
  }
  ASSERT_TRUE(WaitUntil([&] { return fx.pool->active_forks() == 0U; }));
  EXPECT_EQ(fx.slabs->stats().leased_slabs, 0U);
}

TEST(ShardPool, PlaybackAudioArrivesFromTheServerByteExact) {
  auto server = TestWsServer::Start();
  ASSERT_NE(server, nullptr);
  PoolFixture fx(1);

  auto session = fx.pool->StartFork(MakeParams(server->port(), "fork-pb"), MakeTuning(true),
                                    fx.events, fx.clock);
  ASSERT_NE(session, nullptr);
  ASSERT_TRUE(WaitUntil([&] { return fx.events.Count(ForkEventType::kConnect) == 1U; }));

  // the echo server returns whatever we send, so forked audio comes back as
  // playback audio: a full round trip through two real sockets
  std::vector<std::uint8_t> pcm(1280);
  std::iota(pcm.begin(), pcm.end(), 11);
  EXPECT_TRUE(session->PushAudio(ConstByteSpan(pcm)));

  ASSERT_TRUE(WaitUntil([&] { return session->stats().playback_bytes_received >= pcm.size(); }));

  std::vector<std::uint8_t> played;
  ASSERT_TRUE(WaitUntil([&] {
    std::vector<std::uint8_t> frame(320);
    const std::size_t filled = session->ReadPlayback(MutableByteSpan(frame));
    played.insert(played.end(), frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(filled));
    return played.size() >= pcm.size();
  }));
  EXPECT_EQ(played, pcm);

  session->Stop();
  ASSERT_TRUE(WaitUntil([&] { return session->state() == SessionState::kDead; }));
  EXPECT_EQ(fx.slabs->stats().leased_slabs, 0U);
}

TEST(ShardPool, BargeInOverRealSocketsSilencesPlaybackImmediately) {
  auto server = TestWsServer::Start();
  ASSERT_NE(server, nullptr);
  PoolFixture fx(1);

  auto session = fx.pool->StartFork(MakeParams(server->port(), "fork-barge"), MakeTuning(true),
                                    fx.events, fx.clock);
  ASSERT_NE(session, nullptr);
  ASSERT_TRUE(WaitUntil([&] { return fx.events.Count(ForkEventType::kConnect) == 1U; }));

  std::vector<std::uint8_t> pcm(4096);
  std::iota(pcm.begin(), pcm.end(), 3);
  EXPECT_TRUE(session->PushAudio(ConstByteSpan(pcm)));
  ASSERT_TRUE(WaitUntil([&] { return session->stats().playback_bytes_received >= pcm.size(); }));

  server->Broadcast(R"({"type":"clear"})", /*binary=*/false);
  ASSERT_TRUE(WaitUntil([&] { return session->stats().barge_ins == 1U; }));

  // after the clear the very next frames must be silent
  for (int i = 0; i < 3; ++i) {
    std::vector<std::uint8_t> frame(320);
    EXPECT_EQ(session->ReadPlayback(MutableByteSpan(frame)), 0U);
  }
  EXPECT_EQ(fx.events.Count(ForkEventType::kPlaybackCleared), 1U);

  session->Stop();
  ASSERT_TRUE(WaitUntil([&] { return session->state() == SessionState::kDead; }));
}

TEST(ShardPool, StopIsSafeFromAnotherThreadWhileAudioIsFlowing) {
  auto server = TestWsServer::Start();
  ASSERT_NE(server, nullptr);
  PoolFixture fx(2);

  auto session = fx.pool->StartFork(MakeParams(server->port(), "fork-race"), MakeTuning(),
                                    fx.events, fx.clock);
  ASSERT_NE(session, nullptr);
  ASSERT_TRUE(WaitUntil([&] { return session->state() == SessionState::kActive; }));

  std::atomic<bool> pushing{true};
  std::thread media([&] {
    std::vector<std::uint8_t> pcm(320);
    while (pushing.load(std::memory_order_acquire)) {
      (void)session->PushAudio(ConstByteSpan(pcm));
      std::this_thread::sleep_for(1ms);
    }
  });

  std::this_thread::sleep_for(30ms);
  session->Stop();
  ASSERT_TRUE(WaitUntil([&] { return session->state() == SessionState::kDead; }));
  pushing.store(false, std::memory_order_release);
  media.join();
  EXPECT_EQ(fx.slabs->stats().leased_slabs, 0U);
}

TEST(ShardPool, SendTextAndDtmfReachTheServerFromAnotherThread) {
  auto server = TestWsServer::Start();
  ASSERT_NE(server, nullptr);
  PoolFixture fx(2);

  auto session = fx.pool->StartFork(MakeParams(server->port(), "fork-text"), MakeTuning(),
                                    fx.events, fx.clock);
  ASSERT_NE(session, nullptr);
  ASSERT_TRUE(WaitUntil([&] { return fx.events.Count(ForkEventType::kConnect) == 1U; }));

  constexpr int kRounds = 25;
  std::atomic<int> accepted{0};
  std::thread control([&] {
    for (int round = 0; round < kRounds; ++round) {
      const std::string text = R"({"n":)" + std::to_string(round) + "}";
      accepted.fetch_add(static_cast<int>(session->SendText(text)), std::memory_order_relaxed);
      accepted.fetch_add(static_cast<int>(session->SendDtmf('1', 160)), std::memory_order_relaxed);
    }
  });
  control.join();
  EXPECT_EQ(accepted.load(std::memory_order_relaxed), 2 * kRounds);

  // the server echoes every text frame back, so each one returns as a json
  // event — as does the echo of our own hello
  ASSERT_TRUE(WaitUntil([&] {
    return fx.events.Count(ForkEventType::kJson) >= static_cast<std::size_t>(2 * kRounds) + 1U;
  }));
  EXPECT_EQ(session->stats().pending_texts_dropped, 0U);

  session->Stop();
  ASSERT_TRUE(WaitUntil([&] { return session->state() == SessionState::kDead; }));
  EXPECT_EQ(fx.slabs->stats().leased_slabs, 0U);
}

TEST(ShardPool, ForkOverWssStreamsAudioAndText) {
  TestWsTls server_tls{TestTlsPath("server.pem"), TestTlsPath("server.key"), ""};
  auto server = TestWsServer::Start(server_tls);
  ASSERT_NE(server, nullptr);

  TlsOptions tls;
  tls.ca_file = TestTlsPath("ca.pem");
  PoolFixture fx(1, tls);

  ForkParams params = MakeParams(server->port(), "fork-wss", true);
  params.metadata_json = R"({"type":"hello"})";
  auto session = fx.pool->StartFork(params, MakeTuning(), fx.events, fx.clock);
  ASSERT_NE(session, nullptr);
  ASSERT_TRUE(WaitUntil([&] { return fx.events.Count(ForkEventType::kConnect) == 1U; }));

  std::vector<std::uint8_t> pcm(1280);
  std::iota(pcm.begin(), pcm.end(), 0);
  for (int frame = 0; frame < 10; ++frame) {
    EXPECT_TRUE(session->PushAudio(ConstByteSpan(pcm)));
  }
  ASSERT_TRUE(WaitUntil([&] { return session->stats().sent_bytes >= pcm.size() * 10; }));

  EXPECT_TRUE(session->SendText(R"({"n":1})"));
  ASSERT_TRUE(WaitUntil([&] { return fx.events.Count(ForkEventType::kJson) >= 2U; }));
  EXPECT_EQ(fx.events.Count(ForkEventType::kConnectFailed), 0U);

  session->Stop();
  ASSERT_TRUE(WaitUntil([&] { return session->state() == SessionState::kDead; }));
  EXPECT_EQ(fx.slabs->stats().leased_slabs, 0U);
}

}  // namespace
}  // namespace audiofork::net
