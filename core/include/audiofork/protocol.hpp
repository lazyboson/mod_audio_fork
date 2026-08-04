#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace audiofork {

inline constexpr std::string_view kProtocolVersion = "1.0";

struct HelloParams {
  std::string_view call_sid;
  std::uint32_t sample_rate = 16000;
  std::uint8_t channels = 2;
  std::string_view metadata_json;
};

[[nodiscard]] std::optional<std::string> EncodeHello(const HelloParams& params);
[[nodiscard]] std::string EncodeResume(std::uint64_t gap_ms, std::uint64_t dropped_ms);
[[nodiscard]] std::optional<std::string> EncodeDtmf(char digit, std::uint32_t duration_ms);
[[nodiscard]] std::string EncodeBye();

struct StartPlayback {
  std::uint32_t sample_rate = 0;
  std::uint8_t channels = 0;
};
struct ClearPlayback {};
struct PlaybackMark {
  std::string name;
};
struct ServerDisconnect {};
struct RelayToApp {
  std::string payload;
};
struct InvalidMessage {
  std::string reason;
};

using ServerMessage = std::variant<StartPlayback, ClearPlayback, PlaybackMark, ServerDisconnect,
                                   RelayToApp, InvalidMessage>;

// Never throws for any input except allocation failure: hostile text from the
// wire must at worst produce InvalidMessage. Valid JSON that is not a
// recognized control message is relayed to the application verbatim.
[[nodiscard]] ServerMessage ParseServerMessage(std::string_view text);

}  // namespace audiofork
