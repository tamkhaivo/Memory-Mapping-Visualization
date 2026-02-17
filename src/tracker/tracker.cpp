/// @file tracker.cpp
/// @brief Implementation of AllocationTracker.

#include "tracker/tracker.hpp"
#include "allocator/free_list.hpp"
#include <algorithm>

namespace mmap_viz {

AllocationTracker::AllocationTracker(FreeListAllocator &allocator,
                                     std::size_t sampling,
                                     EventCallback callback) noexcept
    : allocator_{allocator}, callback_{std::move(callback)},
      sampling_{sampling > 0 ? sampling : 1} {}

auto AllocationTracker::make_event(EventType type, BlockMetadata block)
    -> AllocationEvent {
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
  active_block_count_++;
  if (++next_event_id_ % sampling_ != 0) {
    return {};
  }

  auto event = make_event(EventType::Allocate, std::move(block));
  event_log_.push_back(event);

  if (callback_) {
    callback_(event);
  }

  return event;
}

auto AllocationTracker::record_dealloc(std::size_t offset) -> AllocationEvent {
  if (active_block_count_ > 0) {
    active_block_count_--;
  }

  // We don't have size/tag easily available without map.
  // But we can construct a partial metadata for the event.
  // Ideally we would read the header from offset, but offset is from base.
  // It's safe to read?
  // offset points to Header Start (from allocator design).
  // Let's try to recover size/tag for better visualization events.

  BlockMetadata block{};
  block.offset = offset;

  // Attempt to read header
  auto *ptr = allocator_.base() + offset;
  auto *header = reinterpret_cast<AllocationHeader *>(ptr);
  // Check magic
  if (header->magic == AllocationHeader::kMagicValue) {
    block.actual_size = header->size;
    block.size = header->size - sizeof(AllocationHeader); // Estimate
    block.set_tag(header->tag);
  }

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

  // Walk the heap
  auto *base = allocator_.base();
  std::size_t capacity = allocator_.capacity();
  std::size_t offset = 0;

  while (offset < capacity) {
    auto *ptr = base + offset;

    // Peek at size (common to FreeBlock and AllocationHeader)
    // We need to cast to something that has size at offset 0.
    struct GenericHeader {
      std::size_t size;
    };
    auto *generic = reinterpret_cast<GenericHeader *>(ptr);
    std::size_t block_size = generic->size;

    if (block_size == 0) {
      // Sentinel or End or Error. Prevent infinite loop.
      break;
    }

    // Check if Allocated
    auto *header = reinterpret_cast<AllocationHeader *>(ptr);
    if (header->magic == AllocationHeader::kMagicValue) {
      // Allocated Block
      BlockMetadata meta;
      meta.offset = offset;
      meta.actual_size = block_size;
      meta.size = block_size - sizeof(AllocationHeader);
      meta.set_tag(header->tag);
      // Timestamp? We don't store timestamp in header.
      // Snapshot won't have correct timestamp.
      // Is this critical?
      // Visualization usually colors by age.
      // Without timestamp, we lose age coloring on reload.
      // Acceptable trade-off for performance.
      meta.timestamp = std::chrono::steady_clock::time_point{};

      result.push_back(meta);
    }

    offset += block_size;
  }

  return result;
}

auto AllocationTracker::event_log() const
    -> const std::vector<AllocationEvent> & {
  return event_log_;
}

auto AllocationTracker::active_block_count() const noexcept -> std::size_t {
  return active_block_count_;
}

void AllocationTracker::set_callback(EventCallback callback) {
  callback_ = std::move(callback);
}

} // namespace mmap_viz
