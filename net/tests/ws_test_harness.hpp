#pragma once

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "audiofork_net/ws_client.hpp"

namespace audiofork::net {

inline constexpr std::size_t kDefaultQueueCap = std::size_t{1024} * 1024;

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
  [[nodiscard]] bool WaitFor(Predicate predicate,
                             std::chrono::milliseconds timeout = std::chrono::seconds{10}) {
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock, timeout, predicate);
  }
};

struct LoopRunner {
  std::unique_ptr<WsEventLoop> loop;
  std::thread thread;

  explicit LoopRunner(const TlsOptions& tls = {}, std::chrono::milliseconds close_drain_timeout =
                                                      std::chrono::milliseconds{5000})
      : loop(WsEventLoop::Create(tls, close_drain_timeout)) {
    EXPECT_NE(loop, nullptr);
    if (loop != nullptr) {
      thread = std::thread([this] { loop->Run(); });
    }
  }
  ~LoopRunner() {
    if (thread.joinable()) {
      loop->Stop();
      thread.join();
    }
  }
  LoopRunner(const LoopRunner&) = delete;
  LoopRunner& operator=(const LoopRunner&) = delete;
  LoopRunner(LoopRunner&&) = delete;
  LoopRunner& operator=(LoopRunner&&) = delete;
};

}  // namespace audiofork::net
