#include "audiofork/protocol.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <string>
#include <variant>

namespace audiofork {
namespace {

using nlohmann::json;

json ParseBack(const std::string& encoded) {
  json parsed = json::parse(encoded, nullptr, false);
  EXPECT_FALSE(parsed.is_discarded());
  return parsed;
}

TEST(ProtocolEncode, HelloCarriesAllFields) {
  const auto encoded = EncodeHello({"call-123", 16000, 2, R"({"tenant":"acme","priority":7})"});
  ASSERT_TRUE(encoded.has_value());
  const json msg = ParseBack(*encoded);
  EXPECT_EQ(msg["type"], "hello");
  EXPECT_EQ(msg["version"], kProtocolVersion);
  EXPECT_EQ(msg["callSid"], "call-123");
  EXPECT_EQ(msg["rate"], 16000);
  EXPECT_EQ(msg["channels"], 2);
  EXPECT_EQ(msg["encoding"], "L16");
  EXPECT_EQ(msg["metadata"]["tenant"], "acme");
  EXPECT_EQ(msg["metadata"]["priority"], 7);
}

TEST(ProtocolEncode, HelloWithoutMetadataOmitsTheField) {
  const auto encoded = EncodeHello({"call-123", 8000, 1, {}});
  ASSERT_TRUE(encoded.has_value());
  EXPECT_FALSE(ParseBack(*encoded).contains("metadata"));
}

TEST(ProtocolEncode, HelloRejectsInvalidInputs) {
  EXPECT_FALSE(EncodeHello({"", 16000, 2, {}}).has_value());
  EXPECT_FALSE(EncodeHello({"call", 7999, 2, {}}).has_value());
  EXPECT_FALSE(EncodeHello({"call", 48001, 2, {}}).has_value());
  EXPECT_FALSE(EncodeHello({"call", 16000, 0, {}}).has_value());
  EXPECT_FALSE(EncodeHello({"call", 16000, 3, {}}).has_value());
  EXPECT_FALSE(EncodeHello({"call", 16000, 2, "{not json"}).has_value());
}

TEST(ProtocolEncode, Resume) {
  const json msg = ParseBack(EncodeResume(1234, 640));
  EXPECT_EQ(msg["type"], "resume");
  EXPECT_EQ(msg["gapMs"], 1234);
  EXPECT_EQ(msg["droppedMs"], 640);
}

TEST(ProtocolEncode, DtmfAcceptsValidDigitsOnly) {
  const auto encoded = EncodeDtmf('5', 160);
  ASSERT_TRUE(encoded.has_value());
  const json msg = ParseBack(*encoded);
  EXPECT_EQ(msg["type"], "dtmf");
  EXPECT_EQ(msg["digit"], "5");
  EXPECT_EQ(msg["durationMs"], 160);

  EXPECT_TRUE(EncodeDtmf('*', 100).has_value());
  EXPECT_TRUE(EncodeDtmf('#', 100).has_value());
  EXPECT_TRUE(EncodeDtmf('A', 100).has_value());
  EXPECT_FALSE(EncodeDtmf('E', 100).has_value());
  EXPECT_FALSE(EncodeDtmf('a', 100).has_value());
  EXPECT_FALSE(EncodeDtmf(' ', 100).has_value());
}

TEST(ProtocolEncode, Bye) { EXPECT_EQ(ParseBack(EncodeBye())["type"], "bye"); }

TEST(ProtocolParse, RecognizedControlMessages) {
  EXPECT_TRUE(std::holds_alternative<ClearPlayback>(ParseServerMessage(R"({"type":"clear"})")));
  EXPECT_TRUE(
      std::holds_alternative<ServerDisconnect>(ParseServerMessage(R"({"type":"disconnect"})")));

  const auto mark = ParseServerMessage(R"({"type":"mark","name":"sentence-1"})");
  ASSERT_TRUE(std::holds_alternative<PlaybackMark>(mark));
  EXPECT_EQ(std::get<PlaybackMark>(mark).name, "sentence-1");

  const auto playback = ParseServerMessage(R"({"type":"start_playback","rate":8000,"channels":1})");
  ASSERT_TRUE(std::holds_alternative<StartPlayback>(playback));
  EXPECT_EQ(std::get<StartPlayback>(playback).sample_rate, 8000U);
  EXPECT_EQ(std::get<StartPlayback>(playback).channels, 1);
}

TEST(ProtocolParse, StartPlaybackFieldsAreOptional) {
  const auto playback = ParseServerMessage(R"({"type":"start_playback"})");
  ASSERT_TRUE(std::holds_alternative<StartPlayback>(playback));
  EXPECT_EQ(std::get<StartPlayback>(playback).sample_rate, 0U);
  EXPECT_EQ(std::get<StartPlayback>(playback).channels, 0);
}

TEST(ProtocolParse, MalformedControlMessagesAreInvalidNotRelayed) {
  EXPECT_TRUE(std::holds_alternative<InvalidMessage>(ParseServerMessage(R"({"type":"mark"})")));
  EXPECT_TRUE(
      std::holds_alternative<InvalidMessage>(ParseServerMessage(R"({"type":"mark","name":""})")));
  EXPECT_TRUE(
      std::holds_alternative<InvalidMessage>(ParseServerMessage(R"({"type":"mark","name":42})")));
  EXPECT_TRUE(std::holds_alternative<InvalidMessage>(
      ParseServerMessage(R"({"type":"start_playback","rate":-1})")));
  EXPECT_TRUE(std::holds_alternative<InvalidMessage>(
      ParseServerMessage(R"({"type":"start_playback","rate":16000.5})")));
  EXPECT_TRUE(std::holds_alternative<InvalidMessage>(
      ParseServerMessage(R"({"type":"start_playback","rate":48001})")));
  EXPECT_TRUE(std::holds_alternative<InvalidMessage>(
      ParseServerMessage(R"({"type":"start_playback","channels":3})")));
  EXPECT_TRUE(std::holds_alternative<InvalidMessage>(
      ParseServerMessage(R"({"type":"start_playback","rate":"16000"})")));
}

TEST(ProtocolParse, MarkNameAtBoundaryLengths) {
  const std::string max_name(256, 'x');
  const auto ok = ParseServerMessage(R"({"type":"mark","name":")" + max_name + R"("})");
  EXPECT_TRUE(std::holds_alternative<PlaybackMark>(ok));

  const std::string too_long(257, 'x');
  const auto bad = ParseServerMessage(R"({"type":"mark","name":")" + too_long + R"("})");
  EXPECT_TRUE(std::holds_alternative<InvalidMessage>(bad));
}

TEST(ProtocolParse, UnrecognizedJsonIsRelayedVerbatim) {
  const std::string transcript = R"({"type":"transcript","text":"hello world"})";
  const auto relayed = ParseServerMessage(transcript);
  ASSERT_TRUE(std::holds_alternative<RelayToApp>(relayed));
  EXPECT_EQ(std::get<RelayToApp>(relayed).payload, transcript);

  EXPECT_TRUE(std::holds_alternative<RelayToApp>(ParseServerMessage(R"({"no_type":true})")));
  EXPECT_TRUE(std::holds_alternative<RelayToApp>(ParseServerMessage(R"([1,2,3])")));
  EXPECT_TRUE(std::holds_alternative<RelayToApp>(ParseServerMessage(R"({"type":42})")));
}

TEST(ProtocolParse, HostileInputBecomesInvalidNeverCrashes) {
  EXPECT_TRUE(std::holds_alternative<InvalidMessage>(ParseServerMessage("")));
  EXPECT_TRUE(std::holds_alternative<InvalidMessage>(ParseServerMessage("{not json")));
  EXPECT_TRUE(std::holds_alternative<InvalidMessage>(ParseServerMessage("\xff\xfe\x00garbage")));

  const std::string deep(100000, '[');
  EXPECT_TRUE(std::holds_alternative<InvalidMessage>(ParseServerMessage(deep)));

  const std::string huge(std::size_t{65} * 1024, 'a');
  EXPECT_TRUE(std::holds_alternative<InvalidMessage>(ParseServerMessage(huge)));
}

TEST(ProtocolParse, BracketsInsideStringsDoNotCountAsNesting) {
  const std::string brackets(1000, '[');
  const auto relayed = ParseServerMessage(R"({"type":"note","text":")" + brackets + R"("})");
  EXPECT_TRUE(std::holds_alternative<RelayToApp>(relayed));
}

}  // namespace
}  // namespace audiofork
