#include "audiofork/spsc_ring.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <numeric>
#include <random>
#include <thread>
#include <vector>

namespace audiofork {
namespace {

std::vector<std::uint8_t> Sequence(std::size_t count, std::uint8_t start = 0) {
  std::vector<std::uint8_t> bytes(count);
  std::iota(bytes.begin(), bytes.end(), start);
  return bytes;
}

TEST(SpscByteRing, CreateRejectsZeroCapacity) { EXPECT_EQ(SpscByteRing::Create(0), nullptr); }

TEST(SpscByteRing, CreateRoundsCapacityUpToPowerOfTwo) {
  ASSERT_NE(SpscByteRing::Create(1), nullptr);
  EXPECT_EQ(SpscByteRing::Create(1)->capacity(), 1U);
  EXPECT_EQ(SpscByteRing::Create(33)->capacity(), 64U);
  EXPECT_EQ(SpscByteRing::Create(4096)->capacity(), 4096U);
}

TEST(SpscByteRing, PushPopRoundTrip) {
  auto ring = SpscByteRing::Create(64);
  ASSERT_NE(ring, nullptr);
  const auto sent = Sequence(10);
  ASSERT_TRUE(ring->Push(ConstByteSpan(sent)));
  EXPECT_EQ(ring->size(), 10U);

  std::vector<std::uint8_t> received(10);
  EXPECT_EQ(ring->Pop(MutableByteSpan(received)), 10U);
  EXPECT_EQ(received, sent);
  EXPECT_EQ(ring->size(), 0U);
}

TEST(SpscByteRing, PushIsAllOrNothingWhenFull) {
  auto ring = SpscByteRing::Create(16);
  ASSERT_NE(ring, nullptr);
  ASSERT_TRUE(ring->Push(ConstByteSpan(Sequence(16))));
  const auto one_byte = Sequence(1);
  EXPECT_FALSE(ring->Push(ConstByteSpan(one_byte)));
  EXPECT_EQ(ring->size(), 16U);
}

TEST(SpscByteRing, PushLargerThanCapacityAlwaysFails) {
  auto ring = SpscByteRing::Create(16);
  ASSERT_NE(ring, nullptr);
  EXPECT_FALSE(ring->Push(ConstByteSpan(Sequence(17))));
}

TEST(SpscByteRing, ZeroSizePushSucceeds) {
  auto ring = SpscByteRing::Create(16);
  ASSERT_NE(ring, nullptr);
  EXPECT_TRUE(ring->Push(ConstByteSpan()));
  EXPECT_EQ(ring->size(), 0U);
}

TEST(SpscByteRing, PopFromEmptyReturnsZero) {
  auto ring = SpscByteRing::Create(16);
  ASSERT_NE(ring, nullptr);
  std::vector<std::uint8_t> dest(8);
  EXPECT_EQ(ring->Pop(MutableByteSpan(dest)), 0U);
}

TEST(SpscByteRing, PopSmallerDestReturnsPartialData) {
  auto ring = SpscByteRing::Create(64);
  ASSERT_NE(ring, nullptr);
  ASSERT_TRUE(ring->Push(ConstByteSpan(Sequence(10))));

  std::vector<std::uint8_t> dest(4);
  EXPECT_EQ(ring->Pop(MutableByteSpan(dest)), 4U);
  EXPECT_EQ(dest, Sequence(4));
  EXPECT_EQ(ring->size(), 6U);
}

TEST(SpscByteRing, DataSurvivesWrapAround) {
  auto ring = SpscByteRing::Create(16);
  ASSERT_NE(ring, nullptr);
  std::uint8_t next_in = 0;
  std::uint8_t next_out = 0;
  for (int round = 0; round < 100; ++round) {
    const auto chunk = Sequence(11, next_in);
    next_in = static_cast<std::uint8_t>(next_in + 11);
    ASSERT_TRUE(ring->Push(ConstByteSpan(chunk)));

    std::vector<std::uint8_t> dest(11);
    ASSERT_EQ(ring->Pop(MutableByteSpan(dest)), 11U);
    ASSERT_EQ(dest, Sequence(11, next_out));
    next_out = static_cast<std::uint8_t>(next_out + 11);
  }
}

TEST(SpscByteRing, ThreadedByteStreamKeepsIntegrity) {
  auto ring = SpscByteRing::Create(4096);
  ASSERT_NE(ring, nullptr);
  constexpr std::size_t kTotalBytes = 1U << 21U;

  std::thread producer([&ring] {
    std::mt19937 rng(42);
    std::uniform_int_distribution<std::size_t> chunk_size(1, 97);
    std::size_t sent = 0;
    std::vector<std::uint8_t> chunk;
    while (sent < kTotalBytes) {
      chunk.resize(std::min(chunk_size(rng), kTotalBytes - sent));
      for (auto& byte : chunk) {
        byte = static_cast<std::uint8_t>(sent++ % 251);
      }
      while (!ring->Push(ConstByteSpan(chunk))) {
        std::this_thread::yield();
      }
    }
  });

  std::size_t received = 0;
  std::size_t corrupt = 0;
  std::vector<std::uint8_t> dest(128);
  while (received < kTotalBytes) {
    const std::size_t count = ring->Pop(MutableByteSpan(dest));
    if (count == 0) {
      std::this_thread::yield();
      continue;
    }
    for (std::size_t i = 0; i < count; ++i) {
      if (dest[i] != static_cast<std::uint8_t>(received % 251)) {
        ++corrupt;
      }
      ++received;
    }
  }
  producer.join();
  EXPECT_EQ(corrupt, 0U);
  EXPECT_EQ(received, kTotalBytes);
  EXPECT_EQ(ring->size(), 0U);
}

}  // namespace
}  // namespace audiofork
