#pragma once
/// @file block_metadata.hpp
/// @brief Data structures for allocation tracking and event recording.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace mmap_viz {

/// @brief Metadata for a single allocated block.
struct BlockMetadata {
  std::size_t offset;      ///< Offset from arena base.
  std::size_t size;        ///< Requested allocation size.
  std::size_t alignment;   ///< Requested alignment.
  std::size_t actual_size; ///< Size including alignment padding.
  char tag[32] = {};       ///< Optional label (fixed buffer to avoid malloc).
  std::chrono::steady_clock::time_point timestamp; ///< When the event occurred.

  void set_tag(std::string_view t) {
    std::size_t len = std::min(t.size(), sizeof(tag) - 1);
    std::memcpy(tag, t.data(), len);
    tag[len] = '\0';
  }
};

/// @brief Type of allocation event.
enum class EventType : std::uint8_t {
  Allocate,
  Deallocate,
};

/// @brief A recorded allocation or deallocation event with aggregate stats.
struct AllocationEvent {
  EventType type;
  BlockMetadata block;
  std::size_t event_id; ///< Monotonically increasing event counter.
  std::size_t
      total_allocated;    ///< Running total allocated bytes after this event.
  std::size_t total_free; ///< Running total free bytes after this event.
  std::size_t fragmentation_pct; ///< External fragmentation percentage (0–100).
  std::size_t free_block_count;  ///< Number of free blocks after this event.
};

} // namespace mmap_viz
