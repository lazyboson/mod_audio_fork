#include "audiofork/config.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace audiofork {
namespace {

using std::chrono::milliseconds;

TEST(AudioFormat, ByteMath) {
  const AudioFormat stereo_16k{16000, 2};
  EXPECT_EQ(stereo_16k.BytesPerSecond(), 64000U);
  EXPECT_EQ(stereo_16k.BytesForDuration(milliseconds{20}), 1280U);
  EXPECT_EQ(stereo_16k.BytesForDuration(milliseconds{10000}), 640000U);

  const AudioFormat mono_8k{8000, 1};
  EXPECT_EQ(mono_8k.BytesPerSecond(), 16000U);
  EXPECT_EQ(mono_8k.BytesForDuration(milliseconds{20}), 320U);
}

TEST(SanitizeConfig, DefaultsPassThroughUnchanged) {
  const ModuleConfig sanitized = SanitizeConfig(ModuleConfig{});
  EXPECT_EQ(sanitized.send_buffer, milliseconds{10000});
  EXPECT_EQ(sanitized.handoff_buffer, milliseconds{1000});
  EXPECT_EQ(sanitized.coalesce_max, milliseconds{100});
  EXPECT_EQ(sanitized.reconnect_min, milliseconds{250});
  EXPECT_EQ(sanitized.reconnect_max, milliseconds{5000});
  EXPECT_EQ(sanitized.max_forks_per_call, 4U);
}

TEST(SanitizeConfig, ClampsNonsenseInsteadOfFailing) {
  ModuleConfig config;
  config.shard_count = 999;
  config.send_buffer = milliseconds{0};
  config.handoff_buffer = milliseconds{999999};
  config.coalesce_max = milliseconds{0};
  config.reconnect_min = milliseconds{0};
  config.reconnect_max = milliseconds{1};
  config.slab_size_bytes = 7;
  config.global_memory_cap_bytes = 1;
  config.max_forks_per_call = 0;

  const ModuleConfig sanitized = SanitizeConfig(config);
  EXPECT_LE(sanitized.shard_count, 16U);
  EXPECT_GE(sanitized.send_buffer, milliseconds{100});
  EXPECT_LE(sanitized.handoff_buffer, sanitized.send_buffer);
  EXPECT_LE(sanitized.coalesce_max, sanitized.handoff_buffer);
  EXPECT_GE(sanitized.reconnect_min, milliseconds{1});
  EXPECT_GE(sanitized.reconnect_max, sanitized.reconnect_min);
  EXPECT_GE(sanitized.slab_size_bytes, 4096U);
  EXPECT_GE(sanitized.global_memory_cap_bytes, sanitized.slab_size_bytes * 16);
  EXPECT_GE(sanitized.max_forks_per_call, 1U);
}

TEST(SanitizeConfig, HugeBufferRequestIsBounded) {
  ModuleConfig config;
  config.send_buffer = milliseconds{600000};
  const ModuleConfig sanitized = SanitizeConfig(config);
  EXPECT_LE(sanitized.send_buffer, milliseconds{60000});
}

TEST(SanitizeConfig, IsIdempotent) {
  ModuleConfig config;
  config.send_buffer = milliseconds{0};
  const ModuleConfig once = SanitizeConfig(config);
  const ModuleConfig twice = SanitizeConfig(once);
  EXPECT_EQ(once.send_buffer, twice.send_buffer);
  EXPECT_EQ(once.handoff_buffer, twice.handoff_buffer);
  EXPECT_EQ(once.coalesce_max, twice.coalesce_max);
  EXPECT_EQ(once.slab_size_bytes, twice.slab_size_bytes);
}

}  // namespace
}  // namespace audiofork
