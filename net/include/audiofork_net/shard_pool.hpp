#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "audiofork/config.hpp"
#include "audiofork/fork_session.hpp"
#include "audiofork/ports.hpp"
#include "audiofork/slab_pool.hpp"
#include "audiofork_net/ws_client.hpp"

namespace audiofork::net {

// Adapts one WsEventLoop to the core NetPort contract.
class LwsNetPort : public NetPort {
 public:
  explicit LwsNetPort(WsEventLoop& loop) : loop_(loop) {}

  [[nodiscard]] NetConnection* Connect(const Endpoint& endpoint, NetHandler& handler,
                                       std::size_t max_queued_bytes) override;
  void Post(std::function<void()> task) override;
  void ScheduleTimer(std::chrono::milliseconds delay, std::function<void()> task) override;

 private:
  WsEventLoop& loop_;
  std::vector<std::unique_ptr<NetConnection>> adapters_;
};

// One shard: a thread, an lws context, and the sessions pinned to it. Sessions
// are pumped from the loop's tick, so the media thread never has to wake it.
class Shard {
 public:
  [[nodiscard]] static std::unique_ptr<Shard> Start(std::chrono::milliseconds tick);
  ~Shard();
  Shard(const Shard&) = delete;
  Shard& operator=(const Shard&) = delete;
  Shard(Shard&&) = delete;
  Shard& operator=(Shard&&) = delete;

  Shard() = default;

  void Adopt(std::shared_ptr<ForkSession> session);
  [[nodiscard]] std::size_t load() const { return load_.load(std::memory_order_relaxed); }
  [[nodiscard]] NetPort& net() { return *port_; }

 private:
  void Tick();

  std::unique_ptr<WsEventLoop> loop_;
  std::unique_ptr<LwsNetPort> port_;
  std::vector<std::shared_ptr<ForkSession>> sessions_;
  std::atomic<std::size_t> load_{0};
  std::thread thread_;
};

// Owns every shard plus the shared slab pool: the module's composition root
// holds exactly one of these (CONSTITUTION Article 4).
class ShardPool {
 public:
  [[nodiscard]] static std::unique_ptr<ShardPool> Start(const ModuleConfig& config, SlabPool pool);

  [[nodiscard]] std::shared_ptr<ForkSession> StartFork(ForkParams params,
                                                       const ForkSession::Tuning& tuning,
                                                       EventSink& events, Clock& clock);
  [[nodiscard]] std::size_t shard_count() const { return shards_.size(); }
  [[nodiscard]] std::size_t active_forks() const;
  [[nodiscard]] SlabPool& pool() { return pool_; }

  explicit ShardPool(SlabPool pool) : pool_(std::move(pool)) {}

 private:
  [[nodiscard]] Shard& LeastLoadedShard();

  SlabPool pool_;
  std::vector<std::unique_ptr<Shard>> shards_;
  std::atomic<std::uint64_t> next_fork_id_{0};
};

}  // namespace audiofork::net
