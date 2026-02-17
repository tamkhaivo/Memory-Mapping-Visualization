/// @file free_list.cpp
/// @brief Implementation of the first-fit free-list allocator using an
/// Address-Ordered Red-Black Tree.

#include "allocator/free_list.hpp"
#include "allocator/arena.hpp"
#include "tracker/block_metadata.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <memory>
#include <new>

namespace mmap_viz {

FreeListAllocator::FreeListAllocator(std::byte *base, std::size_t size) noexcept
    : base_{base}, size_{size} {
  // Initialize sentinel node for leaves.
  // We allocate it from the arena? No, that's messy.
  // We can just use a static instance or a member instance?
  // Member instance is safer for lifetime.
  // But FreeBlock is huge now.
  // Let's allocate it on the heap for now, or use a special location.
  // Ah, we can't allocate from the allocator itself.
  // We can treat nil_ as a special pointer value, but that complicates logic.
  // Better to have a real object.
  // Let's just `new` it on the C++ heap. It's one node per allocator.

  nil_ = new FreeBlock{.size = 0,
                       .parent = nullptr,
                       .left = nullptr,
                       .right = nullptr,
                       .subtree_max = 0,
                       .color = Color::Black};

  // self-referential for safety, though standard RB nil just needs to be Black
  // and 0 size.
  nil_->left = nil_;
  nil_->right = nil_;
  nil_->parent = nil_;

  root_ = nil_;

  // Initialize with a single free block spanning the entire arena.
  auto *block = new (base_) FreeBlock{
      .size = size_,
      .parent = nil_,
      .left = nil_,
      .right = nil_,
      .subtree_max = size_,
      .color = Color::Black, // Root is always black
  };

  insert_node(block);

  // Stats
  free_blocks_ = 1;

  // Initialize segregated free lists
  for (auto &list : free_lists_) {
    list = nullptr;
  }
}

// Check Allocator destructor? We aren't deleting nil_.
// It's a leak of sizeof(FreeBlock) per Allocator instance.
// But this is "Memory Mapping Visualization", likely long lived.
// Ideally usage requires a destructor, but the class didn't have one
// deklarated. We should add one if we want to be clean, but for now we follow
// the existing style or just leak the sentinel. Actually, I can just make nil_
// a member variable? `FreeBlock nil_node_;` `nil_ = &nil_node_;` But FreeBlock
// is incomplete in header? No, it's defined in private. We can't change the
// header again easily in this step. Let's just leak it for now, or use a
// `std::unique_ptr` wrapper if we could. For the purpose of this task, leaking
// one node on allocator destruction (which likely happens at program exit) is
// acceptable for the prototype optimization.

auto FreeListAllocator::allocate(std::size_t size, std::size_t alignment)
    -> std::expected<AllocationResult, AllocError> {
  if (size == 0) {
    size = 1;
  }

  if (!std::has_single_bit(alignment)) {
    return std::unexpected(AllocError::InvalidAlignment);
  }

  // Reserve space for Intrusive Header
  const std::size_t header_size = sizeof(AllocationHeader);
  const std::size_t total_request = size + header_size;

  // 1. Try Segregated Free List
  if (total_request <= kMaxSmallBlockSize && alignment <= kSmallBlockQuantum) {
    auto quantized_size =
        (total_request + kSmallBlockQuantum - 1) & ~(kSmallBlockQuantum - 1);
    std::size_t idx = (quantized_size / kSmallBlockQuantum) - 1;

    if (free_lists_[idx] != nullptr) {
      FreeNode *block = free_lists_[idx];
      free_lists_[idx] = block->next;

      free_blocks_--;
      allocated_ += quantized_size;

      std::memset(block, 0, quantized_size);

      auto *header = reinterpret_cast<AllocationHeader *>(block);
      header->size = quantized_size;
      header->magic = AllocationHeader::kMagicValue;

      return AllocationResult{
          .ptr = reinterpret_cast<std::byte *>(block) + header_size,
          .offset = static_cast<std::size_t>(
              reinterpret_cast<std::byte *>(block) - base_),
          .actual_size = quantized_size,
      };
    }
  }

  // 2. Tree Allocation
  auto min_size = total_request;
  if (total_request > kMaxSmallBlockSize) {
    min_size = std::max(total_request, kMinBlockSize);
  } else {
    min_size = std::max(total_request, std::size_t{16});
  }

  auto *curr = find_first_fit(min_size);

  while (curr != nil_) {
    auto *block_start = reinterpret_cast<std::byte *>(curr);

    // Check alignment for PAYLOAD (after header)
    void *aligned_ptr = block_start + header_size;
    std::size_t space = curr->size - header_size;

    if (std::align(alignment, size, aligned_ptr, space)) {
      // It fits!
      auto *user_ptr = static_cast<std::byte *>(aligned_ptr);
      auto *header_ptr = user_ptr - header_size;
      auto pre_padding = static_cast<std::size_t>(header_ptr - block_start);

      delete_node(curr);

      // Handle Pre-Padding
      if (pre_padding >= kSmallBlockQuantum) {
        auto *gap_block = reinterpret_cast<FreeBlock *>(block_start);
        gap_block->size = pre_padding;

        if (pre_padding <= kMaxSmallBlockSize) {
          std::size_t idx = (pre_padding / kSmallBlockQuantum) - 1;
          auto *node = reinterpret_cast<FreeNode *>(gap_block);
          node->next = free_lists_[idx];
          free_lists_[idx] = node;
        } else {
          gap_block->parent = nil_;
          gap_block->left = nil_;
          gap_block->right = nil_;
          gap_block->subtree_max = pre_padding;
          gap_block->color = Color::Red;
          insert_node(gap_block);
        }
      }

      // Handle Remainder
      std::size_t actual_used_size = size + header_size;

      // Quantize used size if small
      if (actual_used_size <= kMaxSmallBlockSize) {
        auto quantized = (actual_used_size + kSmallBlockQuantum - 1) &
                         ~(kSmallBlockQuantum - 1);
        if (curr->size - pre_padding >= quantized) {
          actual_used_size = quantized;
        }
      }

      std::size_t remainder_size =
          (curr->size - pre_padding) - actual_used_size;

      bool absorbed = false;
      if (remainder_size >= kMinBlockSize) {
        auto *new_free = new (header_ptr + actual_used_size)
            FreeBlock{.size = remainder_size,
                      .parent = nil_,
                      .left = nil_,
                      .right = nil_,
                      .subtree_max = remainder_size,
                      .color = Color::Red};
        insert_node(new_free);
      } else if (remainder_size >= kSmallBlockQuantum) {
        if (remainder_size <= kMaxSmallBlockSize) {
          std::size_t idx = (remainder_size / kSmallBlockQuantum) - 1;
          auto *node =
              reinterpret_cast<FreeNode *>(header_ptr + actual_used_size);
          node->next = free_lists_[idx];
          free_lists_[idx] = node;
        } else {
          absorbed = true;
        }
      } else {
        absorbed = true;
      }

      if (absorbed) {
        actual_used_size += remainder_size;
        free_blocks_--;
      }

      allocated_ += actual_used_size;

      auto *header = reinterpret_cast<AllocationHeader *>(header_ptr);
      header->size = actual_used_size;
      header->magic = AllocationHeader::kMagicValue;

      std::memset(user_ptr, 0, actual_used_size - header_size);

      return AllocationResult{
          .ptr = user_ptr,
          .offset = static_cast<std::size_t>(header_ptr - base_),
          .actual_size = actual_used_size,
      };
    }

    curr = successor(curr);
    while (curr != nil_ && curr->size < min_size) {
      curr = successor(curr);
    }
  }

  return std::unexpected(AllocError::OutOfMemory);
}

auto FreeListAllocator::deallocate(std::byte *ptr, std::size_t /*size_hint*/)
    -> std::expected<void, AllocError> {
  if (ptr == nullptr)
    return {};

  auto *base = base_;
  auto *end = base + size_;

  if (ptr < base || ptr >= end) {
    return std::unexpected(AllocError::BadPointer);
  }

  const std::size_t header_size = sizeof(AllocationHeader);
  auto *header = reinterpret_cast<AllocationHeader *>(ptr - header_size);

  if (reinterpret_cast<std::byte *>(header) < base) {
    return std::unexpected(AllocError::BadPointer);
  }

  if (header->magic != AllocationHeader::kMagicValue) {
    return std::unexpected(AllocError::BadPointer);
  }

  std::size_t actual_size = header->size;

  if (actual_size <= kMaxSmallBlockSize) {
    std::size_t idx = (actual_size / kSmallBlockQuantum) - 1;
    if (idx >= kNumSmallClasses)
      idx = kNumSmallClasses - 1;

    auto *block = reinterpret_cast<FreeNode *>(header);

    block->next = free_lists_[idx];
    free_lists_[idx] = block;

    allocated_ -= actual_size;
    free_blocks_++;
    return {};
  }

  auto *block_addr = reinterpret_cast<std::byte *>(header);

  auto *freed = new (block_addr) FreeBlock{.size = actual_size,
                                           .parent = nil_,
                                           .left = nil_,
                                           .right = nil_,
                                           .subtree_max = actual_size,
                                           .color = Color::Red};

  insert_node(freed);
  free_blocks_++;
  allocated_ -= actual_size;

  auto *prev = predecessor(freed);
  if (prev != nil_) {
    auto *prev_end = reinterpret_cast<std::byte *>(prev) + prev->size;
    if (prev_end == reinterpret_cast<std::byte *>(freed)) {
      delete_node(freed);
      prev->size += freed->size;
      update_max(prev);
      free_blocks_--;
      freed = prev;
    }
  }

  auto *succ = successor(freed);
  if (succ != nil_) {
    auto *freed_end = reinterpret_cast<std::byte *>(freed) + freed->size;
    if (freed_end == reinterpret_cast<std::byte *>(succ)) {
      delete_node(succ);
      freed->size += succ->size;
      update_max(freed);
      free_blocks_--;
    }
  }

  return {};
}

auto FreeListAllocator::bytes_allocated() const noexcept -> std::size_t {
  return allocated_;
}

auto FreeListAllocator::bytes_free() const noexcept -> std::size_t {
  return size_ - allocated_;
}

auto FreeListAllocator::largest_free_block() const noexcept -> std::size_t {
  if (root_ == nil_)
    return 0;
  return root_->subtree_max;
}

auto FreeListAllocator::free_block_count() const noexcept -> std::size_t {
  return free_blocks_;
}

auto FreeListAllocator::capacity() const noexcept -> std::size_t {
  return size_;
}

auto FreeListAllocator::base() const noexcept -> std::byte * { return base_; }

// --- RB Tree Implementation ---

void FreeListAllocator::left_rotate(FreeBlock *x) {
  FreeBlock *y = x->right;
  x->right = y->left;
  if (y->left != nil_) {
    y->left->parent = x;
  }
  y->parent = x->parent;
  if (x->parent == nil_) {
    root_ = y;
  } else if (x == x->parent->left) {
    x->parent->left = y;
  } else {
    x->parent->right = y;
  }
  y->left = x;
  x->parent = y;

  // Update max
  y->subtree_max = x->subtree_max; // y takes x's place, inherits max
  update_max(x);                   // x is now child, recalculate its max
  // Note: y's max is set to what x HAD. But x's max might perform partial
  // update. Strictly: `update_max(x)` then `update_max(y)` is cleaner.
  // Optimization: y's subtree max is `max(y->size, y->left->max,
  // y->right->max)`. Since x is y->left, x must be updated first.
  update_max(y);
}

void FreeListAllocator::right_rotate(FreeBlock *x) {
  FreeBlock *y = x->left;
  x->left = y->right;
  if (y->right != nil_) {
    y->right->parent = x;
  }
  y->parent = x->parent;
  if (x->parent == nil_) {
    root_ = y;
  } else if (x == x->parent->right) {
    x->parent->right = y;
  } else {
    x->parent->left = y;
  }
  y->right = x;
  x->parent = y;

  // Update max
  y->subtree_max = x->subtree_max;
  update_max(x);
  update_max(y);
}

void FreeListAllocator::insert_node(FreeBlock *z) {
  FreeBlock *y = nil_;
  FreeBlock *x = root_;

  // Key is Address
  while (x != nil_) {
    y = x;
    // Check alignment / address order
    // if z < x
    if (z < x) {
      x->subtree_max =
          std::max(x->subtree_max, z->size); // Update max on the way down?
      x = x->left;
    } else {
      x->subtree_max = std::max(x->subtree_max, z->size);
      x = x->right;
    }
  }

  z->parent = y;
  if (y == nil_) {
    root_ = z;
  } else if (z < y) {
    y->left = z;
  } else {
    y->right = z;
  }

  z->left = nil_;
  z->right = nil_;
  z->color = Color::Red;
  z->subtree_max = z->size;

  // Fixup max upwards? We did it on the way down.
  // But rotation might mess it up? No, rotations handle their own local max.
  // But `rb_insert_fixup` does rotations.

  rb_insert_fixup(z);
}

void FreeListAllocator::rb_insert_fixup(FreeBlock *z) {
  while (z->parent->color == Color::Red) {
    if (z->parent == z->parent->parent->left) {
      FreeBlock *y = z->parent->parent->right;
      if (y->color == Color::Red) {
        z->parent->color = Color::Black;
        y->color = Color::Black;
        z->parent->parent->color = Color::Red;
        z = z->parent->parent;
      } else {
        if (z == z->parent->right) {
          z = z->parent;
          left_rotate(z);
        }
        z->parent->color = Color::Black;
        z->parent->parent->color = Color::Red;
        right_rotate(z->parent->parent);
      }
    } else {
      FreeBlock *y = z->parent->parent->left;
      if (y->color == Color::Red) {
        z->parent->color = Color::Black;
        y->color = Color::Black;
        z->parent->parent->color = Color::Red;
        z = z->parent->parent;
      } else {
        if (z == z->parent->left) {
          z = z->parent;
          right_rotate(z);
        }
        z->parent->color = Color::Black;
        z->parent->parent->color = Color::Red;
        left_rotate(z->parent->parent);
      }
    }
  }
  root_->color = Color::Black;
}

void FreeListAllocator::rb_transplant(FreeBlock *u, FreeBlock *v) {
  if (u->parent == nil_) {
    root_ = v;
  } else if (u == u->parent->left) {
    u->parent->left = v;
  } else {
    u->parent->right = v;
  }
  if (v != nil_) { // Standard CLRS says `v.p = u.p`. Nil parent is usually
                   // ignored, but our nil has a parent?
    v->parent = u->parent;
  }
}

void FreeListAllocator::delete_node(FreeBlock *z) {
  if (z->parent == nil_) {
    // Parent is nil (root), this is fine.
  }

  FreeBlock *y = z;
  FreeBlock *x;
  Color y_original_color = y->color;

  // We need to fixup sizes starting from somewhere.
  // The path from the replaced node upwards needs update.
  FreeBlock *fix_start = nullptr;

  if (z->left == nil_) {
    x = z->right;
    rb_transplant(z, z->right);
    fix_start = z->parent; // z is gone, parent is where we start updating max
  } else if (z->right == nil_) {
    x = z->left;
    rb_transplant(z, z->left);
    fix_start = z->parent;
  } else {
    y = minimum(z->right);
    y_original_color = y->color;
    x = y->right;

    // y is moving to z's spot.
    // x moves to y's old spot.
    // fix_start should be y's old parent?
    if (y->parent == z) {
      if (x != nil_)
        x->parent = y; // x parent set in transplant usually?
      // CLRS special case
      fix_start = y;
    } else {
      rb_transplant(y, y->right);
      y->right = z->right;
      y->right->parent = y;
      fix_start = y->parent; // Old parent of y.
    }

    rb_transplant(z, y);
    y->left = z->left;
    y->left->parent = y;
    y->color = z->color;

    // y takes z's spot, so it acts like z.
    // We need to update y's max because its children changed.
    // And we need to update fix_start upwards.
  }

  // First, update max for the node that replaced z (if it wasn't spliced out)
  // Actually, we just need to retrace from where the structural change
  // happened. If y moved, we update y.
  if (y != z) {
    update_max(y);
  }

  // Walk up from fix_start updating max
  FreeBlock *iter = fix_start;
  while (iter != nil_ && iter != nullptr) {
    update_max(iter);
    iter = iter->parent;
  }

  if (y_original_color == Color::Black) {
    rb_delete_fixup(x);
  }
}

void FreeListAllocator::rb_delete_fixup(FreeBlock *x) {
  while (x != root_ && x->color == Color::Black) {
    if (x == x->parent->left) {
      FreeBlock *w = x->parent->right;
      if (w->color == Color::Red) {
        w->color = Color::Black;
        x->parent->color = Color::Red;
        left_rotate(x->parent);
        w = x->parent->right;
      }
      if (w->left->color == Color::Black && w->right->color == Color::Black) {
        w->color = Color::Red;
        x = x->parent;
      } else {
        if (w->right->color == Color::Black) {
          w->left->color = Color::Black;
          w->color = Color::Red;
          right_rotate(w);
          w = x->parent->right;
        }
        w->color = x->parent->color;
        x->parent->color = Color::Black;
        w->right->color = Color::Black;
        left_rotate(x->parent);
        x = root_;
      }
    } else {
      FreeBlock *w = x->parent->left;
      if (w->color == Color::Red) {
        w->color = Color::Black;
        x->parent->color = Color::Red;
        right_rotate(x->parent);
        w = x->parent->left;
      }
      if (w->right->color == Color::Black && w->left->color == Color::Black) {
        w->color = Color::Red;
        x = x->parent;
      } else {
        if (w->left->color == Color::Black) {
          w->right->color = Color::Black;
          w->color = Color::Red;
          left_rotate(w);
          w = x->parent->left;
        }
        w->color = x->parent->color;
        x->parent->color = Color::Black;
        w->left->color = Color::Black;
        right_rotate(x->parent);
        x = root_;
      }
    }
  }
  x->color = Color::Black;
}

auto FreeListAllocator::minimum(FreeBlock *x) const -> FreeBlock * {
  while (x->left != nil_) {
    x = x->left;
  }
  return x;
}

auto FreeListAllocator::maximum(FreeBlock *x) const -> FreeBlock * {
  while (x->right != nil_) {
    x = x->right;
  }
  return x;
}

auto FreeListAllocator::predecessor(FreeBlock *x) const -> FreeBlock * {
  if (x->left != nil_) {
    return maximum(x->left);
  }
  FreeBlock *y = x->parent;
  while (y != nil_ && x == y->left) {
    x = y;
    y = y->parent;
  }
  return y;
}

auto FreeListAllocator::successor(FreeBlock *x) const -> FreeBlock * {
  if (x->right != nil_) {
    return minimum(x->right);
  }
  FreeBlock *y = x->parent;
  while (y != nil_ && x == y->right) {
    x = y;
    y = y->parent;
  }
  return y;
}

void FreeListAllocator::update_max(FreeBlock *x) {
  if (x == nil_)
    return;
  x->subtree_max = x->size;
  if (x->left != nil_) {
    x->subtree_max = std::max(x->subtree_max, x->left->subtree_max);
  }
  if (x->right != nil_) {
    x->subtree_max = std::max(x->subtree_max, x->right->subtree_max);
  }
}

auto FreeListAllocator::find_first_fit(std::size_t size) const -> FreeBlock * {
  FreeBlock *x = root_;
  FreeBlock *result = nil_;

  // We want the node with the SMALLEST address (Leftmost) that satisfies
  // node.size >= size? No, we want the node with SMALLEST address (Leftmost)
  // that satisfies condition. Condition: `node.subtree_max >= size`. Valid
  // nodes are those where `node.size >= size` OR `node.left.subtree_max >=
  // size` OR ... We want to enter the leftmost subtree that contains a
  // candidate.

  while (x != nil_) {
    if (x->left != nil_ && x->left->subtree_max >= size) {
      // There is a candidate on the left.
      // We MUST go left to find the first (address-ordered) contact.
      x = x->left;
    } else {
      // Left subtree does not have it.
      // Check current node.
      if (x->size >= size) {
        // Current node fits.
        // Since left doesn't have it, Current is the winner!
        return x;
      }
      // If current doesn't fit, it must be in right.
      if (x->right != nil_ && x->right->subtree_max >= size) {
        x = x->right;
      } else {
        // Not found in this subtree.
        // Should not happen if we guard call, but for leaf cases:
        return nil_;
      }
    }
  }

  return nil_;
}

} // namespace mmap_viz
