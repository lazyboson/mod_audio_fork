#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "audiofork/bytes.hpp"

namespace audiofork {

struct SlabPoolOptions {
  std::size_t slab_size_bytes = std::size_t{64} * 1024;
  std::size_t max_total_bytes = std::size_t{1536} * 1024 * 1024;
};

namespace detail {
struct SlabPoolState {
  explicit SlabPoolState(SlabPoolOptions opts) : options(opts) {}
  const SlabPoolOptions options;
  std::mutex mutex;
  std::vector<std::vector<std::uint8_t>> free_slabs;
  std::size_t allocated_bytes = 0;
  std::size_t leased_slabs = 0;
};
}  // namespace detail

// Returns its slab to the pool on destruction.
class SlabLease {
 public:
  SlabLease(SlabLease&& other) noexcept = default;
  SlabLease& operator=(SlabLease&& other) noexcept;
  SlabLease(const SlabLease&) = delete;
  SlabLease& operator=(const SlabLease&) = delete;
  ~SlabLease();

  [[nodiscard]] MutableByteSpan bytes() noexcept;

 private:
  friend class SlabPool;
  SlabLease(std::shared_ptr<detail::SlabPoolState> state,
            std::vector<std::uint8_t> storage) noexcept;
  void Release() noexcept;

  std::shared_ptr<detail::SlabPoolState> state_;
  std::vector<std::uint8_t> storage_;
};

// Copyable handle to shared pool state; leases keep the state alive, so a lease
// may safely outlive every pool handle.
class SlabPool {
 public:
  struct Stats {
    std::size_t slab_size_bytes;
    std::size_t allocated_bytes;
    std::size_t leased_slabs;
    std::size_t free_slabs;
  };

  [[nodiscard]] static std::optional<SlabPool> Create(SlabPoolOptions options);

  [[nodiscard]] std::optional<SlabLease> Acquire();
  [[nodiscard]] Stats stats() const;

 private:
  explicit SlabPool(std::shared_ptr<detail::SlabPoolState> state) noexcept;

  std::shared_ptr<detail::SlabPoolState> state_;
};

}  // namespace audiofork
