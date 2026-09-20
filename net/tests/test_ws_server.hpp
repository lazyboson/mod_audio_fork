#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct lws;
struct lws_context;

namespace audiofork::net {

// Test-only echo server: every text/binary message is echoed back on the same
// connection. Runs its own lws context on its own thread; the public methods
// are thread-safe and post onto that thread.
class TestWsServer {
  struct PrivateTag {};

 public:
  [[nodiscard]] static std::unique_ptr<TestWsServer> Start();

  explicit TestWsServer(PrivateTag);
  ~TestWsServer();
  TestWsServer(const TestWsServer&) = delete;
  TestWsServer& operator=(const TestWsServer&) = delete;
  TestWsServer(TestWsServer&&) = delete;
  TestWsServer& operator=(TestWsServer&&) = delete;

  [[nodiscard]] std::uint16_t port() const { return port_; }
  // Queue a text or binary frame to every live connection (playback tests).
  void Broadcast(std::string payload, bool binary);
  [[nodiscard]] int total_connections() const { return total_connections_.load(); }
  // Ordered record of what the server saw across every connection:
  // "text:<payload>", "binary:<byte count>", and "close" when the peer sent a
  // close frame. A peer that resets the socket instead never adds "close".
  [[nodiscard]] std::vector<std::string> transcript() const;
  void CloseAllConnections();

  [[nodiscard]] int HandleLws(lws* wsi, int reason, void* in, std::size_t len);

 private:
  struct PerConnection {
    std::vector<std::uint8_t> incoming;
    std::deque<std::pair<std::vector<std::uint8_t>, bool>> outgoing;
  };

  void Post(std::function<void()> task);
  void DrainPosted();
  void Record(std::string entry);

  lws_context* context_ = nullptr;
  std::uint16_t port_ = 0;
  std::atomic<bool> stop_{false};
  std::atomic<int> total_connections_{0};
  std::mutex posted_mutex_;
  std::vector<std::function<void()>> posted_;
  mutable std::mutex transcript_mutex_;
  std::vector<std::string> transcript_;
  std::unordered_map<lws*, PerConnection> connections_;
  std::thread thread_;
};

}  // namespace audiofork::net
