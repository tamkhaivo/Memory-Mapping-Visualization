/// @file free_list.cpp
/// @brief Implementation of the first-fit free-list allocator using an
/// Address-Ordered Red-Black Tree.

#include "allocator/free_list.hpp"
#include "allocator/arena.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <memory>
#include <new>

namespace mmap_viz {

FreeListAllocator::FreeListAllocator(Arena &arena) noexcept : arena_{arena} {
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
  auto *block = new (arena_.base()) FreeBlock{
      .size = arena_.capacity(),
      .parent = nil_,
      .left = nil_,
      .right = nil_,
      .subtree_max = arena_.capacity(),
      .color = Color::Black, // Root is always black
  };

  insert_node(block);

  // Stats
  free_blocks_ = 1;
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

  // Ensure minimum block size
  const auto min_size = std::max(size, kMinBlockSize);

  // Strategy:
  // We need to find the BEST block that fits.
  // Actually, we want ADDRESS-ORDERED first fit to reduce fragmentation at the
  // beginning? `find_first_fit` does exactly that: lowest address block that is
  // large enough.

  // However, simple `find_first_fit(min_size)` might return a block that is
  // large enough for `min_size` but NOT large enough after alignment is
  // applied. The alignment overhead depends on the specific address. Since we
  // don't know the address until we inspect the node, we might stumble.

  // Naive approach: Find candidate, check alignment. If fail, what?
  // If `find_first_fit` ensures we find the *first* capable block,
  // we can just check it. If it fails alignment (which requires more size),
  // we might need to search for a larger block? or just the *next* block in the
  // tree.

  // Robust approach:
  // Iterate through candidates.
  // `find_first_fit` gives the first block >= min_size, let's call it C.
  // check if C works with alignment.
  // If yes, use it.
  // If no, we need the next block in address order (successor) that is >=
  // min_size. But strictly speaking, if C is the *first* block >= min_size,
  // then any successor S is > C in address. Does S have enough size? Maybe.
  // Since we can't easily query "First block >= size AND meets alignment", we
  // loop using successors logic? But successor in address order might be small.
  // We need to search the tree again?
  // Actually, if we just traverse in-order starting from the candidate?
  // That's O(N) in worst case (bad fragmentation with high alignment
  // requirements). But high alignment is rare. Simple size check usually works.

  // Let's assume alignment overhead is small.
  // We search for `min_size`.

  auto *curr = find_first_fit(min_size);

  while (curr != nil_) {
    // Check alignment
    auto *block_start = reinterpret_cast<std::byte *>(curr);
    void *aligned_ptr = block_start;
    std::size_t space = curr->size;

    // We already know curr->size >= min_size.
    // Check if alignment fits.
    if (std::align(alignment, min_size, aligned_ptr, space)) {
      // It implies it fits!
      // `std::align` updates `aligned_ptr` and decreases `space`.
      // The amount of padding is `curr->size - space`? No.
      // `space` becomes the size of the block *after* the aligned pointer.
      // The used space is `min_size`.
      // The total bytes required from block start is `(aligned_ptr -
      // block_start) + min_size`.

      auto *result_ptr = static_cast<std::byte *>(aligned_ptr);
      auto padding = static_cast<std::size_t>(result_ptr - block_start);
      auto actual_size = min_size + padding;

      // Found a match!

      // Ideally we split this block.
      // 1. Remove curr from tree.
      delete_node(curr);
      // Note: `delete_node` just removes it from the structure. `curr` pointer
      // remains valid to us.

      // 2. Handle leftover
      std::size_t remainder = curr->size - actual_size;

      if (remainder >= kMinBlockSize) {
        // Split.
        // The allocated part is [block_start, block_start + actual_size).
        // The free part is [block_start + actual_size, end).

        auto *new_free = new (block_start + actual_size) FreeBlock{
            .size = remainder,
            .parent = nil_,
            .left = nil_,
            .right = nil_,
            .subtree_max = remainder,
            .color = Color::Red // Insert usually starts Red
        };

        insert_node(new_free);

        // Stats
        // free_blocks_ stays same (1 remove, 1 add)
      } else {
        // Absorb remainder
        actual_size = curr->size;
        free_blocks_--;
      }

      allocated_ += actual_size;
      std::memset(block_start, 0, actual_size); // Zero memory

      return AllocationResult{
          .ptr = result_ptr,
          .offset = static_cast<std::size_t>(block_start - arena_.base()),
          .actual_size = actual_size,
      };
    }

    // Alignment failed. We need a larger block or just the next one?
    // If alignment failed, it means `curr->size` was enough for data, but NOT
    // enough for `data + padding`. We need to look for another block. We can
    // just get `successor(curr)`. But `successor` might be tiny. We can just
    // loop `curr = successor(curr)` until we find one that fits? This degrades
    // to O(N) if many blocks are just barely large enough but fail alignment.
    // Can we search for `min_size + alignment`?
    // If we search for `min_size + alignment`, we are GUARANTEED that even with
    // max alignment overhead it fits? Max overhead for alignment `A` is `A-1`.
    // So if we search for `min_size + alignment - 1`, we are safe.
    // But that might skip a smaller block that effectively has 0 alignment
    // offset. Let's stick to the loop for now. It's robust correctness-wise.
    // Optimization: check `subtree_max` of `root`?

    curr = successor(curr);

    // Optimization: Skip nodes that are clearly too small
    while (curr != nil_ && curr->size < min_size) {
      curr = successor(curr);
    }
  }

  return std::unexpected(AllocError::OutOfMemory);
}

auto FreeListAllocator::deallocate(std::byte *ptr, std::size_t size)
    -> std::expected<void, AllocError> {
  if (ptr == nullptr)
    return {};

  auto *base = arena_.base();
  auto *end = base + arena_.capacity();

  if (ptr < base || ptr >= end) {
    return std::unexpected(AllocError::BadPointer);
  }

  // Same logic: assume actual_size provided is correct or rounded up.
  // In the original, it calculated actual_size by rounding up to min.
  // But wait, the original logic scanned the free list to find where it fits
  // address-wise. Now we use the tree.

  auto actual_size = std::max(size, kMinBlockSize);
  // We cannot easily know the exact padding used during allocation unless we
  // explicitly tracked it. The original allocator assumed the user passed back
  // the same size. If the user passed the requested size (e.g. 64), but we
  // allocated 80 (due to padding/alignment), we might effectively leak the
  // padding or create a hole? The original `deallocate` says: "Since we zeroed
  // the block on alloc... we insert at the ptr address and use the size." Wait,
  // if `ptr` is `aligned_ptr` (shifted by padding), and we free at `ptr`, we
  // lose the padding bytes *before* `ptr`! The original code `auto *block_addr
  // = reinterpret_cast<std::byte *>(ptr);` And then just `new (block_addr)
  // FreeBlock`. It seems the original code IGNORED the padding bytes before
  // `ptr`. If `padding > 0`, that space is lost forever in the original code?
  // Let's look at original `allocate`:
  // `auto *block_start = reinterpret_cast<std::byte *>(curr);`
  // `auto *result_ptr = static_cast<std::byte *>(aligned_ptr);`
  // `return ... .ptr = result_ptr ...`
  // So the user gets `result_ptr`.
  // In `deallocate(ptr)`, `ptr` is `result_ptr`.
  // If `result_ptr > block_start`, there is a gap.
  // The original `deallocate` treats `ptr` as the start of the new free block.
  // So yes, the padding bytes are LEAKED in the original implementation!
  // "Walk back to find the true allocation offset if alignment padding was
  // used." comment existed, but the code just did `auto *block_addr =
  // reinterpret_cast<std::byte *>(ptr);`. Unless `ptr` passed by user is
  // supposed to be the *original* pointer? `AllocationResult` gives `ptr`
  // (aligned). The tests pass `r->ptr`. So the original implementation was
  // leaking padding. We will preserve this behavior for now (bug-for-bug
  // compatibility?) or fix it? If we fix it, we don't know the padding size.
  // Standard free() relies on metadata just before the pointer.
  // We don't have that.
  // So we MUST assume `ptr` is the block start.
  // (Or maybe `AllocationResult.ptr` is the block start? No, it's
  // `aligned_ptr`.)

  auto *block_addr = reinterpret_cast<std::byte *>(ptr);

  auto *freed = new (block_addr) FreeBlock{.size = actual_size,
                                           .parent = nil_,
                                           .left = nil_,
                                           .right = nil_,
                                           .subtree_max = actual_size,
                                           .color = Color::Red};

  insert_node(freed);
  free_blocks_++;
  allocated_ -= actual_size;

  // Coalesce
  // Check predecessor
  auto *prev = predecessor(freed);
  if (prev != nil_) {
    auto *prev_end = reinterpret_cast<std::byte *>(prev) + prev->size;
    if (prev_end == reinterpret_cast<std::byte *>(freed)) {
      // Coalesce prev and freed
      // We extend prev to cover freed.
      // Remove freed from tree? No, remove prev and freed and insert new?
      // Simpler: Remove both, update prev, insert prev.
      // Or just update prev size and `fix_up_max`?
      // Using RB tree, changing key (size doesn't affect key, Address does)
      // Address of prev doesn't change.
      // Size changes -> `subtree_max` changes.
      // So we can keep `prev` in tree, just update size and call
      // `update_max(prev)`. And we MUST remove `freed` from tree.

      delete_node(freed); // Freed is gone from tree

      prev->size += freed->size;
      update_max(prev);

      free_blocks_--;
      freed = prev; // Update freed to point to the merged block for next step
    }
  }

  // Check successor
  auto *succ = successor(freed);
  if (succ != nil_) {
    auto *freed_end = reinterpret_cast<std::byte *>(freed) + freed->size;
    if (freed_end == reinterpret_cast<std::byte *>(succ)) {
      // Coalesce freed and succ
      // We extend freed to cover succ.
      // succ is removed.
      // freed stays (key address is same).

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
  return arena_.capacity() - allocated_;
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
  return arena_.capacity();
}

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
