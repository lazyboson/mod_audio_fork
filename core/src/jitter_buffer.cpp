#include "audiofork/jitter_buffer.hpp"

#include <algorithm>
#include <utility>

namespace audiofork {

JitterBuffer::JitterBuffer(SlabPool pool, Options options)
    : queue_(std::move(pool)), cap_bytes_(options.cap_bytes) {}

std::size_t JitterBuffer::Append(ConstByteSpan bytes) {
  if (muted_ || bytes.empty()) {
    return bytes.size();
  }
  const std::size_t room = cap_bytes_ > queue_.size() ? cap_bytes_ - queue_.size() : 0;
  const std::size_t admit = std::min(room, bytes.size());
  const std::size_t stored = queue_.Append(ConstByteSpan(bytes.begin(), admit));
  written_offset_ += stored;
  return bytes.size() - stored;
}

void JitterBuffer::Clear() {
  queue_.Clear();
  // discarded audio never played, so its marks were never reached
  marks_.clear();
  consumed_offset_ = written_offset_;
  muted_ = true;
}

void JitterBuffer::AddMark(std::string name) {
  muted_ = false;
  if (written_offset_ <= consumed_offset_) {
    // every byte before this mark has already gone downstream, so it is reached
    // the moment it arrives
    reached_marks_.push_back(std::move(name));
    return;
  }
  marks_.push_back(Mark{written_offset_, std::move(name)});
}

void JitterBuffer::Consume(std::size_t bytes) {
  const std::size_t before = queue_.size();
  queue_.Consume(bytes);
  consumed_offset_ += before - queue_.size();
  while (!marks_.empty() && marks_.front().offset <= consumed_offset_) {
    reached_marks_.push_back(std::move(marks_.front().name));
    marks_.pop_front();
  }
}

std::vector<std::string> JitterBuffer::TakeReachedMarks() { return std::move(reached_marks_); }

}  // namespace audiofork
