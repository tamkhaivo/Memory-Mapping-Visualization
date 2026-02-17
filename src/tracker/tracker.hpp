#pragma once
/// @file tracker.hpp
/// @brief Out-of-band allocation tracker using a sorted map of active blocks.

#include "tracker/block_metadata.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <vector>

#include "allocator/free_list.hpp"

namespace mmap_viz {

/// @brief Callback signature for real-time event notification.
using EventCallback = std::function<void(const AllocationEvent &)>;

/// @brief Tracks all allocation events out-of-band (not inline with allocated
/// data).
///
/// Maintains a map of active blocks keyed by offset, and a full event log
/// for replay support. Optionally invokes a callback on each event for
/// real-time streaming to the WebSocket server.
/// @brief Fixed-size lock-free ring buffer for allocation events.
template <typename T, std::size_t N> class RingBuffer {
public:
  void push(T &&item) {
    auto head = head_.load(std::memory_order_relaxed);
    auto next_head = (head + 1) % N;
    if (next_head != tail_.load(std::memory_order_acquire)) {
      buffer_[head] = std::move(item);
      head_.store(next_head, std::memory_order_release);
    }
  }

  bool pop(T &item) {
    auto tail = tail_.load(std::memory_order_relaxed);
    if (tail == head_.load(std::memory_order_acquire)) {
      return false;
    }
    item = std::move(buffer_[tail]);
    tail_.store((tail + 1) % N, std::memory_order_release);
    return true;
  }

private:
  std::array<T, N> buffer_;
  std::atomic<std::size_t> head_{0};
  std::atomic<std::size_t> tail_{0};
};

/// @brief Thread-local tracker that writes to a ring buffer.
class LocalTracker {
public:
  explicit LocalTracker(FreeListAllocator &allocator,
                        std::size_t sampling = 1) noexcept
      : allocator_{allocator}, sampling_{sampling} {}

  void record_alloc(BlockMetadata block) {
    if (++next_event_id_ % sampling_ != 0)
      return;

    AllocationEvent event{
        .type = EventType::Allocate,
        .block = std::move(block),
        .event_id = next_event_id_,
        .total_allocated = allocator_.bytes_allocated(),
        .total_free = allocator_.bytes_free(),
        .fragmentation_pct = 0, // Calculated centrally to avoid overhead
        .free_block_count = allocator_.free_block_count(),
    };
    event_buffer_.push(std::move(event));
  }

  void record_dealloc(std::size_t offset, std::size_t size) {
    if (++next_event_id_ % sampling_ != 0)
      return;

    BlockMetadata block{
        .offset = offset,
        .actual_size = size,
        .timestamp = std::chrono::system_clock::now(),
    };
    AllocationEvent event{
        .type = EventType::Deallocate,
        .block = std::move(block),
        .event_id = next_event_id_,
        .total_allocated = allocator_.bytes_allocated(),
        .total_free = allocator_.bytes_free(),
        .fragmentation_pct = 0,
        .free_block_count = allocator_.free_block_count(),
    };
    event_buffer_.push(std::move(event));
  }

  // Drain events into a vector (called by server thread)
  void drain_to(std::vector<AllocationEvent> &out) {
    AllocationEvent evt;
    while (event_buffer_.pop(evt)) {
      out.push_back(std::move(evt));
    }
  }

private:
  FreeListAllocator &allocator_;
  RingBuffer<AllocationEvent, 4096> event_buffer_; // 4K events per thread
  std::size_t sampling_;
  std::size_t next_event_id_ = 0;
};

} // namespace mmap_viz
