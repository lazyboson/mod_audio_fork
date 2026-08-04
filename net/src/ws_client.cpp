#include "audiofork_net/ws_client.hpp"

#include <libwebsockets.h>

#include <algorithm>
#include <array>
#include <mutex>
#include <utility>

namespace audiofork::net {
namespace {

constexpr const char* kProtocolName = "audiofork";
constexpr std::size_t kRxBufferBytes = std::size_t{64} * 1024;

int RawLwsCallback(lws* wsi, lws_callback_reasons reason, void* user, void* in, std::size_t len) {
  auto* loop = static_cast<WsEventLoop*>(lws_context_user(lws_get_context(wsi)));
  if (loop == nullptr) {
    return 0;
  }
  return loop->HandleLws(wsi, static_cast<int>(reason), user, in, len);
}

constexpr std::array<lws_protocols, 2> kProtocols{{
    {kProtocolName, RawLwsCallback, 0, kRxBufferBytes, 0, nullptr, 0},
    LWS_PROTOCOL_LIST_TERM,
}};

}  // namespace

// lws_sul entries are intrusive: the callback recovers the owning handle with
// lws_container_of, so the sul must be the first member and the handle must
// outlive every scheduled firing.
struct TickHandle {
  lws_sorted_usec_list_t sul{};
  WsEventLoop* loop = nullptr;
  lws_context* context = nullptr;
  lws_usec_t interval_us = 0;
};

namespace {

void OnSulTick(lws_sorted_usec_list_t* sul) {
  auto* handle = lws_container_of(sul, TickHandle, sul);
  lws_sul_schedule(handle->context, 0, &handle->sul, OnSulTick, handle->interval_us);
  handle->loop->OnTick();
}

}  // namespace

TickHandle* CreateTickHandle(WsEventLoop& loop, lws_context* context,
                             std::chrono::milliseconds interval) {
  auto handle = std::make_unique<TickHandle>();
  handle->loop = &loop;
  handle->context = context;
  handle->interval_us = static_cast<lws_usec_t>(interval.count()) * LWS_US_PER_MS;
  lws_sul_schedule(context, 0, &handle->sul, OnSulTick, handle->interval_us);
  return handle.release();
}

void DestroyTickHandle(TickHandle* handle) {
  if (handle == nullptr) {
    return;
  }
  lws_sul_cancel(&handle->sul);
  delete handle;
}

void EnsureLwsLogPolicy() {
  // lws_set_log_level writes lws globals; a magic static makes the write happen
  // exactly once, before any lws service thread this process spawns can read them
  static const bool configured = [] {
    lws_set_log_level(LLL_ERR | LLL_WARN, nullptr);
    return true;
  }();
  (void)configured;
}

WsConnection::WsConnection(PrivateTag, WsConnectionHandler& handler, std::size_t max_queued_bytes)
    : handler_(handler), max_queued_bytes_(max_queued_bytes) {}

bool WsConnection::SendText(std::string_view text) {
  return Enqueue(ConstByteSpan(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()),
                 /*binary=*/false);
}

bool WsConnection::SendBinary(ConstByteSpan bytes) { return Enqueue(bytes, /*binary=*/true); }

void WsConnection::Close() {
  if (closed_delivered_ || close_requested_) {
    return;
  }
  close_requested_ = true;
  if (wsi_ != nullptr && established_) {
    lws_callback_on_writable(wsi_);
  } else if (wsi_ != nullptr) {
    // connect still in flight: force lws to give up on it now
    lws_set_timeout(wsi_, PENDING_TIMEOUT_CLOSE_SEND, LWS_TO_KILL_ASYNC);
  }
}

bool WsConnection::Enqueue(ConstByteSpan bytes, bool binary) {
  if (!established_ || close_requested_ || closed_delivered_) {
    return false;
  }
  if (queued_bytes_ + bytes.size() > max_queued_bytes_) {
    return false;
  }
  Outgoing message;
  message.binary = binary;
  message.padded_payload.resize(LWS_PRE + bytes.size());
  std::copy(bytes.begin(), bytes.end(), message.padded_payload.begin() + LWS_PRE);
  outgoing_.push_back(std::move(message));
  queued_bytes_ += bytes.size();
  lws_callback_on_writable(wsi_);
  return true;
}

std::unique_ptr<WsEventLoop> WsEventLoop::Create() {
  EnsureLwsLogPolicy();
  auto loop = std::make_unique<WsEventLoop>(PrivateTag{});
  lws_context_creation_info info{};
  info.port = CONTEXT_PORT_NO_LISTEN;
  info.protocols = kProtocols.data();
  info.user = loop.get();
  loop->context_ = lws_create_context(&info);
  if (loop->context_ == nullptr) {
    return nullptr;
  }
  return loop;
}

WsEventLoop::WsEventLoop(PrivateTag) {}

WsEventLoop::~WsEventLoop() {
  // the tick's sul lives on a list owned by the context, so it must be
  // cancelled BEFORE the context is destroyed
  tick_.reset();
  if (context_ != nullptr) {
    // fires LWS_CALLBACK_CLIENT_CLOSED for every live wsi, so handlers still
    // receive OnClosed during destruction
    lws_context_destroy(context_);
  }
  connections_.clear();
}

void WsEventLoop::Run() {
  while (!stop_.load(std::memory_order_acquire)) {
    lws_service(context_, 0);
    DrainPosted();
    DrainDueTimers();
  }
}

void WsEventLoop::SetTick(std::chrono::milliseconds interval, std::function<void()> on_tick) {
  tick_interval_ = std::max(interval, std::chrono::milliseconds{1});
  on_tick_ = std::move(on_tick);
  tick_.reset(CreateTickHandle(*this, context_, tick_interval_));
}

void WsEventLoop::ScheduleTimer(std::chrono::milliseconds delay, std::function<void()> task) {
  timers_.push_back(PendingTimer{std::chrono::steady_clock::now() + delay, std::move(task)});
}

void WsEventLoop::OnTick() {
  DrainDueTimers();
  if (on_tick_) {
    on_tick_();
  }
}

void WsEventLoop::DrainDueTimers() {
  if (timers_.empty()) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  std::vector<std::function<void()>> due;
  auto partition =
      std::stable_partition(timers_.begin(), timers_.end(),
                            [now](const PendingTimer& timer) { return timer.deadline > now; });
  for (auto it = partition; it != timers_.end(); ++it) {
    due.push_back(std::move(it->task));
  }
  timers_.erase(partition, timers_.end());
  // run outside the container: a task may schedule another timer
  for (auto& task : due) {
    task();
  }
}

void WsEventLoop::Stop() {
  stop_.store(true, std::memory_order_release);
  lws_cancel_service(context_);
}

void WsEventLoop::Post(std::function<void()> task) {
  {
    const std::scoped_lock lock(posted_mutex_);
    posted_.push_back(std::move(task));
  }
  lws_cancel_service(context_);
}

WsConnection* WsEventLoop::Connect(const WsEndpoint& endpoint, WsConnectionHandler& handler,
                                   std::size_t max_queued_bytes) {
  auto owned =
      std::make_unique<WsConnection>(WsConnection::PrivateTag{}, handler, max_queued_bytes);
  WsConnection* connection = owned.get();

  lws_client_connect_info info{};
  info.context = context_;
  info.address = endpoint.host.c_str();
  info.port = endpoint.port;
  info.path = endpoint.path.c_str();
  info.host = endpoint.host.c_str();
  info.origin = endpoint.host.c_str();
  info.protocol = kProtocolName;
  info.userdata = connection;
  info.pwsi = &connection->wsi_;

  if (lws_client_connect_via_info(&info) == nullptr || connection->closed_delivered_) {
    return nullptr;
  }
  connections_[connection] = std::move(owned);
  return connection;
}

int WsEventLoop::HandleLws(lws* wsi, int reason, void* user, void* in, std::size_t len) {
  auto* connection = static_cast<WsConnection*>(user);
  switch (reason) {
    case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
      DrainPosted();
      return 0;

    case LWS_CALLBACK_CLIENT_ESTABLISHED:
      if (connection != nullptr) {
        connection->wsi_ = wsi;
        connection->established_ = true;
        connection->handler_.OnConnected();
      }
      return 0;

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
      if (connection != nullptr) {
        FinishConnection(*connection, /*connect_failed=*/true);
      }
      return -1;

    case LWS_CALLBACK_CLIENT_RECEIVE: {
      if (connection == nullptr) {
        return 0;
      }
      const auto* bytes = static_cast<const std::uint8_t*>(in);
      if (lws_is_first_fragment(wsi) != 0) {
        connection->incoming_.clear();
      }
      connection->incoming_.insert(connection->incoming_.end(), bytes, bytes + len);
      if (lws_is_final_fragment(wsi) != 0) {
        if (lws_frame_is_binary(wsi) != 0) {
          connection->handler_.OnBinary(ConstByteSpan(connection->incoming_));
        } else {
          connection->handler_.OnText(
              std::string_view(reinterpret_cast<const char*>(connection->incoming_.data()),
                               connection->incoming_.size()));
        }
        connection->incoming_.clear();
      }
      return 0;
    }

    case LWS_CALLBACK_CLIENT_WRITEABLE: {
      if (connection == nullptr) {
        return 0;
      }
      if (connection->close_requested_) {
        lws_close_reason(wsi, LWS_CLOSE_STATUS_NORMAL, nullptr, 0);
        return -1;
      }
      if (!connection->outgoing_.empty()) {
        WsConnection::Outgoing& front = connection->outgoing_.front();
        const std::size_t payload_len = front.padded_payload.size() - LWS_PRE;
        const int written = lws_write(wsi, front.padded_payload.data() + LWS_PRE, payload_len,
                                      front.binary ? LWS_WRITE_BINARY : LWS_WRITE_TEXT);
        if (written < static_cast<int>(payload_len)) {
          return -1;
        }
        connection->queued_bytes_ -= payload_len;
        connection->outgoing_.pop_front();
        if (!connection->outgoing_.empty()) {
          lws_callback_on_writable(wsi);
        }
      }
      return 0;
    }

    case LWS_CALLBACK_CLIENT_CLOSED:
      if (connection != nullptr) {
        FinishConnection(*connection, /*connect_failed=*/false);
      }
      return 0;

    default:
      return 0;
  }
}

void WsEventLoop::DrainPosted() {
  std::vector<std::function<void()>> tasks;
  {
    const std::scoped_lock lock(posted_mutex_);
    tasks.swap(posted_);
  }
  for (auto& task : tasks) {
    task();
  }
}

void WsEventLoop::FinishConnection(WsConnection& connection, bool connect_failed) {
  if (!connection.closed_delivered_) {
    connection.closed_delivered_ = true;
    connection.handler_.OnClosed(connect_failed);
  }
  connections_.erase(&connection);
}

}  // namespace audiofork::net
