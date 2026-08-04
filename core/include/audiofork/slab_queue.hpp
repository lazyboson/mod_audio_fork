#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>

#include "audiofork/bytes.hpp"
#include "audiofork/slab_pool.hpp"

namespace audiofork {

// Slab-backed byte FIFO with no policy of its own: it appends what the pool
// allows and reports how much that was. Overflow policy belongs to the owner —
// SendBuffer drops oldest, JitterBuffer refuses newest — which is why this
// holds neither a cap nor a drop rule.
class SlabQueue {
 public:
  explicit SlabQueue(SlabPool pool);

  // Returns bytes actually stored; short only when the pool is exhausted.
  [[nodiscard]] std::size_t Append(ConstByteSpan bytes);

  // Contiguous front run, empty when drained. Never spans slabs.
  [[nodiscard]] ConstByteSpan Peek(std::size_t max_bytes) const;
  void Consume(std::size_t bytes);
  void Clear();

  [[nodiscard]] std::size_t size() const { return size_; }
  [[nodiscard]] bool empty() const { return size_ == 0; }

 private:
  struct Chunk {
    SlabLease lease;
    std::size_t begin = 0;
    std::size_t end = 0;

    [[nodiscard]] std::size_t size() const { return end - begin; }
  };

  [[nodiscard]] bool AcquireChunk();
  void PopFrontIfDrained();

  SlabPool pool_;
  std::size_t size_ = 0;
  std::deque<Chunk> chunks_;
};

}  // namespace audiofork
