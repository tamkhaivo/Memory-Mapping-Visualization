/// @file tracker.cpp
/// @brief Implementation of AllocationTracker.

#include "tracker/tracker.hpp"
#include "allocator/free_list.hpp"

namespace mmap_viz {

AllocationTracker::AllocationTracker(FreeListAllocator &allocator,
                                     std::size_t sampling,
                                     EventCallback callback) noexcept
    : allocator_{allocator}, callback_{std::move(callback)},
      sampling_{sampling > 0 ? sampling : 1} {}

auto AllocationTracker::make_event(EventType type, BlockMetadata block)
    -> AllocationEvent {
  // Calculate external fragmentation: 1 - (largest_free / total_free).
  auto total_free = allocator_.bytes_free();
  auto largest_free = allocator_.largest_free_block();
  std::size_t frag_pct = 0;

  if (total_free > 0 && largest_free < total_free) {
    frag_pct =
        static_cast<std::size_t>((1.0 - static_cast<double>(largest_free) /
                                            static_cast<double>(total_free)) *
                                 100.0);
  }

  AllocationEvent event{
      .type = type,
      .block = std::move(block),
      .event_id = next_event_id_,
      .total_allocated = allocator_.bytes_allocated(),
      .total_free = total_free,
      .fragmentation_pct = frag_pct,
      .free_block_count = allocator_.free_block_count(),
  };

  return event;
}

auto AllocationTracker::record_alloc(BlockMetadata block) -> AllocationEvent {
  auto offset = block.offset;

  // Always track active blocks for dealloc lookup.
  active_blocks_.emplace(offset, block); // Copy block for map

  // Sampling check.
  if (++next_event_id_ % sampling_ != 0) {
    return {}; // Return empty event if not sampled.
  }

  auto event = make_event(EventType::Allocate, std::move(block));
  event_log_.push_back(event);

  if (callback_) {
    callback_(event);
  }

  return event;
}

auto AllocationTracker::record_dealloc(std::size_t offset) -> AllocationEvent {
  auto it = active_blocks_.find(offset);

  BlockMetadata block{};
  if (it != active_blocks_.end()) {
    block = std::move(it->second);
    active_blocks_.erase(it);
  } else {
    // Untracked block or double free.
    block.offset = offset;
  }

  // Sampling check.
  if (++next_event_id_ % sampling_ != 0) {
    return {};
  }

  auto event = make_event(EventType::Deallocate, std::move(block));
  event_log_.push_back(event);

  if (callback_) {
    callback_(event);
  }

  return event;
}

auto AllocationTracker::snapshot() const -> std::vector<BlockMetadata> {
  std::vector<BlockMetadata> result;
  result.reserve(active_blocks_.size());
  for (const auto &[offset, meta] : active_blocks_) {
    result.push_back(meta);
  }
  return result;
}

auto AllocationTracker::event_log() const
    -> const std::vector<AllocationEvent> & {
  return event_log_;
}

auto AllocationTracker::active_block_count() const noexcept -> std::size_t {
  return active_blocks_.size();
}

void AllocationTracker::set_callback(EventCallback callback) {
  callback_ = std::move(callback);
}

} // namespace mmap_viz
