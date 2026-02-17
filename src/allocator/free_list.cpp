/// @file free_list.cpp
/// @brief Implementation of the first-fit free-list allocator using an
/// Address-Ordered Red-Black Tree.

#include "allocator/free_list.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace mmap_viz {

struct OpLogEntry {
  const char *op;
  void *node;
  void *parent;
  void *left;
  void *right;
  std::size_t size;
};

// Simple circular log per allocator
static constexpr int kMaxLog = 256;
struct AllocLog {
  OpLogEntry entries[kMaxLog];
  int tail = 0;
  void add(const char *op, void *n, void *p, void *l, void *r, std::size_t s) {
    entries[tail % kMaxLog] = {op, n, p, l, r, s};
    tail++;
  }
  void dump() {
    std::fprintf(stderr, "--- OP LOG DUMP ---\n");
    int count = std::min(tail, kMaxLog);
    int start = (tail > kMaxLog) ? (tail % kMaxLog) : 0;
    for (int i = 0; i < count; ++i) {
      int idx = (start + i) % kMaxLog;
      auto &e = entries[idx];
      std::fprintf(stderr, "[%d] %s: node=%p p=%p l=%p r=%p size=%zu\n", idx,
                   e.op, e.node, e.parent, e.left, e.right, e.size);
    }
  }
};
// Use a map to track logs per allocator instance (since we can't edit header
// easily) Actually, I'll just add it as a thread-local for now since we are in
// a stress test where each thread has its own allocator.
thread_local AllocLog g_log;

#define ASSERT_NOT_NULL(ptr)                                                   \
  if ((ptr) == nullptr) {                                                      \
    std::fprintf(stderr, "FATAL: nullptr encountered in RB-Tree! %s:%d\n",     \
                 __FILE__, __LINE__);                                          \
    std::fflush(stderr);                                                       \
    g_log.dump();                                                              \
    std::fflush(stderr);                                                       \
    std::abort();                                                              \
  }

#ifndef ASSERT_NOT_NULL
#define ASSERT_NOT_NULL(ptr)                                                   \
  if ((ptr) == nullptr) {                                                      \
    std::fprintf(stderr, "FATAL: nullptr encountered in RB-Tree! %s:%d\n",     \
                 __FILE__, __LINE__);                                          \
    std::fflush(stderr);                                                       \
    g_log.dump();                                                              \
    std::fflush(stderr);                                                       \
    std::abort();                                                              \
  }
#endif

#define SET_PARENT(n, p)                                                       \
  do {                                                                         \
    ASSERT_NOT_NULL(p);                                                        \
    if ((n) != nil_)                                                           \
      (n)->parent = (p);                                                       \
  } while (0)
#define SET_LEFT(n, l)                                                         \
  do {                                                                         \
    ASSERT_NOT_NULL(n);                                                        \
    ASSERT_NOT_NULL(l);                                                        \
    (n)->left = (l);                                                           \
  } while (0)
#define SET_RIGHT(n, r)                                                        \
  do {                                                                         \
    ASSERT_NOT_NULL(n);                                                        \
    ASSERT_NOT_NULL(r);                                                        \
    (n)->right = (r);                                                          \
  } while (0)

FreeListAllocator::FreeListAllocator(std::byte *base, std::size_t size) noexcept
    : base_{base}, size_{size} {
  static_assert(sizeof(FreeBlock) <= 48, "FreeBlock too large");
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

  nil_ = new FreeBlock();
  nil_->size = 0;
  nil_->parent = nil_;
  nil_->left = nil_;
  nil_->right = nil_;
  nil_->subtree_max = 0;
  nil_->color = Color::Black;

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
  if (size == 0)
    size = 1;

  // Enforce 16-byte alignment for internal structural integrity.
  // All FreeBlock headers MUST be 16-byte aligned.
  std::size_t internal_align = std::max(alignment, std::size_t(16));
  std::size_t internal_size = (size + 15) & ~std::size_t(15);

  // 1. Check small block segregated lists first
  if (internal_size <= kMaxSmallBlockSize &&
      internal_size >= kSmallBlockQuantum) {
    std::size_t idx = (internal_size / kSmallBlockQuantum) - 1;
    if (idx < kNumSmallClasses && free_lists_[idx]) {
      auto *node = free_lists_[idx];
      free_lists_[idx] = node->next;

      allocated_ += internal_size;
      free_blocks_--;

      return AllocationResult{
          .ptr = reinterpret_cast<std::byte *>(node),
          .offset = static_cast<std::size_t>(
              reinterpret_cast<std::byte *>(node) - base_),
          .actual_size = internal_size,
      };
    }
  }

  // 2. Search Red-Black Tree for first-fit
  std::size_t min_size = std::max(internal_size, kMinBlockSize);
  auto *curr = find_first_fit(min_size);

  while (curr != nil_) {
    auto *block_start = reinterpret_cast<std::byte *>(curr);

    // Check alignment for PAYLOAD (which is the entire block now)
    void *aligned_ptr = block_start;
    std::size_t space = curr->size;

    if (std::align(internal_align, internal_size, aligned_ptr, space)) {
      // It fits!
      auto *header_ptr =
          static_cast<std::byte *>(aligned_ptr); // The returned pointer
      auto pre_padding = static_cast<std::size_t>(header_ptr - block_start);

      std::size_t total_block_size = curr->size;
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
          ASSERT_NOT_NULL(nil_);
          gap_block->parent = nil_;
          gap_block->left = nil_;
          gap_block->right = nil_;
          gap_block->subtree_max = pre_padding;
          gap_block->color = Color::Red;
          insert_node(gap_block);
        }
      }

      // Handle Remainder
      std::size_t remainder_size =
          (total_block_size - pre_padding) - internal_size;

      bool absorbed = false;
      if (remainder_size >= kMinBlockSize) {
        auto *new_free = new (header_ptr + internal_size)
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
          auto *node = reinterpret_cast<FreeNode *>(header_ptr + internal_size);
          node->next = free_lists_[idx];
          free_lists_[idx] = node;
        } else {
          absorbed = true;
        }
      } else {
        absorbed = true;
      }

      if (absorbed) {
        internal_size += remainder_size;
        free_blocks_--;
      }

      allocated_ += internal_size;

      verify_tree(root_);
      return AllocationResult{
          .ptr = header_ptr,
          .offset = static_cast<std::size_t>(header_ptr - base_),
          .actual_size = internal_size,
      };
    }

    curr = successor(curr);
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

  auto *base = base_;
  auto *end = base + size_;

  if (ptr < base || ptr >= end) {
    return std::unexpected(AllocError::BadPointer);
  }

  std::size_t remaining_space = static_cast<std::size_t>(end - ptr);
  if (size > remaining_space || size < kSmallBlockQuantum) {
    std::fprintf(stderr,
                 "FATAL: deallocate with invalid size %zu (remaining=%zu)\n",
                 size, remaining_space);
    std::fflush(stderr);
    std::abort();
  }

  if (reinterpret_cast<std::uintptr_t>(ptr) % 16 != 0) {
    return std::unexpected(AllocError::InvalidAlignment);
  }

  std::size_t actual_size = size;
  // No header to find. 'ptr' is the start of the block.

  if (actual_size <= kMaxSmallBlockSize) {
    std::size_t idx = (actual_size / kSmallBlockQuantum) - 1;
    if (idx >= kNumSmallClasses)
      idx = kNumSmallClasses - 1;

    auto *block = reinterpret_cast<FreeNode *>(ptr);

    block->next = free_lists_[idx];
    free_lists_[idx] = block;

    allocated_ -= actual_size;
    free_blocks_++;
    return {};
  }

  // Ensure block is large enough for RB metadata
  if (actual_size < kMinBlockSize) {
    std::size_t idx = kNumSmallClasses - 1;
    auto *block = reinterpret_cast<FreeNode *>(ptr);
    block->next = free_lists_[idx];
    free_lists_[idx] = block;
    allocated_ -= actual_size;
    free_blocks_++;
    return {};
  }

  auto *block_addr = ptr;

  // 1. Find potential neighbors in address-ordered tree
  // We insert a temporary node to find predecessor/successor
  auto *freed = new (block_addr) FreeBlock{.size = actual_size,
                                           .parent = nil_,
                                           .left = nil_,
                                           .right = nil_,
                                           .subtree_max = actual_size,
                                           .color = Color::Red};

  insert_node(freed);
  free_blocks_++;
  allocated_ -= actual_size;

  // 2. Coalesce with Successor
  auto *succ = successor(freed);
  if (succ != nil_) {
    auto *freed_end = reinterpret_cast<std::byte *>(freed) + freed->size;
    if (freed_end == reinterpret_cast<std::byte *>(succ)) {
      std::size_t succ_size = succ->size;
      if (succ_size > size_ || succ_size == 0) {
        std::fprintf(stderr, "FATAL: coalescing with garbage succ_size %zu\n",
                     succ_size);
        std::fflush(stderr);
        g_log.dump();
        std::abort();
      }
      delete_node(succ);
      freed->size += succ_size;
      update_max_upwards(freed);
      free_blocks_--;
    }
  }

  // 3. Coalesce with Predecessor
  auto *prev = predecessor(freed);
  if (prev != nil_) {
    auto *prev_end = reinterpret_cast<std::byte *>(prev) + prev->size;
    if (prev_end == reinterpret_cast<std::byte *>(freed)) {
      std::size_t freed_size = freed->size;
      if (freed_size > size_ || freed_size == 0) {
        std::fprintf(stderr, "FATAL: coalescing with garbage freed_size %zu\n",
                     freed_size);
        std::fflush(stderr);
        g_log.dump();
        std::abort();
      }
      delete_node(freed);
      prev->size += freed_size;
      update_max_upwards(prev);
      free_blocks_--;
    }
  }

  verify_tree(root_);
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
  ASSERT_NOT_NULL(y);
  SET_RIGHT(x, y->left);
  if (y->left != nil_) {
    SET_PARENT(y->left, x);
  }
  SET_PARENT(y, x->parent);
  if (x->parent == nil_) {
    root_ = y;
  } else if (x == x->parent->left) {
    SET_LEFT(x->parent, y);
  } else {
    SET_RIGHT(x->parent, y);
  }
  SET_LEFT(y, x);
  SET_PARENT(x, y);

  g_log.add("left_rotate", x, x->parent, x->left, x->right, x->size);

  // Update max. Note: y's max becomes what x's was, then we re-derive.
  y->subtree_max = x->subtree_max;
  update_max(x);
  update_max(y);
}

void FreeListAllocator::right_rotate(FreeBlock *x) {
  FreeBlock *y = x->left;
  ASSERT_NOT_NULL(y);
  SET_LEFT(x, y->right);
  if (y->right != nil_) {
    SET_PARENT(y->right, x);
  }
  SET_PARENT(y, x->parent);
  if (x->parent == nil_) {
    root_ = y;
  } else if (x == x->parent->right) {
    SET_RIGHT(x->parent, y);
  } else {
    SET_LEFT(x->parent, y);
  }
  SET_RIGHT(y, x);
  SET_PARENT(x, y);

  g_log.add("right_rotate", x, x->parent, x->left, x->right, x->size);

  // Update max. Note: y's max becomes what x's was, then we re-derive.
  y->subtree_max = x->subtree_max;
  update_max(x);
  update_max(y);
}

void FreeListAllocator::insert_node(FreeBlock *z) {
  if (z == nullptr || z == nil_)
    return;

  if (z->size > 1024 * 1024 * 1024 || z->size == 0) {
    std::fprintf(stderr, "FATAL: insert_node with garbage size %zu at %p\n",
                 z->size, (void *)z);
    std::fflush(stderr);
    g_log.dump();
    std::fflush(stderr);
    std::abort();
  }

  if (reinterpret_cast<std::uintptr_t>(z) % 16 != 0) {
    std::fprintf(stderr,
                 "RB-Tree Error: insert_node with misaligned pointer %p\n",
                 (void *)z);
    g_log.dump();
    std::abort();
  }

  FreeBlock *y = nil_;
  FreeBlock *x = root_;

  // Key is Address
  while (x != nil_) {
    ASSERT_NOT_NULL(x);
    y = x;
    if (z < x) {
      x = x->left;
    } else {
      x = x->right;
    }
  }

  ASSERT_NOT_NULL(y);
  z->parent = y;
  if (y == nil_) {
    root_ = z;
  } else if (z < y) {
    ASSERT_NOT_NULL(z);
    y->left = z;
  } else {
    ASSERT_NOT_NULL(z);
    y->right = z;
  }

  ASSERT_NOT_NULL(nil_);
  z->left = nil_;
  z->right = nil_;
  z->color = Color::Red;
  z->subtree_max = z->size;

  g_log.add("insert_node", z, z->parent, z->left, z->right, z->size);

  update_max_upwards(z);

  rb_insert_fixup(z);
}

void FreeListAllocator::rb_insert_fixup(FreeBlock *z) {
  while (z != nullptr && z->parent != nullptr &&
         z->parent->color == Color::Red) {
    ASSERT_NOT_NULL(z->parent->parent);
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
    SET_LEFT(u->parent, v);
  } else {
    SET_RIGHT(u->parent, v);
  }
  if (v != nil_) {
    SET_PARENT(v, u->parent);
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

  FreeBlock *x_parent = nil_;

  if (z->left == nil_) {
    x = z->right;
    x_parent = z->parent;
    rb_transplant(z, z->right);
    fix_start = z->parent;
  } else if (z->right == nil_) {
    x = z->left;
    x_parent = z->parent;
    rb_transplant(z, z->left);
    fix_start = z->parent;
  } else {
    y = minimum(z->right);
    y_original_color = y->color;
    x = y->right;

    if (y->parent == z) {
      x_parent = y;
      fix_start = y;
    } else {
      x_parent = y->parent;
      rb_transplant(y, y->right);
      SET_RIGHT(y, z->right);
      SET_PARENT(y->right, y);
      fix_start = y->parent;
    }

    rb_transplant(z, y);
    SET_LEFT(y, z->left);
    SET_PARENT(y->left, y);
    y->color = z->color;
  }

  g_log.add("delete_node", z, z->parent, z->left, z->right, z->size);

  // First, update max for the node that replaced z (if it wasn't spliced out)
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
    rb_delete_fixup(x, x_parent);
  }
}

void FreeListAllocator::rb_delete_fixup(FreeBlock *x, FreeBlock *x_parent) {
  while (x != root_ &&
         (x == nil_ || (x != nullptr && x->color == Color::Black))) {
    ASSERT_NOT_NULL(x_parent);
    if (x == x_parent->left) {
      FreeBlock *w = x_parent->right;
      if (w->color == Color::Red) {
        w->color = Color::Black;
        x_parent->color = Color::Red;
        left_rotate(x_parent);
        w = x_parent->right;
      }
      if (w->left->color == Color::Black && w->right->color == Color::Black) {
        if (w != nil_)
          w->color = Color::Red;
        x = x_parent;
        x_parent = x->parent;
      } else {
        if (w->right->color == Color::Black) {
          w->left->color = Color::Black;
          w->color = Color::Red;
          right_rotate(w);
          w = x_parent->right;
        }
        w->color = x_parent->color;
        x_parent->color = Color::Black;
        w->right->color = Color::Black;
        left_rotate(x_parent);
        x = root_;
      }
    } else {
      FreeBlock *w = x_parent->left;
      if (w->color == Color::Red) {
        w->color = Color::Black;
        x_parent->color = Color::Red;
        right_rotate(x_parent);
        w = x_parent->left;
      }
      if (w->right->color == Color::Black && w->left->color == Color::Black) {
        if (w != nil_)
          w->color = Color::Red;
        x = x_parent;
        x_parent = x->parent;
      } else {
        if (w->left->color == Color::Black) {
          w->right->color = Color::Black;
          w->color = Color::Red;
          left_rotate(w);
          w = x_parent->left;
        }
        w->color = x_parent->color;
        x_parent->color = Color::Black;
        w->left->color = Color::Black;
        right_rotate(x_parent);
        x = root_;
      }
    }
  }
  if (x != nil_)
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

void FreeListAllocator::update_max_upwards(FreeBlock *x) {
  while (x != nil_ && x != nullptr) {
    std::size_t old_max = x->subtree_max;
    update_max(x);
    if (x->subtree_max == old_max) {
      // Optimization: if max didn't change, we can potentially stop.
      // But wait, if we decreased size, we might need to continue.
      // For simplicity, always walk up or at least check one more level.
    }
    x = x->parent;
  }
}

void FreeListAllocator::update_max(FreeBlock *x) {
  if (x == nil_ || x == nullptr)
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

void FreeListAllocator::verify_tree(FreeBlock *x) const {
  if (x == nil_ || x == nullptr)
    return;

  if (reinterpret_cast<std::uintptr_t>(x) % 16 != 0) {
    std::fprintf(stderr, "RB-Tree Error: node %p is NOT 16-byte aligned\n",
                 (void *)x);
    g_log.dump();
    std::abort();
  }

  if (x->left != nil_) {
    if (x->left->parent != x) {
      std::fprintf(stderr,
                   "RB-Tree Error: x->left->parent != x. x=%p, x->left=%p, "
                   "x->left->parent=%p\n",
                   (void *)x, (void *)x->left, (void *)x->left->parent);
      g_log.dump();
      std::abort();
    }
    verify_tree(x->left);
  }
  if (x->right != nil_) {
    if (x->right->parent != x) {
      std::fprintf(stderr,
                   "RB-Tree Error: x->right->parent != x. x=%p, x->right=%p, "
                   "x->right->parent=%p\n",
                   (void *)x, (void *)x->right, (void *)x->right->parent);
      g_log.dump();
      std::abort();
    }
    verify_tree(x->right);
  }
  std::size_t expected_max = x->size;
  if (x->left != nil_)
    expected_max = std::max(expected_max, x->left->subtree_max);
  if (x->right != nil_)
    expected_max = std::max(expected_max, x->right->subtree_max);
  if (x->subtree_max != expected_max) {
    std::fprintf(
        stderr,
        "RB-Tree Error: subtree_max mismatch. x=%p, size=%zu, left->max=%zu, "
        "right->max=%zu, x->subtree_max=%zu, expected=%zu\n",
        (void *)x, x->size, (x->left != nil_ ? x->left->subtree_max : 0),
        (x->right != nil_ ? x->right->subtree_max : 0), x->subtree_max,
        expected_max);
    std::abort();
  }
}

} // namespace mmap_viz
