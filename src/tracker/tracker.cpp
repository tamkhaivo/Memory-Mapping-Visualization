/// @file tracker.cpp
/// @brief Implementation of AllocationTracker.

#include "tracker/tracker.hpp"
#include "allocator/free_list.hpp"

namespace mmap_viz {

AllocationTracker::AllocationTracker(FreeListAllocator &allocator,
                                     EventCallback callback) noexcept
    : allocator_{allocator}, callback_{std::move(callback)} {}

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
      .event_id = next_event_id_++,
      .total_allocated = allocator_.bytes_allocated(),
      .total_free = total_free,
      .fragmentation_pct = frag_pct,
      .free_block_count = allocator_.free_block_count(),
  };

  return event;
}

auto AllocationTracker::record_alloc(BlockMetadata block) -> AllocationEvent {
  auto offset = block.offset;
  auto event = make_event(EventType::Allocate, block);

  active_blocks_.emplace(offset, std::move(block));
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
    // Unknown block — still record the event with minimal info.
    block.offset = offset;
  }

  auto event = make_event(EventType::Deallocate, block);
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
