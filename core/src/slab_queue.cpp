#include "audiofork/slab_queue.hpp"

#include <algorithm>
#include <utility>

namespace audiofork {

SlabQueue::SlabQueue(SlabPool pool) : pool_(std::move(pool)) {}

std::size_t SlabQueue::Append(ConstByteSpan bytes) {
  const std::uint8_t* read = bytes.begin();
  std::size_t remaining = bytes.size();
  while (remaining > 0) {
    if (chunks_.empty() || chunks_.back().end == chunks_.back().lease.bytes().size()) {
      if (!AcquireChunk()) {
        break;
      }
    }
    Chunk& tail = chunks_.back();
    MutableByteSpan slab = tail.lease.bytes();
    const std::size_t take = std::min(slab.size() - tail.end, remaining);
    std::copy(read, read + take, slab.begin() + static_cast<std::ptrdiff_t>(tail.end));
    tail.end += take;
    size_ += take;
    read += take;
    remaining -= take;
  }
  return bytes.size() - remaining;
}

ConstByteSpan SlabQueue::Peek(std::size_t max_bytes) const {
  if (chunks_.empty() || max_bytes == 0) {
    return {};
  }
  const Chunk& front = chunks_.front();
  const std::size_t available = std::min(front.size(), max_bytes);
  return {front.lease.bytes().begin() + static_cast<std::ptrdiff_t>(front.begin), available};
}

void SlabQueue::Consume(std::size_t bytes) {
  std::size_t remaining = std::min(bytes, size_);
  while (remaining > 0 && !chunks_.empty()) {
    Chunk& front = chunks_.front();
    const std::size_t take = std::min(front.size(), remaining);
    front.begin += take;
    size_ -= take;
    remaining -= take;
    PopFrontIfDrained();
  }
}

void SlabQueue::Clear() {
  chunks_.clear();
  size_ = 0;
}

bool SlabQueue::AcquireChunk() {
  auto lease = pool_.Acquire();
  if (!lease.has_value()) {
    return false;
  }
  chunks_.push_back(Chunk{*std::move(lease), 0, 0});
  return true;
}

void SlabQueue::PopFrontIfDrained() {
  // keep a partially filled tail alive: it is still the append target
  while (chunks_.size() > 1 && chunks_.front().size() == 0) {
    chunks_.pop_front();
  }
  if (chunks_.size() == 1 && chunks_.front().size() == 0 &&
      chunks_.front().end == chunks_.front().lease.bytes().size()) {
    chunks_.pop_front();
  }
}

}  // namespace audiofork
