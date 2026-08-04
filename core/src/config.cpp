#include "audiofork/config.hpp"

#include <algorithm>

namespace audiofork {
namespace {

constexpr std::size_t kMaxShards = 16;
constexpr std::chrono::milliseconds kMinBuffer{100};
constexpr std::chrono::milliseconds kMaxBuffer{60000};
constexpr std::chrono::milliseconds kMinCoalesce{20};

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
  return config;
}

}  // namespace audiofork
