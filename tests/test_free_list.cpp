/// @file test_free_list.cpp
/// @brief Unit tests for the FreeListAllocator.

#include "allocator/arena.hpp"
#include "allocator/free_list.hpp"

#include <gtest/gtest.h>
#include <vector>

using namespace mmap_viz;

class FreeListTest : public ::testing::Test {
protected:
  void SetUp() override {
    auto result = Arena::create(kArenaSize);
    ASSERT_TRUE(result.has_value());
    arena_ = std::make_unique<Arena>(std::move(*result));
    alloc_ = std::make_unique<FreeListAllocator>(*arena_);
  }

  static constexpr std::size_t kArenaSize = 64 * 1024; // 64 KB
  std::unique_ptr<Arena> arena_;
  std::unique_ptr<FreeListAllocator> alloc_;
};

TEST_F(FreeListTest, SingleAllocation) {
  auto result = alloc_->allocate(128);
  ASSERT_TRUE(result.has_value());
  EXPECT_NE(result->ptr, nullptr);
  EXPECT_EQ(result->actual_size, 128u);
  EXPECT_GT(alloc_->bytes_allocated(), 0u);
}

TEST_F(FreeListTest, MultipleAllocations) {
  std::vector<AllocationResult> results;

  for (int i = 0; i < 10; ++i) {
    auto r = alloc_->allocate(64);
    ASSERT_TRUE(r.has_value()) << "Allocation " << i << " failed";
    results.push_back(*r);
  }

  // All pointers should be distinct.
  for (std::size_t i = 0; i < results.size(); ++i) {
    for (std::size_t j = i + 1; j < results.size(); ++j) {
      EXPECT_NE(results[i].ptr, results[j].ptr);
    }
  }
}

TEST_F(FreeListTest, AllocateAndDeallocate) {
  auto r1 = alloc_->allocate(256);
  ASSERT_TRUE(r1.has_value());

  auto before_free = alloc_->bytes_allocated();
  auto dealloc_result = alloc_->deallocate(r1->ptr, 256);
  ASSERT_TRUE(dealloc_result.has_value());

  EXPECT_LT(alloc_->bytes_allocated(), before_free);
}

TEST_F(FreeListTest, Coalescing) {
  // Allocate two adjacent blocks.
  auto r1 = alloc_->allocate(128);
  auto r2 = alloc_->allocate(128);
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());

  auto blocks_before = alloc_->free_block_count();

  // Free both — they should coalesce.
  alloc_->deallocate(r1->ptr, 128);
  alloc_->deallocate(r2->ptr, 128);

  // After coalescing, free block count should not increase.
  EXPECT_LE(alloc_->free_block_count(), blocks_before + 1);
}

TEST_F(FreeListTest, OutOfMemory) {
  // Try to allocate more than the arena.
  auto r = alloc_->allocate(kArenaSize + 1);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), AllocError::OutOfMemory);
}

TEST_F(FreeListTest, InvalidAlignment) {
  auto r = alloc_->allocate(64, 3); // 3 is not a power of 2.
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), AllocError::InvalidAlignment);
}

TEST_F(FreeListTest, AlignedAllocation) {
  auto r = alloc_->allocate(64, 64); // 64-byte alignment
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(r->ptr) % 64, 0u);
}

TEST_F(FreeListTest, BytesFreeConsistency) {
  auto initial_free = alloc_->bytes_free();
  EXPECT_EQ(initial_free, alloc_->capacity());

  auto r = alloc_->allocate(1024);
  ASSERT_TRUE(r.has_value());

  EXPECT_EQ(alloc_->bytes_allocated() + alloc_->bytes_free(),
            alloc_->capacity());
}

TEST_F(FreeListTest, FragmentationPattern) {
  // Allocate many small blocks.
  std::vector<AllocationResult> results;
  for (int i = 0; i < 20; ++i) {
    auto r = alloc_->allocate(64);
    if (r.has_value())
      results.push_back(*r);
  }

  // Free every other block.
  for (std::size_t i = 0; i < results.size(); i += 2) {
    alloc_->deallocate(results[i].ptr, 64);
  }

  // Should have multiple free blocks (fragmentation).
  EXPECT_GT(alloc_->free_block_count(), 1u);
}

TEST_F(FreeListTest, DeallocateNullptr) {
  auto result = alloc_->deallocate(nullptr, 0);
  EXPECT_TRUE(result.has_value()); // Should be a no-op.
}

TEST_F(FreeListTest, BadPointer) {
  std::byte fake[64];
  auto result = alloc_->deallocate(fake, 64);
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), AllocError::BadPointer);
}
