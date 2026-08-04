#include "audiofork_net/shard_pool.hpp"

#include <gtest/gtest.h>

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

ForkSession::Tuning MakeTuning() {
  const AudioFormat format{16000, 2};
  ForkSession::Tuning tuning;
  tuning.send_cap_bytes = format.BytesForDuration(1000ms);
  tuning.handoff_bytes = format.BytesForDuration(500ms);
  tuning.coalesce_max_bytes = format.BytesForDuration(100ms);
  tuning.drain_timeout = 500ms;
  tuning.backoff = {20ms, 100ms, 2.0, 0.0};
  return tuning;
}

ForkParams MakeParams(std::uint16_t port, std::string fork_id) {
  ForkParams params;
  params.call_uuid = "call-e2e";
  params.fork_id = std::move(fork_id);
  params.endpoint = {"127.0.0.1", port, "/", false};
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

  explicit PoolFixture(std::size_t shard_count = 2) {
    ModuleConfig config;
    config.shard_count = shard_count;
    pool = ShardPool::Start(config, *slabs);
    EXPECT_NE(pool, nullptr);
  }
};

TEST(ShardPool, StartsRequestedShardCount) {
  PoolFixture fx(3);
  EXPECT_EQ(fx.pool->shard_count(), 3U);
  EXPECT_EQ(fx.pool->active_forks(), 0U);
}

TEST(ShardPool, ForkStreamsAudioThroughRealSocketsAndTearsDownCleanly) {
  auto server = TestWsServer::Start();
  ASSERT_NE(server, nullptr);
  PoolFixture fx;

  auto session =
      fx.pool->StartFork(MakeParams(server->port(), "fork-a"), MakeTuning(), fx.events, fx.clock);
  ASSERT_NE(session, nullptr);
  ASSERT_TRUE(WaitUntil([&] { return session->state() == SessionState::kActive; }));
  EXPECT_EQ(fx.events.Count(ForkEventType::kConnect), 1U);

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

}  // namespace
}  // namespace audiofork::net
