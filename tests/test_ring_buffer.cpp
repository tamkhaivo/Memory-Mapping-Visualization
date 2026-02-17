/// @file test_ring_buffer.cpp
/// @brief Unit tests for the Lock-Free Ring Buffer.

#include "tracker/tracker.hpp"
#include <gtest/gtest.h>
#include <thread>
#include <vector>

using namespace mmap_viz;

class RingBufferTest : public ::testing::Test {
protected:
  // Use a small buffer for testing wrap-around
  RingBuffer<int, 4> buffer_;
};

TEST_F(RingBufferTest, PushPopSingle) {
  buffer_.push(42);
  int val = 0;
  EXPECT_TRUE(buffer_.pop(val));
  EXPECT_EQ(val, 42);
}

TEST_F(RingBufferTest, EmptyPopReturnsFalse) {
  int val = 0;
  EXPECT_FALSE(buffer_.pop(val));
}

TEST_F(RingBufferTest, FIFOOrder) {
  buffer_.push(1);
  buffer_.push(2);
  buffer_.push(3);

  int val;
  EXPECT_TRUE(buffer_.pop(val));
  EXPECT_EQ(val, 1);
  EXPECT_TRUE(buffer_.pop(val));
  EXPECT_EQ(val, 2);
  EXPECT_TRUE(buffer_.pop(val));
  EXPECT_EQ(val, 3);
}

TEST_F(RingBufferTest, OverflowBehavior) {
  // Capacity is 4. effective capacity is N-1 (3).

  buffer_.push(1); // 1
  buffer_.push(2); // 2
  buffer_.push(3); // 3

  // 4th push should fail/drop silently
  buffer_.push(4);

  int val;
  EXPECT_TRUE(buffer_.pop(val));
  EXPECT_EQ(val, 1);
  EXPECT_TRUE(buffer_.pop(val));
  EXPECT_EQ(val, 2);
  EXPECT_TRUE(buffer_.pop(val));
  EXPECT_EQ(val, 3);
  EXPECT_FALSE(buffer_.pop(val)); // 4 should not be here
}

TEST_F(RingBufferTest, WrapAround) {
  // Fill 3
  buffer_.push(1);
  buffer_.push(2);
  buffer_.push(3);

  // Pop 1 (freeing a slot)
  int val;
  EXPECT_TRUE(buffer_.pop(val));
  EXPECT_EQ(val, 1);

  // Push 4 (should succeed now as slot 0 is free)
  buffer_.push(4);

  EXPECT_TRUE(buffer_.pop(val)); // 2
  EXPECT_EQ(val, 2);
  EXPECT_TRUE(buffer_.pop(val)); // 3
  EXPECT_EQ(val, 3);
  EXPECT_TRUE(buffer_.pop(val)); // 4
  EXPECT_EQ(val, 4);
}
