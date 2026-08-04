#include "audiofork/slab_pool.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace audiofork {
namespace {

constexpr std::size_t kSlab = 1024;

SlabPool MakePool(std::size_t max_slabs) {
  auto pool = SlabPool::Create({kSlab, kSlab * max_slabs});
  EXPECT_TRUE(pool.has_value());
  return *std::move(pool);
}

TEST(SlabPool, CreateRejectsZeroSlabSize) {
  EXPECT_FALSE(SlabPool::Create({0, kSlab}).has_value());
}

TEST(SlabPool, CreateRejectsCapSmallerThanOneSlab) {
  EXPECT_FALSE(SlabPool::Create({kSlab, kSlab - 1}).has_value());
}

TEST(SlabPool, LeaseBytesAreSlabSized) {
  auto pool = MakePool(4);
  auto lease = pool.Acquire();
  ASSERT_TRUE(lease.has_value());
  EXPECT_EQ(lease->bytes().size(), kSlab);
}

TEST(SlabPool, AcquireFailsBeyondCap) {
  auto pool = MakePool(4);
  std::vector<SlabLease> held;
  for (int i = 0; i < 4; ++i) {
    auto lease = pool.Acquire();
    ASSERT_TRUE(lease.has_value());
    held.push_back(*std::move(lease));
  }
  EXPECT_FALSE(pool.Acquire().has_value());

  const auto stats = pool.stats();
  EXPECT_EQ(stats.allocated_bytes, kSlab * 4);
  EXPECT_EQ(stats.leased_slabs, 4U);
  EXPECT_EQ(stats.free_slabs, 0U);
}

TEST(SlabPool, ReleasedSlabIsReusedWithoutNewAllocation) {
  auto pool = MakePool(1);
  {
    auto lease = pool.Acquire();
    ASSERT_TRUE(lease.has_value());
    EXPECT_FALSE(pool.Acquire().has_value());
  }
  EXPECT_EQ(pool.stats().free_slabs, 1U);

  auto again = pool.Acquire();
  ASSERT_TRUE(again.has_value());
  const auto stats = pool.stats();
  EXPECT_EQ(stats.allocated_bytes, kSlab);
  EXPECT_EQ(stats.leased_slabs, 1U);
}

TEST(SlabPool, MovedFromLeaseDoesNotDoubleRelease) {
  auto pool = MakePool(2);
  auto lease = pool.Acquire();
  ASSERT_TRUE(lease.has_value());
  {
    SlabLease moved = *std::move(lease);
    EXPECT_EQ(moved.bytes().size(), kSlab);
    EXPECT_EQ(pool.stats().leased_slabs, 1U);
  }
  EXPECT_EQ(pool.stats().leased_slabs, 0U);
  EXPECT_EQ(pool.stats().free_slabs, 1U);
}

TEST(SlabPool, MoveAssignmentReleasesTheOverwrittenSlab) {
  auto pool = MakePool(2);
  auto first = pool.Acquire();
  auto second = pool.Acquire();
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(pool.stats().leased_slabs, 2U);

  *first = *std::move(second);
  EXPECT_EQ(pool.stats().leased_slabs, 1U);
  EXPECT_EQ(pool.stats().free_slabs, 1U);
}

TEST(SlabPool, LeaseMayOutliveEveryPoolHandle) {
  std::optional<SlabLease> lease;
  {
    auto pool = MakePool(2);
    lease = pool.Acquire();
    ASSERT_TRUE(lease.has_value());
  }
  lease->bytes().begin()[0] = 0xAB;
  lease.reset();
}

TEST(SlabPool, ThreadedAcquireReleaseStaysConsistent) {
  auto pool = MakePool(6);
  constexpr int kThreads = 8;
  constexpr int kIterations = 2000;

  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([&pool, t] {
      for (int i = 0; i < kIterations; ++i) {
        auto lease = pool.Acquire();
        if (!lease.has_value()) {
          std::this_thread::yield();
          continue;
        }
        lease->bytes().begin()[0] = static_cast<std::uint8_t>(t);
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }

  const auto stats = pool.stats();
  EXPECT_EQ(stats.leased_slabs, 0U);
  EXPECT_LE(stats.allocated_bytes, kSlab * 6);
  EXPECT_EQ(stats.free_slabs * kSlab, stats.allocated_bytes);
}

}  // namespace
}  // namespace audiofork
