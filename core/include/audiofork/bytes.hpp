#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace audiofork {

class ConstByteSpan {
 public:
  constexpr ConstByteSpan() noexcept = default;
  constexpr ConstByteSpan(const std::uint8_t* data, std::size_t size) noexcept
      : data_(data), size_(size) {}
  explicit ConstByteSpan(const std::vector<std::uint8_t>& bytes) noexcept
      : data_(bytes.data()), size_(bytes.size()) {}

  [[nodiscard]] constexpr const std::uint8_t* begin() const noexcept { return data_; }
  [[nodiscard]] constexpr const std::uint8_t* end() const noexcept { return data_ + size_; }
  [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }
  [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }

 private:
  const std::uint8_t* data_ = nullptr;
  std::size_t size_ = 0;
};

class MutableByteSpan {
 public:
  constexpr MutableByteSpan() noexcept = default;
  constexpr MutableByteSpan(std::uint8_t* data, std::size_t size) noexcept
      : data_(data), size_(size) {}
  explicit MutableByteSpan(std::vector<std::uint8_t>& bytes) noexcept
      : data_(bytes.data()), size_(bytes.size()) {}

  [[nodiscard]] constexpr std::uint8_t* begin() const noexcept { return data_; }
  [[nodiscard]] constexpr std::uint8_t* end() const noexcept { return data_ + size_; }
  [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }
  [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }

  [[nodiscard]] constexpr operator ConstByteSpan()
      const noexcept {  // NOLINT(google-explicit-constructor)
    return {data_, size_};
  }

 private:
  std::uint8_t* data_ = nullptr;
  std::size_t size_ = 0;
};

}  // namespace audiofork
