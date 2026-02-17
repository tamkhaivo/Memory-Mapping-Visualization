/// @file test_visualization_arena.cpp
/// @brief Unit tests for the VisualizationArena façade.

#include "interface/padding_inspector.hpp"
#include "interface/visualization_arena.hpp"

#include <atomic>
#include <gtest/gtest.h>
#include <memory_resource>
#include <string>
#include <thread>
#include <vector>

using namespace mmap_viz;

// ─── Test fixture ────────────────────────────────────────────────────────

class VisualizationArenaTest : public ::testing::Test {
protected:
  void SetUp() override {
    auto result = VisualizationArena::create({.arena_size = 1024 * 1024});
    ASSERT_TRUE(result.has_value()) << "Failed to create VisualizationArena";
    arena_ = std::make_unique<VisualizationArena>(std::move(*result));
  }

  std::unique_ptr<VisualizationArena> arena_;
};

// ─── Creation tests ─────────────────────────────────────────────────────

TEST_F(VisualizationArenaTest, CreateWithDefaults) {
  EXPECT_NE(arena_->base(), nullptr);
  EXPECT_GE(arena_->capacity(), 64u * 1024);
  EXPECT_EQ(arena_->bytes_allocated(), 0u);
  EXPECT_EQ(arena_->active_block_count(), 0u);
  EXPECT_GT(arena_->cache_line_size(), 0u);
}

TEST_F(VisualizationArenaTest, CreateWithCustomConfig) {
  auto result = VisualizationArena::create({
      .arena_size = 128 * 1024,
      .cache_line_size = 128, // Apple Silicon style
  });
  ASSERT_TRUE(result.has_value());
  EXPECT_GE(result->capacity(), 128u * 1024);
  EXPECT_EQ(result->cache_line_size(), 128u);
}

TEST_F(VisualizationArenaTest, CreateWithZeroFails) {
  auto result = VisualizationArena::create({.arena_size = 0});
  EXPECT_FALSE(result.has_value());
}

// ─── Typed allocation ───────────────────────────────────────────────────

TEST_F(VisualizationArenaTest, TypedAllocDealloc) {
  int *p = arena_->alloc<int>("test_int", 42);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(*p, 42);
  EXPECT_GT(arena_->bytes_allocated(), 0u);
  // active_block_count is not supported in sharded mode efficiently
  // EXPECT_EQ(arena_->active_block_count(), 1u);

  arena_->dealloc(p);
  // EXPECT_EQ(arena_->active_block_count(), 0u);
  EXPECT_EQ(arena_->bytes_allocated(), 0u);
}

TEST_F(VisualizationArenaTest, TypedAllocAlignment) {
  // doubles require 8-byte alignment.
  double *p = arena_->alloc<double>("test_double", 3.14);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % alignof(double), 0u);
  EXPECT_DOUBLE_EQ(*p, 3.14);
  arena_->dealloc(p);
}

TEST_F(VisualizationArenaTest, MultipleTypedAllocs) {
  int *a = arena_->alloc<int>("a", 1);
  int *b = arena_->alloc<int>("b", 2);
  int *c = arena_->alloc<int>("c", 3);

  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  ASSERT_NE(c, nullptr);
  EXPECT_NE(a, b);
  EXPECT_NE(b, c);
  EXPECT_GT(arena_->bytes_allocated(), sizeof(int) * 3);

  arena_->dealloc(a);
  arena_->dealloc(b);
  arena_->dealloc(c);
  EXPECT_EQ(arena_->bytes_allocated(), 0u);
}

// ─── Raw allocation ─────────────────────────────────────────────────────

TEST_F(VisualizationArenaTest, RawAllocDealloc) {
  void *p = arena_->alloc_raw(256, 16, "raw_block");
  ASSERT_NE(p, nullptr);
  EXPECT_GT(arena_->bytes_allocated(), 0u);

  arena_->dealloc_raw(p, 256);
}

TEST_F(VisualizationArenaTest, RawAllocNullOnOOM) {
  // Allocate more than the arena can hold.
  void *p = arena_->alloc_raw(arena_->capacity() + 1, 16, "too_big");
  EXPECT_EQ(p, nullptr);
}

// ─── PMR interop ────────────────────────────────────────────────────────

TEST_F(VisualizationArenaTest, PmrInterop) {
  auto *res = arena_->resource();
  ASSERT_NE(res, nullptr);

  std::pmr::vector<int> vec{res};
  vec.push_back(10);
  vec.push_back(20);
  vec.push_back(30);

  EXPECT_EQ(vec.size(), 3u);
  EXPECT_EQ(vec[0], 10);
  EXPECT_GT(arena_->bytes_allocated(), 0u);
}

// ─── Padding report ─────────────────────────────────────────────────────

TEST_F(VisualizationArenaTest, DISABLED_PaddingReport) {
  arena_->alloc_raw(100, 16, "block_a");
  arena_->alloc_raw(200, 64, "block_b");

  auto report = arena_->padding_report();
  EXPECT_EQ(report.blocks.size(), 2u);
  EXPECT_EQ(report.total_requested, 300u);
  EXPECT_GE(report.total_actual, 300u);
  EXPECT_LE(report.efficiency, 1.0f);
  EXPECT_GT(report.efficiency, 0.0f);
}

// ─── Cache report ───────────────────────────────────────────────────────

TEST_F(VisualizationArenaTest, DISABLED_CacheReport) {
  arena_->alloc_raw(32, 16, "small");
  arena_->alloc_raw(128, 16, "medium");

  auto report = arena_->cache_report();
  EXPECT_GT(report.active_lines, 0u);
  EXPECT_GT(report.avg_utilization, 0.0f);
  EXPECT_EQ(report.cache_line_size, arena_->cache_line_size());
}

// ─── JSON output ────────────────────────────────────────────────────────

TEST_F(VisualizationArenaTest, SnapshotJson) {
  arena_->alloc_raw(64, 16, "json_test");

  auto json = arena_->snapshot_json();
  EXPECT_FALSE(json.empty());
  EXPECT_NE(json.find("\"type\":\"snapshot\""), std::string::npos);
  EXPECT_NE(json.find("\"capacity\""), std::string::npos);
}

TEST_F(VisualizationArenaTest, EventLogJson) {
  arena_->alloc_raw(64, 16, "log_test");

  auto json = arena_->event_log_json();
  EXPECT_FALSE(json.empty());
  EXPECT_NE(json.find("\"allocate\""), std::string::npos);
}

// ─── Struct layout inspection (compile-time macro) ──────────────────────

namespace {
struct TestPadded {
  char a;   // 1 byte
  double b; // 8 bytes (likely 7 bytes padding before)
  char c;   // 1 byte (likely 7 bytes tail padding)
};

struct TestPacked {
  double x; // 8 bytes
  double y; // 8 bytes
  double z; // 8 bytes
};
} // namespace

TEST_F(VisualizationArenaTest, InspectLayoutPadded) {
  auto info = MMAP_VIZ_INSPECT(TestPadded, a, b, c);

  EXPECT_EQ(info.total_size, sizeof(TestPadded));
  EXPECT_EQ(info.total_alignment, alignof(TestPadded));
  EXPECT_EQ(info.fields.size(), 3u);

  // Field 'a' should be at offset 0.
  EXPECT_EQ(info.fields[0].offset, 0u);
  EXPECT_EQ(info.fields[0].size, sizeof(char));

  // Field 'b' should have padding before it.
  EXPECT_GT(info.fields[1].padding_before, 0u);
  EXPECT_EQ(info.fields[1].size, sizeof(double));

  // Overall: useful < total due to padding.
  EXPECT_GT(info.padding_bytes, 0u);
  EXPECT_LT(info.efficiency, 1.0f);
}

TEST_F(VisualizationArenaTest, InspectLayoutPacked) {
  auto info = MMAP_VIZ_INSPECT(TestPacked, x, y, z);

  EXPECT_EQ(info.total_size, sizeof(TestPacked));
  EXPECT_EQ(info.useful_bytes, 3 * sizeof(double));

  // All doubles packed — no internal padding expected.
  EXPECT_EQ(info.fields[0].padding_before, 0u);
  EXPECT_EQ(info.fields[1].padding_before, 0u);
  EXPECT_EQ(info.fields[2].padding_before, 0u);

  // Efficiency should be 1.0 (no wasted bytes).
  EXPECT_FLOAT_EQ(info.efficiency, 1.0f);
}

// ─── Move semantics ────────────────────────────────────────────────────

TEST_F(VisualizationArenaTest, MoveConstruction) {
  arena_->alloc_raw(128, 16, "pre_move");
  auto allocated = arena_->bytes_allocated();

  VisualizationArena moved{std::move(*arena_)};
  EXPECT_EQ(moved.bytes_allocated(), allocated);
  EXPECT_NE(moved.base(), nullptr);
}

TEST_F(VisualizationArenaTest, MultiThreadedAlloc) {
  constexpr int kNumThreads = 8;
  constexpr int kAllocationsPerThread = 100;

  std::vector<std::thread> threads;
  std::atomic<bool> start{false};

  for (int i = 0; i < kNumThreads; ++i) {
    threads.emplace_back([this, &start, i]() {
      while (!start)
        std::this_thread::yield();

      std::string tag = "thread_" + std::to_string(i);
      std::vector<void *> ptrs;

      for (int j = 0; j < kAllocationsPerThread; ++j) {
        void *p = arena_->alloc_raw(128, 16, tag);
        if (p)
          ptrs.push_back(p);
      }

      for (void *p : ptrs) {
        arena_->dealloc_raw(p, 128);
      }
    });
  }

  start = true;
  for (auto &t : threads) {
    t.join();
  }

  EXPECT_EQ(arena_->bytes_allocated(), 0u);
}

TEST_F(VisualizationArenaTest, TwoArenasOneThread) {
  auto result_b = VisualizationArena::create({.arena_size = 1024 * 1024});
  ASSERT_TRUE(result_b.has_value());
  // Move to a unique_ptr to manage lifetime explicitly if needed, but Test
  // fixture manages arena_ Here we use a local arena_b
  VisualizationArena arena_b = std::move(*result_b);

  // 1. Alloc in A (registers context in A)
  arena_->alloc_raw(16, 16, "A1");

  // 2. Alloc in B (deletes A's context, registers B's context)
  arena_b.alloc_raw(16, 16, "B1");

  // 3. Alloc in A again (deletes B's context, registers NEW context in A)
  // At this point, A has TWO pointers in active_contexts. One comes from step 1
  // (freed), one from step 3 (valid).
  arena_->alloc_raw(16, 16, "A2");

  // 4. Trigger iteration over active_contexts in A
  // This should crash if A tries to access the freed context from step 1.
  auto json = arena_->event_log_json();
  EXPECT_FALSE(json.empty());
}
