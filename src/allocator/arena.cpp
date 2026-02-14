/// @file arena.cpp
/// @brief Implementation of the mmap-backed Arena.

#include "allocator/arena.hpp"

#include <cerrno>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>

namespace mmap_viz {

auto Arena::page_size() noexcept -> std::size_t {
  static const auto ps = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
  return ps;
}

auto Arena::create(std::size_t capacity)
    -> std::expected<Arena, std::error_code> {
  if (capacity == 0) {
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  }

  // Round up to page boundary.
  const auto ps = page_size();
  const auto aligned_capacity = ((capacity + ps - 1) / ps) * ps;

  void *ptr = ::mmap(nullptr, aligned_capacity, PROT_READ | PROT_WRITE,
                     MAP_ANONYMOUS | MAP_PRIVATE,
                     -1, // No file descriptor.
                     0   // No offset.
  );

  if (ptr == MAP_FAILED) {
    return std::unexpected(std::make_error_code(static_cast<std::errc>(errno)));
  }

  return Arena{static_cast<std::byte *>(ptr), aligned_capacity};
}

Arena::Arena(std::byte *base, std::size_t capacity) noexcept
    : base_{base}, capacity_{capacity} {}

Arena::~Arena() {
  if (base_ != nullptr) {
    ::munmap(base_, capacity_);
  }
}

Arena::Arena(Arena &&other) noexcept
    : base_{std::exchange(other.base_, nullptr)},
      capacity_{std::exchange(other.capacity_, 0)} {}

Arena &Arena::operator=(Arena &&other) noexcept {
  if (this != &other) {
    if (base_ != nullptr) {
      ::munmap(base_, capacity_);
    }
    base_ = std::exchange(other.base_, nullptr);
    capacity_ = std::exchange(other.capacity_, 0);
  }
  return *this;
}

auto Arena::base() const noexcept -> std::byte * { return base_; }

auto Arena::capacity() const noexcept -> std::size_t { return capacity_; }

} // namespace mmap_viz
