#include "audiofork/send_buffer.hpp"

#include <algorithm>
#include <utility>

namespace audiofork {

SendBuffer::SendBuffer(SlabPool pool, std::size_t cap_bytes)
    : queue_(std::move(pool)), cap_bytes_(cap_bytes) {}

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
    remaining -= skip;
    dropped += skip;
  }
  if (queue_.size() + remaining > cap_bytes_) {
    dropped += DropOldest(queue_.size() + remaining - cap_bytes_);
  }

  while (remaining > 0) {
    const std::size_t stored = queue_.Append(ConstByteSpan(read, remaining));
    read += stored;
    remaining -= stored;
    if (remaining == 0) {
      break;
    }
    // pool exhausted: same policy as a full buffer, drop oldest and retry
    const std::size_t reclaimed = DropOldest(remaining);
    dropped += reclaimed;
    if (reclaimed == 0) {
      return dropped + remaining;
    }
  }
  return dropped;
}

std::size_t SendBuffer::SetCap(std::size_t cap_bytes) {
  cap_bytes_ = cap_bytes;
  if (queue_.size() <= cap_bytes_) {
    return 0;
  }
  return DropOldest(queue_.size() - cap_bytes_);
}

std::size_t SendBuffer::DropOldest(std::size_t bytes) {
  const std::size_t before = queue_.size();
  queue_.Consume(bytes);
  return before - queue_.size();
}

}  // namespace audiofork
