/// @file test_arena.cpp
/// @brief Unit tests for the mmap-backed Arena.

#include "allocator/arena.hpp"

#include <gtest/gtest.h>

using namespace mmap_viz;

TEST(ArenaTest, CreateWithValidCapacity) {
  auto result = Arena::create(4096);
  ASSERT_TRUE(result.has_value());
  EXPECT_NE(result->base(), nullptr);
  EXPECT_GE(result->capacity(), 4096u);
}

TEST(ArenaTest, CapacityIsPageAligned) {
  auto ps = Arena::page_size();
  // Request a non-page-aligned size.
  auto result = Arena::create(ps + 1);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->capacity() % ps, 0u);
  EXPECT_GE(result->capacity(), ps + 1);
}

TEST(ArenaTest, CreateWithZeroCapacityFails) {
  auto result = Arena::create(0);
  ASSERT_FALSE(result.has_value());
}

TEST(ArenaTest, MoveConstruction) {
  auto result = Arena::create(4096);
  ASSERT_TRUE(result.has_value());

  auto *original_base = result->base();
  auto original_cap = result->capacity();

  Arena moved{std::move(*result)};
  EXPECT_EQ(moved.base(), original_base);
  EXPECT_EQ(moved.capacity(), original_cap);

  // Original should be nullified.
  EXPECT_EQ(result->base(), nullptr);
  EXPECT_EQ(result->capacity(), 0u);
}

TEST(ArenaTest, MoveAssignment) {
  auto a = Arena::create(4096);
  auto b = Arena::create(8192);
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());

  auto *b_base = b->base();
  auto b_cap = b->capacity();

  *a = std::move(*b);
  EXPECT_EQ(a->base(), b_base);
  EXPECT_EQ(a->capacity(), b_cap);
}

TEST(ArenaTest, MemoryIsReadableWritable) {
  auto result = Arena::create(4096);
  ASSERT_TRUE(result.has_value());

  // Write pattern.
  auto *base = result->base();
  for (std::size_t i = 0; i < result->capacity(); ++i) {
    base[i] = static_cast<std::byte>(i & 0xFF);
  }

  // Verify.
  for (std::size_t i = 0; i < result->capacity(); ++i) {
    EXPECT_EQ(base[i], static_cast<std::byte>(i & 0xFF));
  }
}

TEST(ArenaTest, LargeArena) {
  auto result = Arena::create(64 * 1024 * 1024); // 64 MB
  ASSERT_TRUE(result.has_value());
  EXPECT_GE(result->capacity(), 64u * 1024 * 1024);
}
