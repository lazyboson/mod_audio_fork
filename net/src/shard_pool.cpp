#include "audiofork_net/shard_pool.hpp"

#include <algorithm>
#include <utility>

namespace audiofork::net {
namespace {

constexpr std::chrono::milliseconds kTickInterval{20};

class WsConnectionAdapter : public NetConnection, public WsConnectionHandler {
 public:
  WsConnectionAdapter(NetHandler& target) : target_(target) {}

  void Bind(WsConnection* connection) { connection_ = connection; }

  [[nodiscard]] bool SendText(std::string_view text) override {
    return connection_ != nullptr && connection_->SendText(text);
  }
  [[nodiscard]] bool SendBinary(ConstByteSpan bytes) override {
    return connection_ != nullptr && connection_->SendBinary(bytes);
  }
  void Close() override {
    if (connection_ != nullptr) {
      connection_->Close();
    }
  }

  void OnConnected() override { target_.OnConnected(); }
  void OnText(std::string_view text) override { target_.OnText(text); }
  void OnBinary(ConstByteSpan bytes) override { target_.OnBinary(bytes); }
  void OnClosed(bool connect_failed) override {
    // the WsConnection dies right after this returns
    connection_ = nullptr;
    target_.OnClosed(connect_failed);
  }

 private:
  NetHandler& target_;
  WsConnection* connection_ = nullptr;
};

}  // namespace

NetConnection* LwsNetPort::Connect(const Endpoint& endpoint, NetHandler& handler,
                                   std::size_t max_queued_bytes) {
  auto adapter = std::make_unique<WsConnectionAdapter>(handler);
  WsConnection* connection = loop_.Connect(WsEndpoint{endpoint.host, endpoint.port, endpoint.path},
                                           *adapter, max_queued_bytes);
  if (connection == nullptr) {
    return nullptr;
  }
  adapter->Bind(connection);
  NetConnection* result = adapter.get();
  // adapters outlive their connection so late lws callbacks stay safe; the
  // shard drops them when its sessions finish
  adapters_.push_back(std::move(adapter));
  return result;
}

void LwsNetPort::Post(std::function<void()> task) { loop_.Post(std::move(task)); }

void LwsNetPort::ScheduleTimer(std::chrono::milliseconds delay, std::function<void()> task) {
  loop_.ScheduleTimer(delay, std::move(task));
}

std::unique_ptr<Shard> Shard::Create(std::chrono::milliseconds tick) {
  auto shard = std::make_unique<Shard>();
  shard->loop_ = WsEventLoop::Create();
  if (shard->loop_ == nullptr) {
    return nullptr;
  }
  shard->tick_ = tick;
  shard->port_ = std::make_unique<LwsNetPort>(*shard->loop_);
  return shard;
}

void Shard::StartThread() {
  Shard* raw = this;
  loop_->SetTick(tick_, [raw] { raw->Tick(); });
  thread_ = std::thread([raw] { raw->loop_->Run(); });
}

void Shard::StopThread() {
  if (thread_.joinable()) {
    loop_->Stop();
    thread_.join();
  }
}

Shard::~Shard() { StopThread(); }

void Shard::Adopt(std::shared_ptr<ForkSession> session) {
  // hop onto the loop thread: sessions_ is loop-owned state
  loop_->Post([this, session = std::move(session)]() mutable {
    sessions_.push_back(session);
    load_.store(sessions_.size(), std::memory_order_relaxed);
    session->Start();
  });
}

void Shard::Tick() {
  for (auto& session : sessions_) {
    session->Pump();
  }
  const auto finished = std::stable_partition(sessions_.begin(), sessions_.end(),
                                              [](const std::shared_ptr<ForkSession>& session) {
                                                return session->state() != SessionState::kDead;
                                              });
  if (finished != sessions_.end()) {
    sessions_.erase(finished, sessions_.end());
    load_.store(sessions_.size(), std::memory_order_relaxed);
  }
}

std::unique_ptr<ShardPool> ShardPool::Start(const ModuleConfig& config, SlabPool pool) {
  auto shard_pool = std::make_unique<ShardPool>(std::move(pool));
  std::size_t count = config.shard_count;
  if (count == 0) {
    const unsigned hardware = std::thread::hardware_concurrency();
    count = hardware == 0 ? 2 : std::min<std::size_t>(hardware, 16);
  }
  for (std::size_t i = 0; i < count; ++i) {
    auto shard = Shard::Create(kTickInterval);
    if (shard == nullptr) {
      return nullptr;
    }
    shard_pool->shards_.push_back(std::move(shard));
  }
  // every context exists before any service thread starts
  for (auto& shard : shard_pool->shards_) {
    shard->StartThread();
  }
  return shard_pool;
}

ShardPool::~ShardPool() {
  // every thread stops before any context is destroyed
  for (auto& shard : shards_) {
    shard->StopThread();
  }
  shards_.clear();
}

std::shared_ptr<ForkSession> ShardPool::StartFork(ForkParams params,
                                                  const ForkSession::Tuning& tuning,
                                                  EventSink& events, Clock& clock) {
  Shard& shard = LeastLoadedShard();
  ForkSession::Tuning seeded = tuning;
  // distinct seeds per fork so 1,000 reconnects do not align
  seeded.backoff_seed = next_fork_id_.fetch_add(1, std::memory_order_relaxed) + 1;
  auto session = ForkSession::Create(std::move(params), seeded, shard.net(), events, clock, pool_);
  if (session == nullptr) {
    return nullptr;
  }
  shard.Adopt(session);
  return session;
}

std::size_t ShardPool::active_forks() const {
  std::size_t total = 0;
  for (const auto& shard : shards_) {
    total += shard->load();
  }
  return total;
}

Shard& ShardPool::LeastLoadedShard() {
  auto it =
      std::min_element(shards_.begin(), shards_.end(),
                       [](const std::unique_ptr<Shard>& lhs, const std::unique_ptr<Shard>& rhs) {
                         return lhs->load() < rhs->load();
                       });
  return **it;
}

}  // namespace audiofork::net
