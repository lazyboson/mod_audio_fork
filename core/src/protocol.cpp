#include "audiofork/protocol.hpp"

#include <algorithm>
#include <cstddef>
#include <nlohmann/json.hpp>
#include <utility>

namespace audiofork {
namespace {

using nlohmann::json;

constexpr std::size_t kMaxMessageBytes = std::size_t{64} * 1024;
constexpr int kMaxNestingDepth = 64;
constexpr std::size_t kMaxMarkNameBytes = 256;
constexpr std::uint32_t kMinSampleRate = 8000;
constexpr std::uint32_t kMaxSampleRate = 48000;
constexpr std::string_view kDtmfDigits = "0123456789*#ABCD";

// nlohmann's parser recurses per nesting level, so hostile input like ~32k
// opening brackets can overflow the stack before parse() reports anything.
// Brackets inside string literals must not count.
bool NestingTooDeep(std::string_view text) {
  int depth = 0;
  bool in_string = false;
  bool escaped = false;
  for (const char character : text) {
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (character == '\\') {
        escaped = true;
      } else if (character == '"') {
        in_string = false;
      }
      continue;
    }
    switch (character) {
      case '"':
        in_string = true;
        break;
      case '{':
      case '[':
        if (++depth > kMaxNestingDepth) {
          return true;
        }
        break;
      case '}':
      case ']':
        depth = std::max(depth - 1, 0);
        break;
      default:
        break;
    }
  }
  return false;
}

std::string Dump(const json& value) {
  return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

bool ValidSampleRate(std::uint32_t rate) {
  return rate >= kMinSampleRate && rate <= kMaxSampleRate;
}

}  // namespace

std::optional<std::string> EncodeHello(const HelloParams& params) {
  if (params.call_sid.empty() || !ValidSampleRate(params.sample_rate) || params.channels < 1 ||
      params.channels > 2) {
    return std::nullopt;
  }
  json message{{"type", "hello"},
               {"version", kProtocolVersion},
               {"callSid", params.call_sid},
               {"rate", params.sample_rate},
               {"channels", params.channels},
               {"encoding", "L16"}};
  if (!params.metadata_json.empty()) {
    json metadata = json::parse(params.metadata_json, nullptr, false);
    if (metadata.is_discarded()) {
      return std::nullopt;
    }
    message["metadata"] = std::move(metadata);
  }
  return Dump(message);
}

std::string EncodeResume(std::uint64_t gap_ms, std::uint64_t dropped_ms) {
  return Dump(json{{"type", "resume"}, {"gapMs", gap_ms}, {"droppedMs", dropped_ms}});
}

std::optional<std::string> EncodeDtmf(char digit, std::uint32_t duration_ms) {
  if (kDtmfDigits.find(digit) == std::string_view::npos) {
    return std::nullopt;
  }
  return Dump(
      json{{"type", "dtmf"}, {"digit", std::string(1, digit)}, {"durationMs", duration_ms}});
}

std::string EncodeBye() { return Dump(json{{"type", "bye"}}); }

ServerMessage ParseServerMessage(std::string_view text) {
  if (text.size() > kMaxMessageBytes) {
    return InvalidMessage{"message exceeds size limit"};
  }
  if (NestingTooDeep(text)) {
    return InvalidMessage{"nesting exceeds depth limit"};
  }
  const json message = json::parse(text, nullptr, false);
  if (message.is_discarded()) {
    return InvalidMessage{"not valid JSON"};
  }
  if (!message.is_object()) {
    return RelayToApp{std::string(text)};
  }
  const auto type_it = message.find("type");
  if (type_it == message.end() || !type_it->is_string()) {
    return RelayToApp{std::string(text)};
  }
  const auto& type = type_it->get_ref<const std::string&>();

  if (type == "clear") {
    return ClearPlayback{};
  }
  if (type == "disconnect") {
    return ServerDisconnect{};
  }
  if (type == "mark") {
    const auto name_it = message.find("name");
    if (name_it == message.end() || !name_it->is_string()) {
      return InvalidMessage{"mark requires a string name"};
    }
    const auto& name = name_it->get_ref<const std::string&>();
    if (name.empty() || name.size() > kMaxMarkNameBytes) {
      return InvalidMessage{"mark name length out of bounds"};
    }
    return PlaybackMark{name};
  }
  if (type == "start_playback") {
    StartPlayback playback;
    if (const auto rate_it = message.find("rate"); rate_it != message.end()) {
      if (!rate_it->is_number_unsigned()) {
        return InvalidMessage{"start_playback rate must be an unsigned integer"};
      }
      const auto rate = rate_it->get<std::uint64_t>();
      if (!ValidSampleRate(static_cast<std::uint32_t>(rate)) || rate > kMaxSampleRate) {
        return InvalidMessage{"start_playback rate out of range"};
      }
      playback.sample_rate = static_cast<std::uint32_t>(rate);
    }
    if (const auto channels_it = message.find("channels"); channels_it != message.end()) {
      if (!channels_it->is_number_unsigned()) {
        return InvalidMessage{"start_playback channels must be an unsigned integer"};
      }
      const auto channels = channels_it->get<std::uint64_t>();
      if (channels < 1 || channels > 2) {
        return InvalidMessage{"start_playback channels out of range"};
      }
      playback.channels = static_cast<std::uint8_t>(channels);
    }
    return playback;
  }
  return RelayToApp{std::string(text)};
}

}  // namespace audiofork
