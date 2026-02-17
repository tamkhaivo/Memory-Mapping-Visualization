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
  /// @brief Construct a free-list allocator over the given memory range.
  /// @param base Start of the memory region.
  /// @param size Size of the memory region in bytes.
  FreeListAllocator(std::byte *base, std::size_t size) noexcept;

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

  /// @brief Base address of the arena.
  [[nodiscard]] auto base() const noexcept -> std::byte *;

  /// @brief Check if this allocator owns the given pointer.
  [[nodiscard]] bool contains(const void *ptr) const noexcept {
    const auto *p = reinterpret_cast<const std::byte *>(ptr);
    return p >= base_ && p < base_ + size_;
  }

private:
  enum class Color : bool { Red, Black };

  /// @brief Intrusive Red-Black Tree Node stored within free regions.
  struct FreeBlock {
    std::size_t size;        ///< Size of this block.
    FreeBlock *parent;       ///< Parent node.
    FreeBlock *left;         ///< Left child (lower addresses).
    FreeBlock *right;        ///< Right child (higher addresses).
    std::size_t subtree_max; ///< Max block size in this subtree.
    Color color;             ///< RB Color.
  };

  /// @brief Minimum size required to store a FreeBlock header.
  static constexpr std::size_t kMinBlockSize = sizeof(FreeBlock);

  // --- RB Tree Helpers ---
  void insert_node(FreeBlock *z);
  void delete_node(FreeBlock *z);
  void rb_transplant(FreeBlock *u, FreeBlock *v);
  void rb_insert_fixup(FreeBlock *z);
  void rb_delete_fixup(FreeBlock *x, FreeBlock *x_parent);
  void left_rotate(FreeBlock *x);
  void right_rotate(FreeBlock *x);

  /// @brief Updates subtree_max for x and its ancestors.
  void update_max(FreeBlock *x);
  void update_max_upwards(FreeBlock *x);

  /// @brief Finds the first block in address order that fits the size.
  [[nodiscard]] auto find_first_fit(std::size_t size) const -> FreeBlock *;

  [[nodiscard]] auto minimum(FreeBlock *x) const -> FreeBlock *;
  [[nodiscard]] auto maximum(FreeBlock *x) const -> FreeBlock *;
  [[nodiscard]] auto predecessor(FreeBlock *x) const -> FreeBlock *;
  [[nodiscard]] auto successor(FreeBlock *x) const -> FreeBlock *;

  std::byte *base_;
  std::size_t size_;
  FreeBlock *root_ = nullptr; ///< Root of the address-ordered RB tree.
  FreeBlock *nil_;            ///< Sentinel node for leaves.

  // Segregated Free Lists for small allocations (O(1))
  static constexpr std::size_t kSmallBlockQuantum = 16;
  static constexpr std::size_t kNumSmallClasses = 8;
  static constexpr std::size_t kMaxSmallBlockSize =
      kSmallBlockQuantum * kNumSmallClasses;

  struct FreeNode {
    FreeNode *next;
  };

  FreeNode *free_lists_[kNumSmallClasses];

  void verify_tree(FreeBlock *x) const;

  std::size_t allocated_ = 0;
  std::size_t free_blocks_ = 0;
};

} // namespace mmap_viz
