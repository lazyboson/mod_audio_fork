#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "audiofork/bytes.hpp"

namespace audiofork {

// Single-producer single-consumer byte ring: Push from exactly one thread, Pop
// from exactly one other. Push is all-or-nothing; a full ring rejects the frame
// and the caller counts the drop (drop-oldest policy lives in the shard-owned
// send buffer, not here).
class SpscByteRing {
  struct PrivateTag {};

 public:
  [[nodiscard]] static std::unique_ptr<SpscByteRing> Create(std::size_t min_capacity) {
    if (min_capacity == 0 || min_capacity > kMaxCapacity) {
      return nullptr;
    }
    std::size_t capacity = 1;
    while (capacity < min_capacity) {
      capacity <<= 1;
    }
    return std::make_unique<SpscByteRing>(PrivateTag{}, capacity);
  }

  SpscByteRing(PrivateTag, std::size_t capacity) : storage_(capacity), mask_(capacity - 1) {}

  [[nodiscard]] bool Push(ConstByteSpan bytes) noexcept {
    const std::uint64_t head = head_.load(std::memory_order_relaxed);
    // acquire pairs with the consumer's release in Pop(): space the consumer
    // freed has been fully read before the producer overwrites it
    const std::uint64_t tail = tail_.load(std::memory_order_acquire);
    const std::size_t free_bytes = storage_.size() - static_cast<std::size_t>(head - tail);
    if (bytes.size() > free_bytes) {
      return false;
    }
    CopyIn(bytes, head);
    // release publishes the bytes written by CopyIn to the consumer's acquire
    head_.store(head + bytes.size(), std::memory_order_release);
    return true;
  }

  [[nodiscard]] std::size_t Pop(MutableByteSpan dest) noexcept {
    const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
    // acquire pairs with the producer's release in Push(): published bytes are visible
    const std::uint64_t head = head_.load(std::memory_order_acquire);
    const std::size_t count = std::min(dest.size(), static_cast<std::size_t>(head - tail));
    CopyOut(dest, tail, count);
    // release publishes the completed read so the producer may reuse the space
    tail_.store(tail + count, std::memory_order_release);
    return count;
  }

  [[nodiscard]] std::size_t capacity() const noexcept { return storage_.size(); }

  [[nodiscard]] std::size_t size() const noexcept {
    return static_cast<std::size_t>(head_.load(std::memory_order_acquire) -
                                    tail_.load(std::memory_order_acquire));
  }

 private:
  static constexpr std::size_t kMaxCapacity = std::size_t{1} << 31U;

  void CopyIn(ConstByteSpan bytes, std::uint64_t head) noexcept {
    const auto offset = static_cast<std::ptrdiff_t>(static_cast<std::size_t>(head) & mask_);
    const auto first = static_cast<std::ptrdiff_t>(
        std::min(bytes.size(), storage_.size() - static_cast<std::size_t>(offset)));
    std::copy(bytes.begin(), bytes.begin() + first, storage_.begin() + offset);
    std::copy(bytes.begin() + first, bytes.end(), storage_.begin());
  }

  void CopyOut(MutableByteSpan dest, std::uint64_t tail, std::size_t count) noexcept {
    const auto offset = static_cast<std::ptrdiff_t>(static_cast<std::size_t>(tail) & mask_);
    const auto total = static_cast<std::ptrdiff_t>(count);
    const auto first = static_cast<std::ptrdiff_t>(
        std::min(count, storage_.size() - static_cast<std::size_t>(offset)));
    std::copy(storage_.begin() + offset, storage_.begin() + offset + first, dest.begin());
    std::copy(storage_.begin(), storage_.begin() + (total - first), dest.begin() + first);
  }

  std::vector<std::uint8_t> storage_;
  const std::size_t mask_;
  // producer and consumer indices live on separate cache lines to avoid false sharing
  alignas(64) std::atomic<std::uint64_t> head_{0};
  alignas(64) std::atomic<std::uint64_t> tail_{0};
};

}  // namespace audiofork
