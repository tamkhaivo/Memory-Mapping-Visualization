/// @file cache_analyzer.cpp
/// @brief Implementation of cache-line utilization analysis.

#include "interface/cache_analyzer.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <unordered_map>

#ifdef __APPLE__
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace mmap_viz {

CacheAnalyzer::CacheAnalyzer(std::size_t cache_line_size) noexcept
    : cache_line_size_{cache_line_size} {
  // Enforce power-of-2 constraint. Fall back to 64 if invalid.
  if (!std::has_single_bit(cache_line_size_) || cache_line_size_ == 0) {
    cache_line_size_ = 64;
  }
}

auto CacheAnalyzer::line_size() const noexcept -> std::size_t {
  return cache_line_size_;
}

auto CacheAnalyzer::detect_line_size() noexcept -> std::size_t {
#ifdef __APPLE__
  std::size_t line_size = 0;
  std::size_t size = sizeof(line_size);
  if (::sysctlbyname("hw.cachelinesize", &line_size, &size, nullptr, 0) == 0 &&
      line_size > 0 && std::has_single_bit(line_size)) {
    return line_size;
  }
#elif defined(__linux__)
  long result = ::sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
  if (result > 0 && std::has_single_bit(static_cast<std::size_t>(result))) {
    return static_cast<std::size_t>(result);
  }
#endif
  return 64; // Conservative default for x86-64.
}

auto CacheAnalyzer::analyze(std::span<const BlockMetadata> blocks,
                            std::size_t arena_capacity) const -> CacheReport {
  CacheReport report{};
  report.cache_line_size = cache_line_size_;
  report.total_lines =
      (arena_capacity + cache_line_size_ - 1) / cache_line_size_;
  report.active_lines = 0;
  report.fully_utilized = 0;
  report.split_allocations = 0;
  report.avg_utilization = 0.0f;

  if (blocks.empty() || arena_capacity == 0) {
    return report;
  }

  // Per-line accumulator: line_index → (bytes_used, is_split, tags).
  struct LineAccum {
    std::size_t bytes_used = 0;
    bool is_split = false;
    std::vector<std::string> tags;
  };
  std::unordered_map<std::size_t, LineAccum> line_map;

  for (const auto &block : blocks) {
    const auto block_start = block.offset;
    const auto block_end = block.offset + block.actual_size;

    // Determine which cache lines this block spans.
    const auto first_line = block_start / cache_line_size_;
    const auto last_line =
        (block_end > 0) ? (block_end - 1) / cache_line_size_ : first_line;

    // If the block spans more than one line, it's a split allocation.
    if (last_line > first_line) {
      ++report.split_allocations;
    }

    for (auto line = first_line; line <= last_line; ++line) {
      const auto line_start = line * cache_line_size_;
      const auto line_end = line_start + cache_line_size_;

      // Clamp the block to this cache line's boundaries.
      const auto overlap_start = std::max(block_start, line_start);
      const auto overlap_end = std::min(block_end, line_end);

      if (overlap_start >= overlap_end) {
        continue; // No overlap (shouldn't happen given loop bounds).
      }

      auto &accum = line_map[line];
      accum.bytes_used += (overlap_end - overlap_start);

      // Mark as split if this isn't the only line for this block.
      if (last_line > first_line) {
        accum.is_split = true;
      }

      if (!block.tag.empty()) {
        accum.tags.push_back(block.tag);
      }
    }
  }

  // Build per-line info and aggregate stats.
  float total_utilization = 0.0f;

  for (auto &[line_idx, accum] : line_map) {
    // Clamp bytes_used to cache_line_size_ (overlapping blocks could
    // theoretically exceed it if the allocator allows, but shouldn't).
    accum.bytes_used = std::min(accum.bytes_used, cache_line_size_);

    const auto wasted = cache_line_size_ - accum.bytes_used;
    const auto util = static_cast<float>(accum.bytes_used) /
                      static_cast<float>(cache_line_size_);

    CacheLineInfo info{
        .line_index = line_idx,
        .line_offset = line_idx * cache_line_size_,
        .bytes_used = accum.bytes_used,
        .bytes_wasted = wasted,
        .utilization = util,
        .is_split = accum.is_split,
        .tags = std::move(accum.tags),
    };

    if (accum.bytes_used == cache_line_size_) {
      ++report.fully_utilized;
    }

    total_utilization += util;
    report.lines.push_back(std::move(info));
  }

  report.active_lines = report.lines.size();

  if (report.active_lines > 0) {
    report.avg_utilization =
        total_utilization / static_cast<float>(report.active_lines);
  }

  // Sort lines by offset for deterministic output.
  std::sort(report.lines.begin(), report.lines.end(),
            [](const CacheLineInfo &a, const CacheLineInfo &b) {
              return a.line_index < b.line_index;
            });

  return report;
}

} // namespace mmap_viz
