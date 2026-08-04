#include "audiofork/slab_pool.hpp"

#include <utility>

namespace audiofork {

SlabLease::SlabLease(std::shared_ptr<detail::SlabPoolState> state,
                     std::vector<std::uint8_t> storage) noexcept
    : state_(std::move(state)), storage_(std::move(storage)) {}

SlabLease& SlabLease::operator=(SlabLease&& other) noexcept {
  if (this != &other) {
    Release();
    state_ = std::move(other.state_);
    storage_ = std::move(other.storage_);
  }
  return *this;
}

SlabLease::~SlabLease() { Release(); }

MutableByteSpan SlabLease::bytes() noexcept { return MutableByteSpan(storage_); }

ConstByteSpan SlabLease::bytes() const noexcept { return ConstByteSpan(storage_); }

// push_back cannot allocate: Create() reserved free_slabs capacity for every
// slab the cap allows, so the noexcept below cannot be violated
// NOLINTNEXTLINE(bugprone-exception-escape)
void SlabLease::Release() noexcept {
  if (!state_) {
    return;
  }
  {
    const std::scoped_lock lock(state_->mutex);
    state_->free_slabs.push_back(std::move(storage_));
    --state_->leased_slabs;
  }
  state_.reset();
}

SlabPool::SlabPool(std::shared_ptr<detail::SlabPoolState> state) noexcept
    : state_(std::move(state)) {}

std::optional<SlabPool> SlabPool::Create(SlabPoolOptions options) {
  if (options.slab_size_bytes == 0 || options.max_total_bytes < options.slab_size_bytes) {
    return std::nullopt;
  }
  auto state = std::make_shared<detail::SlabPoolState>(options);
  state->free_slabs.reserve(options.max_total_bytes / options.slab_size_bytes);
  return SlabPool(std::move(state));
}

std::optional<SlabLease> SlabPool::Acquire() {
  const std::size_t slab_size = state_->options.slab_size_bytes;
  std::vector<std::uint8_t> storage;
  {
    const std::scoped_lock lock(state_->mutex);
    if (!state_->free_slabs.empty()) {
      storage = std::move(state_->free_slabs.back());
      state_->free_slabs.pop_back();
    } else if (state_->allocated_bytes + slab_size <= state_->options.max_total_bytes) {
      state_->allocated_bytes += slab_size;
    } else {
      return std::nullopt;
    }
    ++state_->leased_slabs;
  }
  if (storage.empty()) {
    // fresh slab: allocate outside the lock so page faults don't serialize shards
    try {
      storage.resize(slab_size);
    } catch (...) {
      const std::scoped_lock lock(state_->mutex);
      state_->allocated_bytes -= slab_size;
      --state_->leased_slabs;
      throw;
    }
  }
  return SlabLease(state_, std::move(storage));
}

SlabPool::Stats SlabPool::stats() const {
  const std::scoped_lock lock(state_->mutex);
  return Stats{state_->options.slab_size_bytes, state_->allocated_bytes, state_->leased_slabs,
               state_->free_slabs.size()};
}

}  // namespace audiofork
