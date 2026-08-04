#include "audiofork/send_buffer.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <numeric>
#include <optional>
#include <utility>
#include <vector>

namespace audiofork {
namespace {

constexpr std::size_t kSlab = 256;

SlabPool MakePool(std::size_t max_slabs) {
  auto pool = SlabPool::Create({kSlab, kSlab * max_slabs});
  EXPECT_TRUE(pool.has_value());
  return *std::move(pool);
}

std::vector<std::uint8_t> Ramp(std::size_t count, std::uint8_t start = 0) {
  std::vector<std::uint8_t> bytes(count);
  std::iota(bytes.begin(), bytes.end(), start);
  return bytes;
}

std::vector<std::uint8_t> DrainAll(SendBuffer& buffer) {
  std::vector<std::uint8_t> out;
  while (!buffer.empty()) {
    const ConstByteSpan chunk = buffer.Peek(64);
    EXPECT_FALSE(chunk.empty());
    out.insert(out.end(), chunk.begin(), chunk.end());
    buffer.Consume(chunk.size());
  }
  return out;
}

TEST(SendBuffer, RoundTripsWithinCap) {
  SendBuffer buffer(MakePool(8), 1024);
  const auto payload = Ramp(600);
  EXPECT_EQ(buffer.Append(ConstByteSpan(payload)), 0U);
  EXPECT_EQ(buffer.size(), 600U);
  EXPECT_EQ(DrainAll(buffer), payload);
}

TEST(SendBuffer, SpansMultipleSlabs) {
  SendBuffer buffer(MakePool(8), 2048);
  const auto payload = Ramp(1000);
  EXPECT_EQ(buffer.Append(ConstByteSpan(payload)), 0U);
  EXPECT_EQ(DrainAll(buffer), payload);
}

TEST(SendBuffer, PeekNeverSpansSlabsButDrainIsContiguous) {
  SendBuffer buffer(MakePool(8), 2048);
  const auto payload = Ramp(700);
  EXPECT_EQ(buffer.Append(ConstByteSpan(payload)), 0U);
  const ConstByteSpan first = buffer.Peek(700);
  EXPECT_LE(first.size(), kSlab);
  EXPECT_GT(first.size(), 0U);
  EXPECT_EQ(DrainAll(buffer), payload);
}

TEST(SendBuffer, DropsOldestWhenCapExceeded) {
  SendBuffer buffer(MakePool(16), 512);
  EXPECT_EQ(buffer.Append(ConstByteSpan(Ramp(512, 0))), 0U);
  const auto fresh = Ramp(100, 100);
  EXPECT_EQ(buffer.Append(ConstByteSpan(fresh)), 100U);
  EXPECT_EQ(buffer.size(), 512U);

  const auto remaining = DrainAll(buffer);
  ASSERT_EQ(remaining.size(), 512U);
  // the tail must be the newest audio, and the head must be the old audio
  // that survived — never a mix in the wrong order
  EXPECT_EQ(std::vector<std::uint8_t>(remaining.end() - 100, remaining.end()), fresh);
  EXPECT_EQ(remaining[0], 100);
}

TEST(SendBuffer, AppendLargerThanCapKeepsNewestTail) {
  SendBuffer buffer(MakePool(16), 256);
  const auto payload = Ramp(1000);
  EXPECT_EQ(buffer.Append(ConstByteSpan(payload)), 744U);
  EXPECT_EQ(buffer.size(), 256U);
  const auto kept = DrainAll(buffer);
  EXPECT_EQ(kept, std::vector<std::uint8_t>(payload.end() - 256, payload.end()));
}

TEST(SendBuffer, PoolExhaustionDegradesLikeAFullBuffer) {
  // cap allows 4 slabs of audio but the pool only ever yields 2
  SendBuffer buffer(MakePool(2), kSlab * 4);
  EXPECT_EQ(buffer.Append(ConstByteSpan(Ramp(kSlab * 2))), 0U);
  const auto fresh = Ramp(kSlab, 7);
  const std::size_t dropped = buffer.Append(ConstByteSpan(fresh));
  EXPECT_EQ(dropped, kSlab);
  EXPECT_EQ(buffer.size(), kSlab * 2);
  const auto kept = DrainAll(buffer);
  EXPECT_EQ(std::vector<std::uint8_t>(kept.end() - kSlab, kept.end()), fresh);
}

TEST(SendBuffer, ZeroCapDropsEverything) {
  SendBuffer buffer(MakePool(4), 0);
  EXPECT_EQ(buffer.Append(ConstByteSpan(Ramp(100))), 100U);
  EXPECT_TRUE(buffer.empty());
}

TEST(SendBuffer, EmptyAppendAndPeekAreNoOps) {
  SendBuffer buffer(MakePool(4), 1024);
  EXPECT_EQ(buffer.Append(ConstByteSpan()), 0U);
  EXPECT_TRUE(buffer.Peek(64).empty());
  buffer.Consume(999);
  EXPECT_TRUE(buffer.empty());
}

TEST(SendBuffer, SetCapShrinkDropsOldestImmediately) {
  SendBuffer buffer(MakePool(16), 1024);
  const auto payload = Ramp(1000);
  EXPECT_EQ(buffer.Append(ConstByteSpan(payload)), 0U);
  EXPECT_EQ(buffer.SetCap(256), 744U);
  EXPECT_EQ(buffer.size(), 256U);
  EXPECT_EQ(DrainAll(buffer), std::vector<std::uint8_t>(payload.end() - 256, payload.end()));
}

TEST(SendBuffer, SetCapGrowKeepsData) {
  SendBuffer buffer(MakePool(16), 256);
  const auto payload = Ramp(200);
  EXPECT_EQ(buffer.Append(ConstByteSpan(payload)), 0U);
  EXPECT_EQ(buffer.SetCap(1024), 0U);
  EXPECT_EQ(DrainAll(buffer), payload);
}

TEST(SendBuffer, ClearReleasesSlabsBackToPool) {
  auto pool = MakePool(8);
  SendBuffer buffer(pool, 2048);
  EXPECT_EQ(buffer.Append(ConstByteSpan(Ramp(1000))), 0U);
  EXPECT_GT(pool.stats().leased_slabs, 0U);
  buffer.Clear();
  EXPECT_TRUE(buffer.empty());
  EXPECT_EQ(pool.stats().leased_slabs, 0U);
}

TEST(SendBuffer, SteadyStateRecyclesSlabsInsteadOfGrowing) {
  auto pool = MakePool(64);
  SendBuffer buffer(pool, kSlab * 40);
  // a healthy fork: append a frame, immediately drain it, forever
  for (int round = 0; round < 500; ++round) {
    EXPECT_EQ(buffer.Append(ConstByteSpan(Ramp(160))), 0U);
    (void)DrainAll(buffer);
  }
  EXPECT_LE(pool.stats().allocated_bytes, kSlab * 2);
}

}  // namespace
}  // namespace audiofork
