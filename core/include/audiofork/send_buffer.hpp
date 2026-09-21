#pragma once

#include <cstddef>

#include "audiofork/bytes.hpp"
#include "audiofork/slab_pool.hpp"
#include "audiofork/slab_queue.hpp"

namespace audiofork {

// Shard-owned outbound audio buffer: a SlabQueue plus one policy — when the cap
// (or the pool) is reached, the OLDEST audio goes (DESIGN.md §7). A stalled peer
// must not stall the call, and live audio outranks history.
class SendBuffer {
 public:
  SendBuffer(SlabPool pool, std::size_t cap_bytes);

  // `pool_exhausted` separates the two reasons audio goes: this fork's own cap
  // (ordinary backpressure) and a dry global pool, which is what triggers the
  // emergency degradation of DESIGN.md §5.
  struct AppendResult {
    std::size_t dropped_bytes = 0;
    bool pool_exhausted = false;
  };

  // `dropped_bytes` is what was discarded to make room (0 when healthy).
  [[nodiscard]] AppendResult Append(ConstByteSpan bytes);

  [[nodiscard]] ConstByteSpan Peek(std::size_t max_bytes) const { return queue_.Peek(max_bytes); }
  void Consume(std::size_t bytes) { queue_.Consume(bytes); }
  void Clear() { queue_.Clear(); }

  [[nodiscard]] std::size_t size() const { return queue_.size(); }
  [[nodiscard]] bool empty() const { return queue_.empty(); }
  [[nodiscard]] std::size_t cap_bytes() const { return cap_bytes_; }

  // Emergency degradation (DESIGN.md §5): shrinking below the current fill
  // drops oldest audio immediately and reports how much went.
  [[nodiscard]] std::size_t SetCap(std::size_t cap_bytes);

 private:
  [[nodiscard]] std::size_t DropOldest(std::size_t bytes);

  SlabQueue queue_;
  std::size_t cap_bytes_;
};

}  // namespace audiofork
