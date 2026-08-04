#include "test_ws_server.hpp"

#include <libwebsockets.h>

#include <array>
#include <utility>

#include "audiofork_net/ws_client.hpp"

namespace audiofork::net {
namespace {

constexpr const char* kProtocolName = "audiofork";
constexpr std::size_t kRxBufferBytes = std::size_t{64} * 1024;

int RawServerCallback(lws* wsi, lws_callback_reasons reason, void* /*user*/, void* in,
                      std::size_t len) {
  auto* server = static_cast<TestWsServer*>(lws_context_user(lws_get_context(wsi)));
  if (server == nullptr) {
    return 0;
  }
  return server->HandleLws(wsi, static_cast<int>(reason), in, len);
}

constexpr std::array<lws_protocols, 2> kProtocols{{
    {kProtocolName, RawServerCallback, 0, kRxBufferBytes, 0, nullptr, 0},
    LWS_PROTOCOL_LIST_TERM,
}};

}  // namespace

std::unique_ptr<TestWsServer> TestWsServer::Start() {
  EnsureLwsLogPolicy();
  auto server = std::make_unique<TestWsServer>(PrivateTag{});
  lws_context_creation_info info{};
  info.port = 0;
  info.iface = "127.0.0.1";
  info.protocols = kProtocols.data();
  info.user = server.get();
  info.options = LWS_SERVER_OPTION_EXPLICIT_VHOSTS;
  server->context_ = lws_create_context(&info);
  if (server->context_ == nullptr) {
    return nullptr;
  }
  lws_vhost* vhost = lws_create_vhost(server->context_, &info);
  if (vhost == nullptr) {
    return nullptr;
  }
  server->port_ = static_cast<std::uint16_t>(lws_get_vhost_listen_port(vhost));

  TestWsServer* raw = server.get();
  server->thread_ = std::thread([raw] {
    while (!raw->stop_.load(std::memory_order_acquire)) {
      lws_service(raw->context_, 0);
      raw->DrainPosted();
    }
  });
  return server;
}

TestWsServer::TestWsServer(PrivateTag) {}

TestWsServer::~TestWsServer() {
  if (thread_.joinable()) {
    stop_.store(true, std::memory_order_release);
    lws_cancel_service(context_);
    thread_.join();
  }
  if (context_ != nullptr) {
    lws_context_destroy(context_);
  }
}

void TestWsServer::CloseAllConnections() {
  Post([this] {
    for (auto& [wsi, state] : connections_) {
      lws_set_timeout(wsi, PENDING_TIMEOUT_CLOSE_SEND, LWS_TO_KILL_ASYNC);
    }
  });
}

void TestWsServer::Post(std::function<void()> task) {
  {
    const std::scoped_lock lock(posted_mutex_);
    posted_.push_back(std::move(task));
  }
  lws_cancel_service(context_);
}

void TestWsServer::DrainPosted() {
  std::vector<std::function<void()>> tasks;
  {
    const std::scoped_lock lock(posted_mutex_);
    tasks.swap(posted_);
  }
  for (auto& task : tasks) {
    task();
  }
}

int TestWsServer::HandleLws(lws* wsi, int reason, void* in, std::size_t len) {
  switch (reason) {
    case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
      DrainPosted();
      return 0;

    case LWS_CALLBACK_ESTABLISHED:
      connections_[wsi] = PerConnection{};
      total_connections_.fetch_add(1);
      return 0;

    case LWS_CALLBACK_RECEIVE: {
      auto& state = connections_[wsi];
      const auto* bytes = static_cast<const std::uint8_t*>(in);
      if (lws_is_first_fragment(wsi) != 0) {
        state.incoming.clear();
      }
      state.incoming.insert(state.incoming.end(), bytes, bytes + len);
      if (lws_is_final_fragment(wsi) != 0) {
        std::vector<std::uint8_t> echo(LWS_PRE);
        echo.insert(echo.end(), state.incoming.begin(), state.incoming.end());
        state.outgoing.emplace_back(std::move(echo), lws_frame_is_binary(wsi) != 0);
        state.incoming.clear();
        lws_callback_on_writable(wsi);
      }
      return 0;
    }

    case LWS_CALLBACK_SERVER_WRITEABLE: {
      auto& state = connections_[wsi];
      if (!state.outgoing.empty()) {
        auto& [payload, binary] = state.outgoing.front();
        const std::size_t payload_len = payload.size() - LWS_PRE;
        const int written = lws_write(wsi, payload.data() + LWS_PRE, payload_len,
                                      binary ? LWS_WRITE_BINARY : LWS_WRITE_TEXT);
        if (written < static_cast<int>(payload_len)) {
          return -1;
        }
        state.outgoing.pop_front();
        if (!state.outgoing.empty()) {
          lws_callback_on_writable(wsi);
        }
      }
      return 0;
    }

    case LWS_CALLBACK_CLOSED:
      connections_.erase(wsi);
      return 0;

    default:
      return 0;
  }
}

}  // namespace audiofork::net
