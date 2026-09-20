#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

struct lws;
struct lws_context;

namespace audiofork::net {

// Absolute path to one of the test-only credentials in tests/tls (see the
// README there).
[[nodiscard]] std::string TestTlsPath(std::string_view name);

// Empty cert_file leaves the server in plaintext. A non-empty client_ca_file
// makes lws demand a client certificate that CA signed.
struct TestWsTls {
  std::string cert_file;
  std::string key_file;
  std::string client_ca_file;
};

// Test-only echo server: every text/binary message is echoed back on the same
// connection. Runs its own lws context on its own thread; the public methods
// are thread-safe and post onto that thread.
class TestWsServer {
  struct PrivateTag {};

 public:
  [[nodiscard]] static std::unique_ptr<TestWsServer> Start(const TestWsTls& tls = {});

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
  void CloseAllConnections();

  [[nodiscard]] int HandleLws(lws* wsi, int reason, void* in, std::size_t len);

 private:
  struct PerConnection {
    std::vector<std::uint8_t> incoming;
    std::deque<std::pair<std::vector<std::uint8_t>, bool>> outgoing;
  };

  void Post(std::function<void()> task);
  void DrainPosted();

  lws_context* context_ = nullptr;
  std::uint16_t port_ = 0;
  std::atomic<bool> stop_{false};
  std::atomic<int> total_connections_{0};
  std::mutex posted_mutex_;
  std::vector<std::function<void()>> posted_;
  std::unordered_map<lws*, PerConnection> connections_;
  std::thread thread_;
};

}  // namespace audiofork::net
