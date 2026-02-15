#pragma once
/// @file visualization_arena.hpp
/// @brief Single-entry-point façade for instrumented memory allocation.
///
/// Wraps the full pipeline (Arena → FreeListAllocator → AllocationTracker →
/// TrackedResource) into one object. Provides typed and raw allocation,
/// PMR interop, and diagnostic queries (padding waste, cache utilization).
///
/// Usage:
/// @code
///   auto arena = mmap_viz::VisualizationArena::create({.arena_size = 1 <<
///   20}); int* p = arena->alloc<int>("counter"); *p = 42; auto cache =
///   arena->cache_report(); auto pad   = arena->padding_report();
///   arena->dealloc(p);
/// @endcode

#include "allocator/arena.hpp"
#include "allocator/free_list.hpp"
#include "allocator/tracked_resource.hpp"
#include "interface/cache_analyzer.hpp"
#include "interface/padding_inspector.hpp"
#include "tracker/tracker.hpp"

#include <cstddef>
#include <expected>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace mmap_viz {

// Forward-declare WsServer to keep the header lightweight.
class WsServer;

/// @brief Configuration for VisualizationArena construction.
struct ArenaConfig {
  std::size_t arena_size = 1024 * 1024; ///< Total arena capacity (bytes).
  std::size_t cache_line_size = 0;      ///< 0 = auto-detect at runtime.
  bool enable_server = false;           ///< Start WebSocket server.
  unsigned short port = 8080;           ///< Server port (if enabled).
  std::string web_root = "web";         ///< Static file root (if enabled).
};

/// @brief Single-object façade wrapping the entire instrumented allocation
/// pipeline.
///
/// Owns: Arena, FreeListAllocator, AllocationTracker, TrackedResource,
/// CacheAnalyzer, and optionally WsServer.
///
/// Thread-safety: individual allocations are **not** internally synchronized.
/// The caller must provide external synchronization if allocating from
/// multiple threads. Diagnostic queries (reports, JSON) take a snapshot
/// under a lock.
class VisualizationArena {
public:
  /// @brief Create a fully initialized VisualizationArena.
  /// @param cfg Configuration parameters.
  /// @return Initialized arena, or error_code on mmap failure.
  [[nodiscard]] static auto create(ArenaConfig cfg = {})
      -> std::expected<VisualizationArena, std::error_code>;

  ~VisualizationArena();

  // Move-only (owns mmap memory).
  VisualizationArena(VisualizationArena &&other) noexcept;
  VisualizationArena &operator=(VisualizationArena &&other) noexcept;
  VisualizationArena(const VisualizationArena &) = delete;
  VisualizationArena &operator=(const VisualizationArena &) = delete;

  // ─── Typed allocation ────────────────────────────────────────────────

  /// @brief Allocate and construct a T within the arena.
  /// @tparam T    Type to construct.
  /// @tparam Args Constructor argument types.
  /// @param tag   Diagnostic tag for this allocation.
  /// @param args  Forwarded to T's constructor.
  /// @return Pointer to the constructed T, or nullptr on OOM.
  template <typename T, typename... Args>
  auto alloc(std::string_view tag, Args &&...args) -> T * {
    auto *raw = alloc_raw(sizeof(T), alignof(T), tag);
    if (raw == nullptr) {
      return nullptr;
    }
    return ::new (raw) T(std::forward<Args>(args)...);
  }

  /// @brief Destruct and deallocate a T previously allocated with alloc<T>().
  /// @tparam T Type to destroy.
  /// @param ptr Pointer returned by alloc<T>().
  template <typename T> void dealloc(T *ptr) {
    if (ptr == nullptr) {
      return;
    }
    ptr->~T();
    dealloc_raw(ptr, sizeof(T));
  }

  // ─── Raw allocation ──────────────────────────────────────────────────

  /// @brief Allocate raw bytes from the arena.
  /// @param size      Requested size in bytes.
  /// @param alignment Required alignment (power of 2).
  /// @param tag       Diagnostic tag.
  /// @return Pointer to allocated memory, or nullptr on failure.
  auto alloc_raw(std::size_t size, std::size_t alignment, std::string_view tag)
      -> void *;

  /// @brief Deallocate raw bytes previously allocated via alloc_raw().
  /// @param ptr  Pointer returned by alloc_raw().
  /// @param size Original requested size.
  void dealloc_raw(void *ptr, std::size_t size);

  // ─── PMR interop ─────────────────────────────────────────────────────

  /// @brief Get a std::pmr::memory_resource* backed by this arena.
  /// @return Non-owning pointer; valid for the lifetime of this arena.
  [[nodiscard]] auto resource() noexcept -> std::pmr::memory_resource *;

  // ─── Diagnostics ─────────────────────────────────────────────────────

  /// @brief Generate a padding waste report for all active allocations.
  [[nodiscard]] auto padding_report() const -> PaddingReport;

  /// @brief Generate a cache-line utilization report.
  [[nodiscard]] auto cache_report() const -> CacheReport;

  /// @brief Get the current snapshot as a JSON string.
  [[nodiscard]] auto snapshot_json() const -> std::string;

  /// @brief Get the full event history as a JSON string.
  [[nodiscard]] auto event_log_json() const -> std::string;

  // ─── Accessors ───────────────────────────────────────────────────────

  /// @brief Total arena capacity in bytes.
  [[nodiscard]] auto capacity() const noexcept -> std::size_t;

  /// @brief Bytes currently allocated.
  [[nodiscard]] auto bytes_allocated() const noexcept -> std::size_t;

  /// @brief Bytes currently free.
  [[nodiscard]] auto bytes_free() const noexcept -> std::size_t;

  /// @brief Number of currently active (allocated) blocks.
  [[nodiscard]] auto active_block_count() const noexcept -> std::size_t;

  /// @brief The cache-line size used by the analyzer.
  [[nodiscard]] auto cache_line_size() const noexcept -> std::size_t;

  /// @brief Base address of the underlying arena.
  [[nodiscard]] auto base() const noexcept -> std::byte *;

private:
  VisualizationArena() = default;

  // unique_ptr gives the Arena a stable address so FreeListAllocator can
  // hold a reference to it even if VisualizationArena moves.
  std::unique_ptr<Arena> arena_;
  std::unique_ptr<FreeListAllocator> allocator_;
  std::unique_ptr<AllocationTracker> tracker_;
  std::unique_ptr<TrackedResource> resource_;
  CacheAnalyzer cache_analyzer_;

  // Optional server (heap-allocated to avoid pulling in Boost headers).
  std::unique_ptr<WsServer> server_;

  // Track raw alloc sizes for typed dealloc (ptr → size mapping).
  std::unordered_map<void *, std::size_t> alloc_sizes_;

  // Protects alloc_sizes_ and diagnostic queries.
  mutable std::mutex diag_mutex_;
};

} // namespace mmap_viz
