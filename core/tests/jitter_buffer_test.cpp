#include "audiofork/jitter_buffer.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <numeric>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace audiofork {
namespace {

constexpr std::size_t kSlab = 256;

SlabPool MakePool(std::size_t max_slabs) {
  auto pool = SlabPool::Create({kSlab, kSlab * max_slabs});
  EXPECT_TRUE(pool.has_value());
  return *std::move(pool);
}

JitterBuffer MakeBuffer(std::size_t cap = 2048, std::size_t max_slabs = 32) {
  return JitterBuffer(MakePool(max_slabs), {cap});
}

std::vector<std::uint8_t> Ramp(std::size_t count, std::uint8_t start = 0) {
  std::vector<std::uint8_t> bytes(count);
  std::iota(bytes.begin(), bytes.end(), start);
  return bytes;
}

std::vector<std::uint8_t> DrainAll(JitterBuffer& buffer) {
  std::vector<std::uint8_t> out;
  while (!buffer.empty()) {
    const ConstByteSpan chunk = buffer.Peek(64);
    EXPECT_FALSE(chunk.empty());
    out.insert(out.end(), chunk.begin(), chunk.end());
    buffer.Consume(chunk.size());
  }
  return out;
}

TEST(JitterBuffer, RoundTripsAudioByteExact) {
  JitterBuffer buffer = MakeBuffer();
  const auto audio = Ramp(700);
  EXPECT_EQ(buffer.Append(ConstByteSpan(audio)), 0U);
  EXPECT_EQ(buffer.size(), 700U);
  EXPECT_EQ(DrainAll(buffer), audio);
}

TEST(JitterBuffer, CapRefusesExcessRatherThanDroppingHistory) {
  // the cap is a backstop for a peer ignoring backpressure; losing the END of a
  // sentence is worse than delaying it, so newest is what gets refused
  JitterBuffer buffer = MakeBuffer(/*cap=*/1024);
  const auto first = Ramp(1024, 0);
  EXPECT_EQ(buffer.Append(ConstByteSpan(first)), 0U);
  const auto excess = Ramp(200, 99);
  EXPECT_EQ(buffer.Append(ConstByteSpan(excess)), 200U);

  const auto kept = DrainAll(buffer);
  EXPECT_EQ(kept, first);
}

TEST(JitterBuffer, ClearFlushesAndMutesUntilNextMark) {
  JitterBuffer buffer = MakeBuffer();
  EXPECT_EQ(buffer.Append(ConstByteSpan(Ramp(400))), 0U);

  buffer.Clear();
  EXPECT_TRUE(buffer.empty());
  EXPECT_TRUE(buffer.muted());

  // audio the server already had in flight must not resurrect
  const auto residue = Ramp(300, 7);
  EXPECT_EQ(buffer.Append(ConstByteSpan(residue)), 300U);
  EXPECT_TRUE(buffer.empty());

  buffer.AddMark("after-barge-in");
  EXPECT_FALSE(buffer.muted());
  const auto fresh = Ramp(200, 11);
  EXPECT_EQ(buffer.Append(ConstByteSpan(fresh)), 0U);
  EXPECT_EQ(DrainAll(buffer), fresh);
}

TEST(JitterBuffer, MarkIsReachedOnlyOnceItsAudioIsConsumed) {
  JitterBuffer buffer = MakeBuffer();
  EXPECT_EQ(buffer.Append(ConstByteSpan(Ramp(300))), 0U);
  buffer.AddMark("sentence-1");
  EXPECT_EQ(buffer.Append(ConstByteSpan(Ramp(300))), 0U);
  buffer.AddMark("sentence-2");

  EXPECT_TRUE(buffer.TakeReachedMarks().empty());

  buffer.Consume(299);
  EXPECT_TRUE(buffer.TakeReachedMarks().empty());

  buffer.Consume(1);
  EXPECT_EQ(buffer.TakeReachedMarks(), std::vector<std::string>{"sentence-1"});

  buffer.Consume(300);
  EXPECT_EQ(buffer.TakeReachedMarks(), std::vector<std::string>{"sentence-2"});
  EXPECT_TRUE(buffer.TakeReachedMarks().empty());
}

TEST(JitterBuffer, MarksAreReportedOldestFirst) {
  JitterBuffer buffer = MakeBuffer();
  EXPECT_EQ(buffer.Append(ConstByteSpan(Ramp(100))), 0U);
  buffer.AddMark("first");
  EXPECT_EQ(buffer.Append(ConstByteSpan(Ramp(100))), 0U);
  buffer.AddMark("second");

  buffer.Consume(200);
  const std::vector<std::string> expected{"first", "second"};
  EXPECT_EQ(buffer.TakeReachedMarks(), expected);
}

TEST(JitterBuffer, ClearDiscardsPendingMarksBecauseTheirAudioNeverPlayed) {
  JitterBuffer buffer = MakeBuffer();
  EXPECT_EQ(buffer.Append(ConstByteSpan(Ramp(200))), 0U);
  buffer.AddMark("never-played");

  buffer.Clear();
  buffer.AddMark("re-armed");
  EXPECT_EQ(buffer.Append(ConstByteSpan(Ramp(100))), 0U);
  buffer.Consume(100);
  EXPECT_EQ(buffer.TakeReachedMarks(), std::vector<std::string>{"re-armed"});
}

TEST(JitterBuffer, MarkOnAnEmptyStreamIsReachedImmediately) {
  JitterBuffer buffer = MakeBuffer();
  buffer.AddMark("empty-stream");
  EXPECT_EQ(buffer.TakeReachedMarks(), std::vector<std::string>{"empty-stream"});
}

TEST(JitterBuffer, PoolExhaustionRefusesRatherThanCorrupting) {
  JitterBuffer buffer = MakeBuffer(/*cap=*/kSlab * 8, /*max_slabs=*/2);
  const auto audio = Ramp(kSlab * 4);
  const std::size_t refused = buffer.Append(ConstByteSpan(audio));
  EXPECT_EQ(refused, kSlab * 2);
  EXPECT_EQ(buffer.size(), kSlab * 2);
  const auto kept = DrainAll(buffer);
  EXPECT_EQ(kept, std::vector<std::uint8_t>(audio.begin(), audio.begin() + kSlab * 2));
}

TEST(JitterBuffer, EmptyAppendAndConsumeAreNoOps) {
  JitterBuffer buffer = MakeBuffer();
  EXPECT_EQ(buffer.Append(ConstByteSpan()), 0U);
  buffer.Consume(999);
  EXPECT_TRUE(buffer.empty());
  EXPECT_TRUE(buffer.Peek(64).empty());
}

TEST(JitterBuffer, MarkArrivingAfterItsAudioAlreadyDrainedIsReachedAtOnce) {
  JitterBuffer buffer = MakeBuffer();
  EXPECT_EQ(buffer.Append(ConstByteSpan(Ramp(200))), 0U);
  buffer.Consume(200);
  buffer.AddMark("trailing");
  EXPECT_EQ(buffer.TakeReachedMarks(), std::vector<std::string>{"trailing"});
}

}  // namespace
}  // namespace audiofork
