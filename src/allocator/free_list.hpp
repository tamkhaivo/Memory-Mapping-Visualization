#pragma once
/// @file free_list.hpp
/// @brief First-fit free-list allocator operating over an Arena.

#include <cstddef>
#include <expected>
#include <string>

namespace mmap_viz {

class Arena;

/// @brief Error codes specific to the free-list allocator.
enum class AllocError : std::uint8_t {
  OutOfMemory,
  InvalidAlignment,
  DoubleFree,
  BadPointer,
};

/// @brief Human-readable description of an AllocError.
[[nodiscard]] constexpr auto to_string(AllocError e) -> const char * {
  switch (e) {
  case AllocError::OutOfMemory:
    return "out of memory";
  case AllocError::InvalidAlignment:
    return "invalid alignment (must be power of 2)";
  case AllocError::DoubleFree:
    return "double free detected";
  case AllocError::BadPointer:
    return "pointer not owned by this allocator";
  }
  return "unknown";
}

/// @brief Result of a successful allocation.
struct AllocationResult {
  std::byte *ptr;          ///< Pointer to the allocated region.
  std::size_t offset;      ///< Offset from the arena base.
  std::size_t actual_size; ///< Size including alignment padding.
};

/// @brief First-fit free-list allocator backed by an Arena.
///
/// Maintains an intrusive linked list of free blocks stored within
/// the free regions themselves (zero metadata overhead for free blocks).
/// Supports coalescing on deallocate and splitting on allocate.
class FreeListAllocator {
public:
  /// @brief Construct a free-list allocator over the given arena.
  /// @param arena Reference to the backing Arena (must outlive this allocator).
  explicit FreeListAllocator(Arena &arena) noexcept;

  // Non-copyable, non-movable (references an arena).
  FreeListAllocator(const FreeListAllocator &) = delete;
  FreeListAllocator &operator=(const FreeListAllocator &) = delete;
  FreeListAllocator(FreeListAllocator &&) = delete;
  FreeListAllocator &operator=(FreeListAllocator &&) = delete;

  /// @brief Allocate a block of at least @p size bytes with given @p alignment.
  /// @param size     Requested size in bytes (must be > 0).
  /// @param alignment Required alignment (must be power of 2, default 16).
  /// @return AllocationResult on success, AllocError on failure.
  [[nodiscard]] auto allocate(std::size_t size,
                              std::size_t alignment = alignof(std::max_align_t))
      -> std::expected<AllocationResult, AllocError>;

  /// @brief Deallocate a previously allocated block.
  /// @param ptr  Pointer returned by allocate().
  /// @param size Size passed to allocate().
  /// @return AllocError if the pointer is invalid.
  auto deallocate(std::byte *ptr, std::size_t size)
      -> std::expected<void, AllocError>;

  /// @brief Total bytes currently allocated (not including free-list overhead).
  [[nodiscard]] auto bytes_allocated() const noexcept -> std::size_t;

  /// @brief Total bytes currently free.
  [[nodiscard]] auto bytes_free() const noexcept -> std::size_t;

  /// @brief Size of the largest contiguous free block.
  [[nodiscard]] auto largest_free_block() const noexcept -> std::size_t;

  /// @brief Number of free blocks in the list (fragmentation indicator).
  [[nodiscard]] auto free_block_count() const noexcept -> std::size_t;

  /// @brief Total capacity of the backing arena.
  [[nodiscard]] auto capacity() const noexcept -> std::size_t;

private:
  /// @brief Intrusive free-block header stored within free regions.
  struct FreeBlock {
    std::size_t size; ///< Total size of this free block (including header).
    FreeBlock *next;  ///< Next free block in the list, or nullptr.
  };

  static constexpr std::size_t kMinBlockSize = sizeof(FreeBlock);

  Arena &arena_;
  FreeBlock *head_ = nullptr;
  std::size_t allocated_ = 0;
};

} // namespace mmap_viz
