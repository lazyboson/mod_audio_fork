#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "audiofork/bytes.hpp"
#include "audiofork/slab_pool.hpp"
#include "audiofork/slab_queue.hpp"

namespace audiofork {

// Inbound playback audio, shard-owned (DESIGN.md §7, decision 13).
//
// The cap exists only as a backstop for a peer that ignores backpressure:
// refusing the newest audio beats dropping history, because losing the END of a
// bot's sentence is worse than delaying it. Flow control proper lives in
// ForkSession, which can also see the audio already handed to the media thread.
//
// Barge-in (`clear`) flushes everything AND mutes until the next `mark`, so
// audio the server already put on the wire cannot resurrect after the caller
// interrupts.
class JitterBuffer {
 public:
  struct Options {
    std::size_t cap_bytes = 0;
  };

  JitterBuffer(SlabPool pool, Options options);

  // Returns bytes refused (0 in the healthy case). Refuses while muted after a
  // clear, and refuses the excess if a peer overruns the cap.
  [[nodiscard]] std::size_t Append(ConstByteSpan bytes);

  // Barge-in: drop everything buffered and mute until the next mark.
  void Clear();

  // A named position in the audio stream. Also re-arms playback after a clear.
  void AddMark(std::string name);

  [[nodiscard]] ConstByteSpan Peek(std::size_t max_bytes) const { return queue_.Peek(max_bytes); }
  void Consume(std::size_t bytes);

  // Marks whose audio has been fully handed downstream, oldest first.
  [[nodiscard]] std::vector<std::string> TakeReachedMarks();

  [[nodiscard]] bool muted() const { return muted_; }
  [[nodiscard]] std::size_t size() const { return queue_.size(); }
  [[nodiscard]] bool empty() const { return queue_.empty(); }

 private:
  struct Mark {
    std::uint64_t offset = 0;
    std::string name;
  };

  SlabQueue queue_;
  std::size_t cap_bytes_;
  bool muted_ = false;
  std::uint64_t written_offset_ = 0;
  std::uint64_t consumed_offset_ = 0;
  std::deque<Mark> marks_;
  std::vector<std::string> reached_marks_;
};

}  // namespace audiofork
