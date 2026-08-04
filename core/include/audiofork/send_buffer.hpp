#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

#include "audiofork/bytes.hpp"
#include "audiofork/slab_pool.hpp"

namespace audiofork {

// Shard-owned outbound audio buffer: grows in slabs up to a byte cap, then
// drops the OLDEST audio to admit new frames (DESIGN.md §7 — a stalled peer
// must not stall the call, and live audio outranks history). Slab exhaustion
// from the global cap is treated exactly like reaching the local cap, so
// pool pressure degrades quality instead of failing calls.
class SendBuffer {
 public:
  SendBuffer(SlabPool pool, std::size_t cap_bytes);

  // Returns bytes discarded from the front to make room (0 in the healthy case).
  [[nodiscard]] std::size_t Append(ConstByteSpan bytes);

  // Contiguous front run, empty when drained. Never spans slabs.
  [[nodiscard]] ConstByteSpan Peek(std::size_t max_bytes) const;
  void Consume(std::size_t bytes);
  void Clear();

  [[nodiscard]] std::size_t size() const { return size_; }
  [[nodiscard]] bool empty() const { return size_ == 0; }
  [[nodiscard]] std::size_t cap_bytes() const { return cap_bytes_; }

  // Emergency degradation (DESIGN.md §5): shrinking below the current fill
  // drops oldest audio immediately and reports how much went.
  [[nodiscard]] std::size_t SetCap(std::size_t cap_bytes);

 private:
  struct Chunk {
    SlabLease lease;
    std::size_t begin = 0;
    std::size_t end = 0;

    [[nodiscard]] std::size_t size() const { return end - begin; }
  };

  [[nodiscard]] bool AcquireChunk();
  [[nodiscard]] std::size_t DropOldest(std::size_t bytes);
  void PopFrontIfDrained();

  SlabPool pool_;
  std::size_t cap_bytes_;
  std::size_t size_ = 0;
  std::deque<Chunk> chunks_;
};

}  // namespace audiofork
