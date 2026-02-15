/// @file free_list.cpp
/// @brief Implementation of the first-fit free-list allocator.

#include "allocator/free_list.hpp"
#include "allocator/arena.hpp"

#include <bit>
#include <cstring>
#include <memory>
#include <new>

namespace mmap_viz {

FreeListAllocator::FreeListAllocator(Arena &arena) noexcept : arena_{arena} {
  // Initialize with a single free block spanning the entire arena.
  auto *block = new (arena_.base()) FreeBlock{
      .size = arena_.capacity(),
      .next = nullptr,
  };
  head_ = block;
  free_blocks_ = 1;
  largest_free_ = block->size;
  largest_free_dirty_ = false;
}

auto FreeListAllocator::allocate(std::size_t size, std::size_t alignment)
    -> std::expected<AllocationResult, AllocError> {
  if (size == 0) {
    size = 1; // Minimum allocation.
  }

  if (!std::has_single_bit(alignment)) {
    return std::unexpected(AllocError::InvalidAlignment);
  }

  // Ensure minimum block size so freed blocks can hold FreeBlock header.
  const auto min_size = std::max(size, kMinBlockSize);

  FreeBlock *prev = nullptr;
  FreeBlock *curr = head_;

  while (curr != nullptr) {
    auto *block_start = reinterpret_cast<std::byte *>(curr);
    auto *block_end = block_start + curr->size;

    // Calculate aligned address within this block.
    void *aligned_ptr = block_start;
    std::size_t space = curr->size;

    if (!std::align(alignment, min_size, aligned_ptr, space)) {
      // Block too small after alignment.
      prev = curr;
      curr = curr->next;
      continue;
    }

    auto *result_ptr = static_cast<std::byte *>(aligned_ptr);
    auto padding = static_cast<std::size_t>(result_ptr - block_start);
    auto actual_size = min_size + padding;

    if (curr->size < actual_size) {
      prev = curr;
      curr = curr->next;
      continue;
    }

    // Check if we can split the remainder.
    std::size_t remainder = curr->size - actual_size;

    if (remainder >= kMinBlockSize) {
      // Split: create a new free block after the allocation.
      auto *new_free = new (block_start + actual_size) FreeBlock{
          .size = remainder,
          .next = curr->next,
      };

      if (prev != nullptr) {
        prev->next = new_free;
      } else {
        head_ = new_free;
      }

      // Count stays same (1 used, 1 new free replacing old free)
      // But size changed, so largest might be invalid if we split the largest
      if (curr->size == largest_free_) {
        largest_free_dirty_ = true;
      }
    } else {
      // Absorb remainder into allocation (avoid tiny unusable fragments).
      actual_size = curr->size;

      if (prev != nullptr) {
        prev->next = curr->next;
      } else {
        head_ = curr->next;
      }

      free_blocks_--;
      if (curr->size == largest_free_) {
        largest_free_dirty_ = true;
      }
    }

    // Zero out the allocated region (security + determinism).
    std::memset(block_start, 0, actual_size);

    allocated_ += actual_size;

    auto offset = static_cast<std::size_t>(block_start - arena_.base());

    return AllocationResult{
        .ptr = result_ptr,
        .offset = offset,
        .actual_size = actual_size,
    };
  }

  return std::unexpected(AllocError::OutOfMemory);
}

auto FreeListAllocator::deallocate(std::byte *ptr, std::size_t size)
    -> std::expected<void, AllocError> {
  if (ptr == nullptr) {
    return {};
  }

  auto *base = arena_.base();
  auto *end = base + arena_.capacity();

  if (ptr < base || ptr >= end) {
    return std::unexpected(AllocError::BadPointer);
  }

  // Find the actual block start. We need to scan the free list to determine
  // the actual_size. For simplicity, we use the provided size rounded up to
  // kMinBlockSize.
  auto actual_size = std::max(size, kMinBlockSize);

  // Find insertion point to maintain address-ordered free list.
  auto *block_addr = reinterpret_cast<std::byte *>(ptr);

  // Walk back to find the true allocation offset if alignment padding was used.
  // Since we zeroed the block on alloc, and the free list is address-ordered,
  // we insert at the ptr address and use the size.

  FreeBlock *prev = nullptr;
  FreeBlock *curr = head_;

  // Find where this block falls in address order.
  while (curr != nullptr && reinterpret_cast<std::byte *>(curr) < block_addr) {
    prev = curr;
    curr = curr->next;
  }

  // Create the new free block at the deallocation address.
  auto *freed = new (block_addr) FreeBlock{
      .size = actual_size,
      .next = curr,
  };

  if (prev != nullptr) {
    prev->next = freed;
  } else {
    head_ = freed;
  }

  free_blocks_++;
  if (freed->size > largest_free_) {
    largest_free_ = freed->size;
    largest_free_dirty_ = false;
  }

  allocated_ -= actual_size;

  // Coalesce with next block if adjacent.
  if (curr != nullptr) {
    auto *freed_end = reinterpret_cast<std::byte *>(freed) + freed->size;
    if (freed_end == reinterpret_cast<std::byte *>(curr)) {
      freed->size += curr->size;
      freed->next = curr->next;
      free_blocks_--;
      if (freed->size > largest_free_) {
        largest_free_ = freed->size;
        largest_free_dirty_ = false;
      }
    }
  }

  // Coalesce with previous block if adjacent.
  if (prev != nullptr) {
    auto *prev_end = reinterpret_cast<std::byte *>(prev) + prev->size;
    if (prev_end == reinterpret_cast<std::byte *>(freed)) {
      prev->size += freed->size;
      prev->next = freed->next;
      free_blocks_--;
      if (prev->size > largest_free_) {
        largest_free_ = prev->size;
        largest_free_dirty_ = false;
      }
    }
  }

  return {};
}

auto FreeListAllocator::bytes_allocated() const noexcept -> std::size_t {
  return allocated_;
}

auto FreeListAllocator::bytes_free() const noexcept -> std::size_t {
  return arena_.capacity() - allocated_;
}

auto FreeListAllocator::largest_free_block() const noexcept -> std::size_t {
  if (largest_free_dirty_) {
    std::size_t largest = 0;
    for (auto *curr = head_; curr != nullptr; curr = curr->next) {
      if (curr->size > largest) {
        largest = curr->size;
      }
    }
    largest_free_ = largest;
    largest_free_dirty_ = false;
  }
  return largest_free_;
}

auto FreeListAllocator::free_block_count() const noexcept -> std::size_t {
  return free_blocks_;
}

auto FreeListAllocator::capacity() const noexcept -> std::size_t {
  return arena_.capacity();
}

} // namespace mmap_viz
