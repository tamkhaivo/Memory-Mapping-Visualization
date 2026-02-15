/// @file test_cache_analyzer.cpp
/// @brief Unit tests for the CacheAnalyzer.

#include "interface/cache_analyzer.hpp"
#include "tracker/block_metadata.hpp"

#include <chrono>
#include <gtest/gtest.h>
#include <vector>

using namespace mmap_viz;

// ─── Helpers ────────────────────────────────────────────────────────────

static auto make_block(std::size_t offset, std::size_t size,
                       std::string tag = "") -> BlockMetadata {
  BlockMetadata meta{
      .offset = offset,
      .size = size,
      .alignment = 16,
      .actual_size = size,
      // .tag set below
      .timestamp = std::chrono::steady_clock::now(),
  };
  meta.set_tag(tag);
  return meta;
}

// ─── Tests ──────────────────────────────────────────────────────────────

class CacheAnalyzerTest : public ::testing::Test {
protected:
  CacheAnalyzer analyzer_{64}; // 64-byte cache lines
  static constexpr std::size_t kArenaCapacity = 4096;
};

TEST_F(CacheAnalyzerTest, EmptyArena) {
  std::vector<BlockMetadata> blocks;
  auto report = analyzer_.analyze(blocks, kArenaCapacity);

  EXPECT_EQ(report.cache_line_size, 64u);
  EXPECT_EQ(report.total_lines, kArenaCapacity / 64);
  EXPECT_EQ(report.active_lines, 0u);
  EXPECT_EQ(report.fully_utilized, 0u);
  EXPECT_EQ(report.split_allocations, 0u);
  EXPECT_FLOAT_EQ(report.avg_utilization, 0.0f);
  EXPECT_TRUE(report.lines.empty());
}

TEST_F(CacheAnalyzerTest, SingleBlockSingleLine) {
  // 32B block at offset 0 fits entirely in line 0.
  std::vector<BlockMetadata> blocks = {make_block(0, 32, "small")};

  auto report = analyzer_.analyze(blocks, kArenaCapacity);

  EXPECT_EQ(report.active_lines, 1u);
  EXPECT_EQ(report.split_allocations, 0u);
  EXPECT_EQ(report.lines.size(), 1u);

  auto &line = report.lines[0];
  EXPECT_EQ(line.line_index, 0u);
  EXPECT_EQ(line.bytes_used, 32u);
  EXPECT_EQ(line.bytes_wasted, 32u);
  EXPECT_FLOAT_EQ(line.utilization, 0.5f);
  EXPECT_FALSE(line.is_split);
  EXPECT_EQ(line.tags.size(), 1u);
  EXPECT_EQ(line.tags[0], "small");
}

TEST_F(CacheAnalyzerTest, FullLineUtilization) {
  // 64B block at offset 0 fills line 0 completely.
  std::vector<BlockMetadata> blocks = {make_block(0, 64, "full")};

  auto report = analyzer_.analyze(blocks, kArenaCapacity);

  EXPECT_EQ(report.fully_utilized, 1u);
  EXPECT_EQ(report.lines.size(), 1u);
  EXPECT_FLOAT_EQ(report.lines[0].utilization, 1.0f);
  EXPECT_EQ(report.lines[0].bytes_wasted, 0u);
}

TEST_F(CacheAnalyzerTest, SplitAcrossLines) {
  // 96B block starting at offset 32 spans lines 0 and 1.
  // Line 0: bytes [32..63] = 32 used
  // Line 1: bytes [64..127] = 64 used  (only 96-32=64 remaining)
  std::vector<BlockMetadata> blocks = {make_block(32, 96, "split")};

  auto report = analyzer_.analyze(blocks, kArenaCapacity);

  EXPECT_EQ(report.split_allocations, 1u);
  EXPECT_EQ(report.active_lines, 2u);

  // Both lines should be marked as split.
  for (const auto &line : report.lines) {
    EXPECT_TRUE(line.is_split);
  }

  // Line 0: 32 bytes used (offset 32-63)
  auto it0 = std::find_if(report.lines.begin(), report.lines.end(),
                          [](const auto &l) { return l.line_index == 0; });
  ASSERT_NE(it0, report.lines.end());
  EXPECT_EQ(it0->bytes_used, 32u);

  // Line 1: 64 bytes used (offset 64-127)
  auto it1 = std::find_if(report.lines.begin(), report.lines.end(),
                          [](const auto &l) { return l.line_index == 1; });
  ASSERT_NE(it1, report.lines.end());
  EXPECT_EQ(it1->bytes_used, 64u);
}

TEST_F(CacheAnalyzerTest, MultipleBlocksOneLine) {
  // Two 16B blocks in line 0.
  std::vector<BlockMetadata> blocks = {
      make_block(0, 16, "block_a"),
      make_block(16, 16, "block_b"),
  };

  auto report = analyzer_.analyze(blocks, kArenaCapacity);

  EXPECT_EQ(report.active_lines, 1u);
  EXPECT_EQ(report.split_allocations, 0u);

  auto &line = report.lines[0];
  EXPECT_EQ(line.bytes_used, 32u);
  EXPECT_FLOAT_EQ(line.utilization, 0.5f);
  EXPECT_EQ(line.tags.size(), 2u);
}

TEST_F(CacheAnalyzerTest, UtilizationAverage) {
  // Line 0: 32B used (50%), Line 1: 64B used (100%).
  std::vector<BlockMetadata> blocks = {
      make_block(0, 32, "half"),
      make_block(64, 64, "full"),
  };

  auto report = analyzer_.analyze(blocks, kArenaCapacity);

  EXPECT_EQ(report.active_lines, 2u);
  EXPECT_EQ(report.fully_utilized, 1u);
  // Average: (0.5 + 1.0) / 2 = 0.75
  EXPECT_NEAR(report.avg_utilization, 0.75f, 0.01f);
}

TEST_F(CacheAnalyzerTest, LargeBlockSpansMultipleLines) {
  // 256B block at offset 0 spans 4 cache lines (0-3).
  std::vector<BlockMetadata> blocks = {make_block(0, 256, "large")};

  auto report = analyzer_.analyze(blocks, kArenaCapacity);

  EXPECT_EQ(report.active_lines, 4u);
  EXPECT_EQ(report.fully_utilized, 4u);
  EXPECT_EQ(report.split_allocations, 1u);
  EXPECT_FLOAT_EQ(report.avg_utilization, 1.0f);
}

TEST_F(CacheAnalyzerTest, DetectLineSize) {
  auto detected = CacheAnalyzer::detect_line_size();
  // Should be a power of 2 and at least 32.
  EXPECT_GT(detected, 0u);
  EXPECT_TRUE((detected & (detected - 1)) == 0) << "Not a power of 2";
}

TEST_F(CacheAnalyzerTest, LineSizeAccessor) {
  CacheAnalyzer a{128};
  EXPECT_EQ(a.line_size(), 128u);

  CacheAnalyzer b{64};
  EXPECT_EQ(b.line_size(), 64u);
}

TEST_F(CacheAnalyzerTest, InvalidLineSizeFallback) {
  // Non-power-of-2 should fall back to 64.
  CacheAnalyzer bad{100};
  EXPECT_EQ(bad.line_size(), 64u);

  CacheAnalyzer zero{0};
  EXPECT_EQ(zero.line_size(), 64u);
}

TEST_F(CacheAnalyzerTest, SortedOutputByLineIndex) {
  // Blocks in reverse order — output should still be sorted.
  std::vector<BlockMetadata> blocks = {
      make_block(192, 32, "third"),
      make_block(64, 32, "second"),
      make_block(0, 32, "first"),
  };

  auto report = analyzer_.analyze(blocks, kArenaCapacity);

  ASSERT_GE(report.lines.size(), 3u);
  for (std::size_t i = 1; i < report.lines.size(); ++i) {
    EXPECT_GT(report.lines[i].line_index, report.lines[i - 1].line_index);
  }
}
