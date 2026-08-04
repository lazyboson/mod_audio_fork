#include <cstddef>
#include <cstdint>
#include <string_view>

#include "audiofork/protocol.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view text(reinterpret_cast<const char*>(data), size);
  const auto message = audiofork::ParseServerMessage(text);
  (void)message;
  return 0;
}
