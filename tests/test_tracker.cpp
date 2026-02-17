/// @file test_tracker.cpp
/// @brief Unit tests for LocalTracker.

#include "allocator/arena.hpp"
#include "allocator/free_list.hpp"
#include "tracker/tracker.hpp"

#include <gtest/gtest.h>

using namespace mmap_viz;

class TrackerTest : public ::testing::Test {
protected:
  void SetUp() override {
    auto result = Arena::create(kArenaSize);
    ASSERT_TRUE(result.has_value());
    arena_ = std::make_unique<Arena>(std::move(*result));
    alloc_ =
        std::make_unique<FreeListAllocator>(arena_->base(), arena_->capacity());
    tracker_ = std::make_unique<LocalTracker>(*alloc_, 1);
  }

  static constexpr std::size_t kArenaSize = 64 * 1024;
  std::unique_ptr<Arena> arena_;
  std::unique_ptr<FreeListAllocator> alloc_;
  std::unique_ptr<LocalTracker> tracker_;
};

TEST_F(TrackerTest, RecordAlloc) {
  BlockMetadata meta{
      .offset = 0,
      .size = 128,
      .alignment = 16,
      .actual_size = 128,
      .timestamp = std::chrono::steady_clock::now(),
  };
  meta.set_tag("test_block");

  tracker_->record_alloc(std::move(meta));

  std::vector<AllocationEvent> events;
  tracker_->drain_to(events);

  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].type, EventType::Allocate);
  EXPECT_EQ(events[0].event_id, 1u);
  // Verify tag
  EXPECT_STREQ(events[0].block.tag, "test_block");
}

TEST_F(TrackerTest, RecordDealloc) {
  // Pre-condition: Alloc something so metrics are interesting (optional)

  tracker_->record_dealloc(128, 64);

  std::vector<AllocationEvent> events;
  tracker_->drain_to(events);

  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].type, EventType::Deallocate);
  EXPECT_EQ(events[0].block.offset, 128u);
  EXPECT_EQ(events[0].block.actual_size, 64u);
}

TEST_F(TrackerTest, Sampling) {
  // Create tracker with sampling = 2
  auto sampled_tracker = std::make_unique<LocalTracker>(*alloc_, 2);

  BlockMetadata meta{};

  // Event 1 (ID 1) -> Passed?
  // if (++next_event_id_ % sampling_ != 0) return;
  // 1 % 2 != 0 -> dropped.
  sampled_tracker->record_alloc(meta);

  // Event 2 (ID 2) -> 2 % 2 == 0 -> recorded.
  sampled_tracker->record_dealloc(0, 0);

  std::vector<AllocationEvent> events;
  sampled_tracker->drain_to(events);

  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].event_id, 2u);
  EXPECT_EQ(events[0].type, EventType::Deallocate);
}

TEST_F(TrackerTest, EventMonotonicity) {
  for (int i = 0; i < 5; ++i) {
    tracker_->record_dealloc(0, 0);
  }

  std::vector<AllocationEvent> events;
  tracker_->drain_to(events);

  ASSERT_EQ(events.size(), 5u);
  for (size_t i = 0; i < events.size(); ++i) {
    EXPECT_EQ(events[i].event_id, i + 1);
  }
}
