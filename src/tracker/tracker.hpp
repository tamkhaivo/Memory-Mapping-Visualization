#pragma once
/// @file tracker.hpp
/// @brief Out-of-band allocation tracker using a sorted map of active blocks.

#include "tracker/block_metadata.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <vector>

namespace mmap_viz {

class FreeListAllocator;

/// @brief Callback signature for real-time event notification.
using EventCallback = std::function<void(const AllocationEvent &)>;

/// @brief Tracks all allocation events out-of-band (not inline with allocated
/// data).
///
/// Maintains a map of active blocks keyed by offset, and a full event log
/// for replay support. Optionally invokes a callback on each event for
/// real-time streaming to the WebSocket server.
class AllocationTracker {
public:
  /// @brief Construct a tracker for the given allocator.
  /// @param allocator Reference to the backing FreeListAllocator.
  /// @param sampling  Event sampling rate (1 = track all, N = track 1/N).
  /// @param callback  Optional callback invoked on each sampled event.
  explicit AllocationTracker(FreeListAllocator &allocator, std::size_t sampling,
                             EventCallback callback = nullptr) noexcept;

  /// @brief Record an allocation event.
  /// @param block Metadata for the newly allocated block.
  /// @return The generated AllocationEvent (empty if not sampled).
  auto record_alloc(BlockMetadata block) -> AllocationEvent;

  /// @brief Record a deallocation event.
  /// @param offset Offset of the block being freed.
  /// @return The generated AllocationEvent (empty if not sampled).
  auto record_dealloc(std::size_t offset) -> AllocationEvent;

  /// @brief Current snapshot of all active (allocated) blocks.
  [[nodiscard]] auto snapshot() const -> std::vector<BlockMetadata>;

  /// @brief Full event history for replay.
  [[nodiscard]] auto event_log() const -> const std::vector<AllocationEvent> &;

  /// @brief Number of currently active (allocated) blocks.
  [[nodiscard]] auto active_block_count() const noexcept -> std::size_t;

  /// @brief Set or replace the event callback.
  void set_callback(EventCallback callback);

private:
  auto make_event(EventType type, BlockMetadata block) -> AllocationEvent;

  FreeListAllocator &allocator_;
  std::unordered_map<std::size_t, BlockMetadata>
      active_blocks_; ///< Keyed by offset.
  std::vector<AllocationEvent> event_log_;
  EventCallback callback_;
  std::size_t sampling_;
  std::size_t next_event_id_ = 0;
};

} // namespace mmap_viz
