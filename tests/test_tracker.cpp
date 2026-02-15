/// @file test_tracker.cpp
/// @brief Unit tests for AllocationTracker.

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
    alloc_ = std::make_unique<FreeListAllocator>(*arena_);
    tracker_ = std::make_unique<AllocationTracker>(*alloc_, 1);
  }

  static constexpr std::size_t kArenaSize = 64 * 1024;
  std::unique_ptr<Arena> arena_;
  std::unique_ptr<FreeListAllocator> alloc_;
  std::unique_ptr<AllocationTracker> tracker_;
};

TEST_F(TrackerTest, RecordAlloc) {
  BlockMetadata meta{
      .offset = 0,
      .size = 128,
      .alignment = 16,
      .actual_size = 128,
      .tag = "test_block",
      .timestamp = std::chrono::steady_clock::now(),
  };

  auto event = tracker_->record_alloc(meta);
  EXPECT_EQ(event.type, EventType::Allocate);
  EXPECT_EQ(event.event_id, 0u);
  EXPECT_EQ(tracker_->active_block_count(), 1u);
}

TEST_F(TrackerTest, RecordDealloc) {
  BlockMetadata meta{
      .offset = 0,
      .size = 128,
      .alignment = 16,
      .actual_size = 128,
      .tag = "test",
      .timestamp = std::chrono::steady_clock::now(),
  };

  tracker_->record_alloc(meta);
  EXPECT_EQ(tracker_->active_block_count(), 1u);

  auto event = tracker_->record_dealloc(0);
  EXPECT_EQ(event.type, EventType::Deallocate);
  EXPECT_EQ(tracker_->active_block_count(), 0u);
}

TEST_F(TrackerTest, EventLog) {
  for (int i = 0; i < 5; ++i) {
    BlockMetadata meta{
        .offset = static_cast<std::size_t>(i * 128),
        .size = 128,
        .alignment = 16,
        .actual_size = 128,
        .tag = "block_" + std::to_string(i),
        .timestamp = std::chrono::steady_clock::now(),
    };
    tracker_->record_alloc(meta);
  }

  EXPECT_EQ(tracker_->event_log().size(), 5u);
  EXPECT_EQ(tracker_->active_block_count(), 5u);
}

TEST_F(TrackerTest, Snapshot) {
  for (int i = 0; i < 3; ++i) {
    BlockMetadata meta{
        .offset = static_cast<std::size_t>(i * 256),
        .size = 256,
        .alignment = 16,
        .actual_size = 256,
        .tag = "snap_" + std::to_string(i),
        .timestamp = std::chrono::steady_clock::now(),
    };
    tracker_->record_alloc(meta);
  }

  auto snap = tracker_->snapshot();
  EXPECT_EQ(snap.size(), 3u);

  // Snapshot should be ordered by offset (map iteration order).
  for (std::size_t i = 1; i < snap.size(); ++i) {
    EXPECT_GT(snap[i].offset, snap[i - 1].offset);
  }
}

TEST_F(TrackerTest, EventCallback) {
  int callback_count = 0;
  tracker_->set_callback([&](const AllocationEvent &) { ++callback_count; });

  BlockMetadata meta{
      .offset = 0,
      .size = 64,
      .alignment = 16,
      .actual_size = 64,
      .tag = "cb_test",
      .timestamp = std::chrono::steady_clock::now(),
  };

  tracker_->record_alloc(meta);
  tracker_->record_dealloc(0);

  EXPECT_EQ(callback_count, 2);
}

TEST_F(TrackerTest, MonotonicEventIds) {
  for (int i = 0; i < 5; ++i) {
    BlockMetadata meta{
        .offset = static_cast<std::size_t>(i * 128),
        .size = 128,
        .alignment = 16,
        .actual_size = 128,
        .tag = "id_test",
        .timestamp = std::chrono::steady_clock::now(),
    };
    tracker_->record_alloc(meta);
  }

  const auto &log = tracker_->event_log();
  for (std::size_t i = 1; i < log.size(); ++i) {
    EXPECT_GT(log[i].event_id, log[i - 1].event_id);
  }
}
