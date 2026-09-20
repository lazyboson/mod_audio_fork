#include "audiofork_net/ws_client.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#include "audiofork/backoff.hpp"
#include "test_ws_server.hpp"

namespace audiofork::net {
namespace {

using namespace std::chrono_literals;

constexpr std::size_t kDefaultQueueCap = std::size_t{1024} * 1024;

struct RecordingHandler : WsConnectionHandler {
  std::mutex mutex;
  std::condition_variable cv;
  bool connected = false;
  bool closed = false;
  int close_count = 0;
  bool connect_failed = false;
  std::vector<std::string> texts;
  std::vector<std::vector<std::uint8_t>> binaries;

  void OnConnected() override {
    const std::scoped_lock lock(mutex);
    connected = true;
    cv.notify_all();
  }
  void OnText(std::string_view text) override {
    const std::scoped_lock lock(mutex);
    texts.emplace_back(text);
    cv.notify_all();
  }
  void OnBinary(ConstByteSpan bytes) override {
    const std::scoped_lock lock(mutex);
    binaries.emplace_back(bytes.begin(), bytes.end());
    cv.notify_all();
  }
  void OnClosed(bool failed) override {
    const std::scoped_lock lock(mutex);
    closed = true;
    ++close_count;
    connect_failed = failed;
    cv.notify_all();
  }

  template <typename Predicate>
  [[nodiscard]] bool WaitFor(Predicate predicate, std::chrono::milliseconds timeout = 10s) {
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock, timeout, predicate);
  }
};

struct LoopRunner {
  std::unique_ptr<WsEventLoop> loop = WsEventLoop::Create();
  std::thread thread;

  LoopRunner() {
    EXPECT_NE(loop, nullptr);
    thread = std::thread([this] { loop->Run(); });
  }
  ~LoopRunner() {
    loop->Stop();
    thread.join();
  }
};

// The server records on its own thread, so poll rather than sleep a fixed time.
[[nodiscard]] bool WaitForTranscript(const TestWsServer& server, std::size_t entries) {
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (server.transcript().size() >= entries) {
      return true;
    }
    std::this_thread::sleep_for(1ms);
  }
  return false;
}

std::uint16_t FindClosedPort() {
  auto server = TestWsServer::Start();
  EXPECT_NE(server, nullptr);
  return server->port();
}

TEST(WsClient, EchoTextAndBinaryRoundTrip) {
  auto server = TestWsServer::Start();
  ASSERT_NE(server, nullptr);

  std::vector<std::uint8_t> pcm(640);
  std::iota(pcm.begin(), pcm.end(), 0);

  struct SendingHandler : RecordingHandler {
    WsConnection* self = nullptr;
    std::string text_to_send;
    std::vector<std::uint8_t> binary_to_send;
    void OnConnected() override {
      RecordingHandler::OnConnected();
      EXPECT_TRUE(self->SendText(text_to_send));
      EXPECT_TRUE(self->SendBinary(ConstByteSpan(binary_to_send)));
    }
  };
  SendingHandler sender;
  sender.text_to_send = R"({"type":"hello"})";
  sender.binary_to_send = pcm;
  // destroyed before the handler: context teardown must find handlers alive
  LoopRunner runner;
  runner.loop->Post([&] {
    sender.self =
        runner.loop->Connect({"127.0.0.1", server->port(), "/"}, sender, kDefaultQueueCap);
    ASSERT_NE(sender.self, nullptr);
  });

  ASSERT_TRUE(sender.WaitFor([&] { return sender.texts.size() == 1; }));
  EXPECT_EQ(sender.texts[0], R"({"type":"hello"})");
  ASSERT_TRUE(sender.WaitFor([&] { return sender.binaries.size() == 1; }));
  EXPECT_EQ(sender.binaries[0], pcm);
}

TEST(WsClient, LargeBinarySurvivesFragmentation) {
  auto server = TestWsServer::Start();
  ASSERT_NE(server, nullptr);

  std::vector<std::uint8_t> large(std::size_t{200} * 1024);
  for (std::size_t i = 0; i < large.size(); ++i) {
    large[i] = static_cast<std::uint8_t>(i % 251);
  }

  struct SendingHandler : RecordingHandler {
    WsConnection* self = nullptr;
    std::vector<std::uint8_t> payload;
    void OnConnected() override {
      RecordingHandler::OnConnected();
      EXPECT_TRUE(self->SendBinary(ConstByteSpan(payload)));
    }
  };
  SendingHandler sender;
  sender.payload = large;
  LoopRunner runner;
  runner.loop->Post([&] {
    sender.self =
        runner.loop->Connect({"127.0.0.1", server->port(), "/"}, sender, kDefaultQueueCap);
    ASSERT_NE(sender.self, nullptr);
  });

  ASSERT_TRUE(sender.WaitFor([&] { return sender.binaries.size() == 1; }));
  EXPECT_EQ(sender.binaries[0], large);
}

TEST(WsClient, ConnectionRefusedReportsConnectFailure) {
  const std::uint16_t dead_port = FindClosedPort();
  RecordingHandler handler;
  LoopRunner runner;

  runner.loop->Post([&] {
    (void)runner.loop->Connect({"127.0.0.1", dead_port, "/"}, handler, kDefaultQueueCap);
  });
  ASSERT_TRUE(handler.WaitFor([&] { return handler.closed; }));
  EXPECT_TRUE(handler.connect_failed);
  EXPECT_FALSE(handler.connected);
}

TEST(WsClient, ServerDropDeliversOnClosed) {
  auto server = TestWsServer::Start();
  ASSERT_NE(server, nullptr);
  RecordingHandler handler;
  LoopRunner runner;

  runner.loop->Post([&] {
    (void)runner.loop->Connect({"127.0.0.1", server->port(), "/"}, handler, kDefaultQueueCap);
  });
  ASSERT_TRUE(handler.WaitFor([&] { return handler.connected; }));

  server->CloseAllConnections();
  ASSERT_TRUE(handler.WaitFor([&] { return handler.closed; }));
  EXPECT_FALSE(handler.connect_failed);
}

TEST(WsClient, SendQueueCapRejectsExcess) {
  auto server = TestWsServer::Start();
  ASSERT_NE(server, nullptr);

  struct CapHandler : RecordingHandler {
    WsConnection* self = nullptr;
    bool first_send_ok = false;
    bool second_send_rejected = false;
    void OnConnected() override {
      const std::vector<std::uint8_t> chunk(48);
      const bool first = self->SendBinary(ConstByteSpan(chunk));
      const bool second_rejected = !self->SendBinary(ConstByteSpan(chunk));
      {
        const std::scoped_lock lock(mutex);
        first_send_ok = first;
        second_send_rejected = second_rejected;
      }
      // last: publishes the flags-before-connected ordering WaitFor relies on
      RecordingHandler::OnConnected();
    }
  };
  CapHandler handler;
  LoopRunner runner;
  runner.loop->Post([&] {
    handler.self =
        runner.loop->Connect({"127.0.0.1", server->port(), "/"}, handler, /*max_queued_bytes=*/64);
    ASSERT_NE(handler.self, nullptr);
  });

  ASSERT_TRUE(handler.WaitFor([&] { return handler.connected; }));
  EXPECT_TRUE(handler.first_send_ok);
  EXPECT_TRUE(handler.second_send_rejected);
}

TEST(WsClient, ReconnectWithBackoffEventuallySucceeds) {
  const std::uint16_t dead_port = FindClosedPort();
  auto server = TestWsServer::Start();
  ASSERT_NE(server, nullptr);
  std::vector<std::unique_ptr<RecordingHandler>> handlers;
  LoopRunner runner;

  ReconnectBackoff backoff({10ms, 50ms, 2.0, 0.0}, 1);
  std::vector<std::chrono::milliseconds> delays;
  int attempts = 0;

  for (;;) {
    ++attempts;
    const bool use_real_server = attempts > 2;
    const std::uint16_t port = use_real_server ? server->port() : dead_port;

    handlers.push_back(std::make_unique<RecordingHandler>());
    RecordingHandler& handler = *handlers.back();
    runner.loop->Post(
        [&] { (void)runner.loop->Connect({"127.0.0.1", port, "/"}, handler, kDefaultQueueCap); });
    ASSERT_TRUE(handler.WaitFor([&] { return handler.connected || handler.closed; }));

    if (handler.connected) {
      break;
    }
    ASSERT_TRUE(handler.connect_failed);
    ASSERT_LT(attempts, 10);
    const auto delay = backoff.NextDelay();
    delays.push_back(delay);
    std::this_thread::sleep_for(delay);
  }

  EXPECT_EQ(attempts, 3);
  ASSERT_EQ(delays.size(), 2U);
  EXPECT_EQ(delays[0], 10ms);
  EXPECT_EQ(delays[1], 20ms);

  // client-side OnConnected can precede the server thread's ESTABLISHED count
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (server->total_connections() != 1 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  EXPECT_EQ(server->total_connections(), 1);
}

TEST(WsClient, QueuedFrameIsWrittenBeforeTheCloseHandshake) {
  auto server = TestWsServer::Start();
  ASSERT_NE(server, nullptr);

  struct ByeHandler : RecordingHandler {
    WsConnection* self = nullptr;
    void OnConnected() override {
      RecordingHandler::OnConnected();
      EXPECT_TRUE(self->SendText(R"({"type":"bye"})"));
      self->Close();
    }
  };
  ByeHandler sender;
  LoopRunner runner;
  runner.loop->Post([&] {
    sender.self =
        runner.loop->Connect({"127.0.0.1", server->port(), "/"}, sender, kDefaultQueueCap);
    ASSERT_NE(sender.self, nullptr);
  });

  ASSERT_TRUE(WaitForTranscript(*server, 2));
  EXPECT_EQ(server->transcript(), (std::vector<std::string>{R"(text:{"type":"bye"})", "close"}));
  EXPECT_TRUE(sender.WaitFor([&] { return sender.closed; }));
}

TEST(WsClient, EveryQueuedFrameArrivesInOrderBeforeTheClose) {
  auto server = TestWsServer::Start();
  ASSERT_NE(server, nullptr);

  struct BurstHandler : RecordingHandler {
    WsConnection* self = nullptr;
    void OnConnected() override {
      RecordingHandler::OnConnected();
      const std::vector<std::uint8_t> pcm(320, 7);
      EXPECT_TRUE(self->SendText("one"));
      EXPECT_TRUE(self->SendBinary(ConstByteSpan(pcm)));
      EXPECT_TRUE(self->SendText("three"));
      self->Close();
    }
  };
  BurstHandler sender;
  LoopRunner runner;
  runner.loop->Post([&] {
    sender.self =
        runner.loop->Connect({"127.0.0.1", server->port(), "/"}, sender, kDefaultQueueCap);
    ASSERT_NE(sender.self, nullptr);
  });

  ASSERT_TRUE(WaitForTranscript(*server, 4));
  EXPECT_EQ(server->transcript(),
            (std::vector<std::string>{"text:one", "binary:320", "text:three", "close"}));
}

TEST(WsClient, SendAfterCloseIsRefused) {
  auto server = TestWsServer::Start();
  ASSERT_NE(server, nullptr);

  struct RefusingHandler : RecordingHandler {
    WsConnection* self = nullptr;
    bool text_refused = false;
    bool binary_refused = false;
    void OnConnected() override {
      self->Close();
      const std::vector<std::uint8_t> pcm(320);
      const bool text = !self->SendText("late");
      const bool binary = !self->SendBinary(ConstByteSpan(pcm));
      {
        const std::scoped_lock lock(mutex);
        text_refused = text;
        binary_refused = binary;
      }
      // last: publishes the flags-before-connected ordering WaitFor relies on
      RecordingHandler::OnConnected();
    }
  };
  RefusingHandler sender;
  LoopRunner runner;
  runner.loop->Post([&] {
    sender.self =
        runner.loop->Connect({"127.0.0.1", server->port(), "/"}, sender, kDefaultQueueCap);
    ASSERT_NE(sender.self, nullptr);
  });

  ASSERT_TRUE(sender.WaitFor([&] { return sender.connected; }));
  EXPECT_TRUE(sender.text_refused);
  EXPECT_TRUE(sender.binary_refused);
  ASSERT_TRUE(WaitForTranscript(*server, 1));
  EXPECT_EQ(server->transcript(), (std::vector<std::string>{"close"}));
}

TEST(WsClient, CloseBeforeEstablishedDeliversOneConnectFailure) {
  auto server = TestWsServer::Start();
  ASSERT_NE(server, nullptr);
  RecordingHandler handler;
  LoopRunner runner;

  // still in the handshake: Connect returns before the server's reply, so this
  // is the close-while-connecting path, not the close-while-established one
  runner.loop->Post([&] {
    WsConnection* connection =
        runner.loop->Connect({"127.0.0.1", server->port(), "/"}, handler, kDefaultQueueCap);
    ASSERT_NE(connection, nullptr);
    connection->Close();
    connection->Close();
  });

  ASSERT_TRUE(handler.WaitFor([&] { return handler.closed; }));
  EXPECT_TRUE(handler.connect_failed);
  EXPECT_FALSE(handler.connected);
  EXPECT_FALSE(handler.WaitFor([&] { return handler.close_count > 1; }, 500ms));
  EXPECT_EQ(handler.close_count, 1);
}

}  // namespace
}  // namespace audiofork::net
