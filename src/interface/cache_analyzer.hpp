#pragma once
/// @file cache_analyzer.hpp
/// @brief Cache-line utilization analyzer for arena allocations.
///
/// Maps active allocation blocks to hardware cache lines and computes
/// per-line utilization, split-allocation detection, and aggregate
/// efficiency metrics. Data produced here feeds directly into the
/// visualization frontend for cache-optimization workflows.

#include "tracker/block_metadata.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace mmap_viz {

/// @brief Per-cache-line occupancy and utilization details.
struct CacheLineInfo {
  std::size_t line_index;   ///< Cache-line ordinal from arena base.
  std::size_t line_offset;  ///< Byte offset of this line within the arena.
  std::size_t bytes_used;   ///< Bytes occupied by live allocations.
  std::size_t bytes_wasted; ///< Unused / padding bytes in this line.
  float utilization;        ///< bytes_used / cache_line_size  (0.0–1.0).
  bool is_split; ///< True if any allocation straddles this line boundary.
  std::vector<std::string> tags; ///< Tags of blocks touching this cache line.
};

/// @brief Aggregate cache-utilization report over the entire arena.
struct CacheReport {
  std::size_t cache_line_size;   ///< Hardware cache-line width (bytes).
  std::size_t total_lines;       ///< Total cache lines in the arena.
  std::size_t active_lines;      ///< Lines touched by ≥1 allocation.
  std::size_t fully_utilized;    ///< Lines at 100% utilization.
  std::size_t split_allocations; ///< Allocations that straddle a line boundary.
  float avg_utilization;         ///< Mean utilization across active lines.
  std::vector<CacheLineInfo> lines; ///< Per-line detail (only active lines).
};

/// @brief Analyzes cache-line utilization from a set of allocation blocks.
///
/// Stateless: each call to analyze() produces an independent report.
/// The cache-line size is configurable to support x86-64 (64 B),
/// Apple Silicon (128 B), or any custom line width.
class CacheAnalyzer {
public:
  /// @brief Construct an analyzer for a given cache-line width.
  /// @param cache_line_size Must be a power of 2 (default 64 for x86-64).
  explicit CacheAnalyzer(std::size_t cache_line_size = 64) noexcept;

  /// @brief Analyze cache-line utilization for the given blocks.
  /// @param blocks        Snapshot of currently active allocations.
  /// @param arena_capacity Total arena capacity in bytes.
  /// @return CacheReport with per-line and aggregate metrics.
  [[nodiscard]] auto analyze(std::span<const BlockMetadata> blocks,
                             std::size_t arena_capacity) const -> CacheReport;

  /// @brief The configured cache-line size.
  [[nodiscard]] auto line_size() const noexcept -> std::size_t;

  /// @brief Detect the hardware cache-line size at runtime.
  /// Falls back to 64 if detection fails.
  [[nodiscard]] static auto detect_line_size() noexcept -> std::size_t;

private:
  std::size_t cache_line_size_;
};

} // namespace mmap_viz
