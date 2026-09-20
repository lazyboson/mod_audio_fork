#include "audiofork/config.hpp"

#include <algorithm>
#include <string>

namespace audiofork {
namespace {

constexpr std::size_t kMaxShards = 16;
constexpr std::chrono::milliseconds kMinBuffer{100};
constexpr std::chrono::milliseconds kMaxBuffer{60000};
constexpr std::chrono::milliseconds kMinCoalesce{20};
constexpr std::uint16_t kDefaultTlsPort = 443;
constexpr std::uint16_t kDefaultPlainPort = 80;

[[nodiscard]] bool ParsePort(std::string_view text, std::uint16_t& port) {
  if (text.empty() || text.size() > 5) {
    return false;
  }
  unsigned value = 0;
  for (const char digit : text) {
    if (digit < '0' || digit > '9') {
      return false;
    }
    value = value * 10 + static_cast<unsigned>(digit - '0');
  }
  if (value == 0 || value > 65535) {
    return false;
  }
  port = static_cast<std::uint16_t>(value);
  return true;
}

// Returns false only on a malformed authority; `port_text` is empty when the
// authority carried no colon at all.
[[nodiscard]] bool SplitAuthority(std::string_view authority, std::string_view& host,
                                  std::string_view& port_text, bool& has_port) {
  has_port = false;
  if (!authority.empty() && authority.front() == '[') {
    const std::size_t close = authority.find(']');
    if (close == std::string_view::npos) {
      return false;
    }
    host = authority.substr(1, close - 1);
    const std::string_view rest = authority.substr(close + 1);
    if (rest.empty()) {
      return true;
    }
    if (rest.front() != ':') {
      return false;
    }
    port_text = rest.substr(1);
    has_port = true;
    return true;
  }
  const std::size_t colon = authority.rfind(':');
  if (colon == std::string_view::npos) {
    host = authority;
    return true;
  }
  host = authority.substr(0, colon);
  port_text = authority.substr(colon + 1);
  has_port = true;
  return true;
}

}  // namespace

ModuleConfig SanitizeConfig(ModuleConfig config) {
  config.shard_count = std::min(config.shard_count, kMaxShards);
  config.send_buffer = std::clamp(config.send_buffer, kMinBuffer, kMaxBuffer);
  config.handoff_buffer = std::clamp(config.handoff_buffer, kMinCoalesce, config.send_buffer);
  config.coalesce_max = std::clamp(config.coalesce_max, kMinCoalesce, config.handoff_buffer);
  config.emergency_buffer = std::clamp(config.emergency_buffer, kMinCoalesce, config.send_buffer);
  config.drain_timeout = std::max(config.drain_timeout, std::chrono::milliseconds{0});
  config.playback_high_watermark =
      std::clamp(config.playback_high_watermark, kMinCoalesce, kMaxBuffer);
  config.playback_low_watermark = std::clamp(
      config.playback_low_watermark, std::chrono::milliseconds{0}, config.playback_high_watermark);
  config.reconnect_min = std::max(config.reconnect_min, std::chrono::milliseconds{1});
  config.reconnect_max = std::max(config.reconnect_max, config.reconnect_min);
  config.slab_size_bytes = std::max(config.slab_size_bytes, std::size_t{4} * 1024);
  config.global_memory_cap_bytes =
      std::max(config.global_memory_cap_bytes, config.slab_size_bytes * 16);
  config.max_forks_per_call = std::clamp(config.max_forks_per_call, std::size_t{1}, std::size_t{8});
  // A certificate without its key (or the reverse) cannot produce a client
  // identity, and handing lws half of one fails the whole context: drop back to
  // no client certificate instead.
  if (config.tls.cert_file.empty() != config.tls.key_file.empty()) {
    config.tls.cert_file.clear();
    config.tls.key_file.clear();
  }
  return config;
}

std::optional<Endpoint> ParseWsUrl(std::string_view url, std::string& error) {
  Endpoint endpoint;
  if (url.rfind("wss://", 0) == 0) {
    endpoint.tls = true;
    url.remove_prefix(6);
  } else if (url.rfind("ws://", 0) == 0) {
    endpoint.tls = false;
    url.remove_prefix(5);
  } else {
    error = "url must start with ws:// or wss://";
    return std::nullopt;
  }

  const std::size_t slash = url.find('/');
  const std::string_view authority = url.substr(0, slash);
  endpoint.path = slash == std::string_view::npos ? "/" : std::string(url.substr(slash));

  std::string_view host;
  std::string_view port_text;
  bool has_port = false;
  if (!SplitAuthority(authority, host, port_text, has_port)) {
    error = "url has a malformed host";
    return std::nullopt;
  }
  if (host.empty()) {
    error = "url has no host";
    return std::nullopt;
  }
  if (has_port) {
    if (!ParsePort(port_text, endpoint.port)) {
      error = "url has an invalid port";
      return std::nullopt;
    }
  } else {
    endpoint.port = endpoint.tls ? kDefaultTlsPort : kDefaultPlainPort;
  }
  endpoint.host = std::string(host);
  return endpoint;
}

}  // namespace audiofork
