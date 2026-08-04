#include "audiofork/send_buffer.hpp"

#include <algorithm>
#include <utility>

namespace audiofork {

SendBuffer::SendBuffer(SlabPool pool, std::size_t cap_bytes)
    : pool_(std::move(pool)), cap_bytes_(cap_bytes) {}

std::size_t SendBuffer::Append(ConstByteSpan bytes) {
  if (bytes.empty() || cap_bytes_ == 0) {
    return bytes.size();
  }

  std::size_t dropped = 0;
  // A frame larger than the whole cap keeps only its tail: the newest audio.
  const std::uint8_t* read = bytes.begin();
  std::size_t remaining = bytes.size();
  if (remaining > cap_bytes_) {
    const std::size_t skip = remaining - cap_bytes_;
    read += skip;
    remaining = cap_bytes_;
    dropped += skip;
  }
  if (size_ + remaining > cap_bytes_) {
    dropped += DropOldest(size_ + remaining - cap_bytes_);
  }

  while (remaining > 0) {
    if (chunks_.empty() || chunks_.back().end == chunks_.back().lease.bytes().size()) {
      if (!AcquireChunk()) {
        // pool exhausted: same policy as a full buffer, drop oldest and retry
        const std::size_t reclaimed = DropOldest(remaining);
        dropped += reclaimed;
        if (reclaimed == 0) {
          return dropped + remaining;
        }
        continue;
      }
    }
    Chunk& tail = chunks_.back();
    MutableByteSpan slab = tail.lease.bytes();
    const std::size_t room = slab.size() - tail.end;
    const std::size_t take = std::min(room, remaining);
    std::copy(read, read + take, slab.begin() + static_cast<std::ptrdiff_t>(tail.end));
    tail.end += take;
    size_ += take;
    read += take;
    remaining -= take;
  }
  return dropped;
}

ConstByteSpan SendBuffer::Peek(std::size_t max_bytes) const {
  if (chunks_.empty() || max_bytes == 0) {
    return {};
  }
  const Chunk& front = chunks_.front();
  const std::size_t available = std::min(front.size(), max_bytes);
  return {front.lease.bytes().begin() + static_cast<std::ptrdiff_t>(front.begin), available};
}

void SendBuffer::Consume(std::size_t bytes) {
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

void SendBuffer::Clear() {
  chunks_.clear();
  size_ = 0;
}

std::size_t SendBuffer::SetCap(std::size_t cap_bytes) {
  cap_bytes_ = cap_bytes;
  if (size_ <= cap_bytes_) {
    return 0;
  }
  return DropOldest(size_ - cap_bytes_);
}

bool SendBuffer::AcquireChunk() {
  auto lease = pool_.Acquire();
  if (!lease.has_value()) {
    return false;
  }
  chunks_.push_back(Chunk{*std::move(lease), 0, 0});
  return true;
}

std::size_t SendBuffer::DropOldest(std::size_t bytes) {
  const std::size_t before = size_;
  Consume(bytes);
  return before - size_;
}

void SendBuffer::PopFrontIfDrained() {
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
