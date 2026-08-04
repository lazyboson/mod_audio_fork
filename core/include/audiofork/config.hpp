#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace audiofork {

enum class MixType : std::uint8_t {
  kMono,
  kMixed,
  kStereo,
};

struct AudioFormat {
  std::uint32_t sample_rate = 16000;
  std::uint8_t channels = 2;

  [[nodiscard]] std::size_t BytesPerSecond() const {
    return static_cast<std::size_t>(sample_rate) * channels * sizeof(std::int16_t);
  }
  [[nodiscard]] std::size_t BytesForDuration(std::chrono::milliseconds duration) const {
    return BytesPerSecond() * static_cast<std::size_t>(duration.count()) / 1000;
  }
};

struct Endpoint {
  std::string host;
  std::uint16_t port = 0;
  std::string path = "/";
  bool tls = false;
};

// Populated from audio_fork.conf.xml by the module; core never parses XML.
struct ModuleConfig {
  std::size_t shard_count = 0;
  std::chrono::milliseconds send_buffer{10000};
  std::chrono::milliseconds handoff_buffer{1000};
  std::chrono::milliseconds coalesce_max{100};
  std::chrono::milliseconds drain_timeout{2000};
  std::chrono::milliseconds playback_high_watermark{10000};
  std::chrono::milliseconds playback_low_watermark{2000};
  std::size_t global_memory_cap_bytes = std::size_t{1536} * 1024 * 1024;
  std::chrono::milliseconds emergency_buffer{2000};
  std::chrono::milliseconds reconnect_min{250};
  std::chrono::milliseconds reconnect_max{5000};
  std::size_t slab_size_bytes = std::size_t{64} * 1024;
  std::size_t max_forks_per_call = 4;
};

// Rejects values that would break invariants downstream (zero-size buffers,
// inverted backoff bounds) rather than letting them surface as odd runtime
// behaviour. Clamps rather than fails: a bad config must not stop call flow.
[[nodiscard]] ModuleConfig SanitizeConfig(ModuleConfig config);

struct ForkParams {
  std::string call_uuid;
  std::string fork_id;
  Endpoint endpoint;
  AudioFormat format;
  MixType mix_type = MixType::kMono;
  std::string metadata_json;
};

}  // namespace audiofork
