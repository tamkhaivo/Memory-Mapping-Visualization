/// @file visualization_arena.cpp
/// @brief Implementation of the VisualizationArena façade.

#include "interface/visualization_arena.hpp"
#include "serialization/json_serializer.hpp"
#include "server/ws_server.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <thread>
#include <utility>

namespace mmap_viz {

auto VisualizationArena::create(ArenaConfig cfg)
    -> std::expected<VisualizationArena, std::error_code> {

  // 1. Create the mmap-backed arena.
  auto arena_result = Arena::create(cfg.arena_size);
  if (!arena_result.has_value()) {
    return std::unexpected(arena_result.error());
  }

  VisualizationArena va;

  // 2. Move arena into a unique_ptr to ensure address stability.
  va.arena_ = std::make_unique<Arena>(std::move(*arena_result));

  // 3. Build allocator over the arena.
  va.allocator_ = std::make_unique<FreeListAllocator>(*va.arena_);

  // 4. Resolve cache-line size: auto-detect or use provided value.
  auto line_sz = (cfg.cache_line_size == 0) ? CacheAnalyzer::detect_line_size()
                                            : cfg.cache_line_size;
  va.cache_analyzer_ = CacheAnalyzer{line_sz};

  // 5. Build tracker (optionally with server broadcast callback).
  if (cfg.enable_server) {
    va.server_ = std::make_unique<WsServer>(cfg.port, cfg.web_root, nullptr);
    va.batcher_ = std::make_shared<Batcher>();

    // Capture shared_ptr to batcher, ensuring validity even if va moves/dies
    // (though va must live for server)
    va.tracker_ = std::make_unique<AllocationTracker>(
        *va.allocator_, [batcher = va.batcher_](const AllocationEvent &evt) {
          nlohmann::json j = evt;
          std::lock_guard lock(batcher->mutex);
          batcher->events.push_back(j.dump());
        });

    // Wire up snapshot provider.
    auto *tracker_ptr = va.tracker_.get();
    auto *alloc_ptr = va.allocator_.get();
    va.server_->set_snapshot_provider([tracker_ptr,
                                       alloc_ptr]() -> std::string {
      auto blocks = tracker_ptr->snapshot();
      auto j = snapshot_to_json(blocks, alloc_ptr->bytes_allocated(),
                                alloc_ptr->bytes_free(), alloc_ptr->capacity(),
                                0, alloc_ptr->free_block_count());
      return j.dump();
    });

    // Start server in background thread.
    std::thread([server = va.server_.get()]() { server->run(); }).detach();

    // Start batch flusher thread (approx 60Hz).
    // Capture shared_ptr to batcher and raw pointer to server (server owned by
    // generic va life)
    std::thread([batcher = va.batcher_, server = va.server_.get()]() {
      while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));

        // simple flush logic
        std::vector<std::string> batch;
        {
          std::lock_guard lock(batcher->mutex);
          if (batcher->events.empty())
            continue;
          batch.swap(batcher->events);
        }

        if (server) { // server pointer valid as long as main thread runs
          std::string payload = "[";
          for (size_t i = 0; i < batch.size(); ++i) {
            payload += batch[i];
            if (i < batch.size() - 1)
              payload += ",";
          }
          payload += "]";
          server->broadcast(payload);
        }
      }
    }).detach();

  } else {
    va.tracker_ = std::make_unique<AllocationTracker>(*va.allocator_);
  }

  // 6. Build the PMR resource bridge.
  va.resource_ =
      std::make_unique<TrackedResource>(*va.allocator_, *va.tracker_);

  return va;
}

VisualizationArena::~VisualizationArena() {
  if (server_) {
    server_->stop();
  }
}

VisualizationArena::VisualizationArena(VisualizationArena &&other) noexcept
    : arena_{std::move(other.arena_)}, allocator_{std::move(other.allocator_)},
      tracker_{std::move(other.tracker_)},
      resource_{std::move(other.resource_)},
      cache_analyzer_{other.cache_analyzer_}, server_{std::move(other.server_)},
      alloc_sizes_{std::move(other.alloc_sizes_)},
      batcher_{std::move(other.batcher_)} {}

VisualizationArena &
VisualizationArena::operator=(VisualizationArena &&other) noexcept {
  if (this != &other) {
    if (server_) {
      server_->stop();
    }
    // No need to lock mutexes for shared_ptr move, just move the pointer.
    // The underlying Batcher object stays valid for any existing references.

    arena_ = std::move(other.arena_);
    allocator_ = std::move(other.allocator_);
    tracker_ = std::move(other.tracker_);
    resource_ = std::move(other.resource_);
    cache_analyzer_ = other.cache_analyzer_;
    server_ = std::move(other.server_);
    alloc_sizes_ = std::move(other.alloc_sizes_);
    batcher_ = std::move(other.batcher_);
  }
  return *this;
}

// ─── Raw allocation ──────────────────────────────────────────────────────

auto VisualizationArena::alloc_raw(std::size_t size, std::size_t alignment,
                                   std::string_view tag) -> void * {
  resource_->set_next_tag(std::string{tag});

  auto result = allocator_->allocate(size, alignment);
  if (!result.has_value()) {
    return nullptr;
  }

  // Record in tracker (TrackedResource handles this via do_allocate, but
  // we bypass it here for raw access — record manually).
  // Actually, let's use the PMR do_allocate path for consistency:
  // No — that would throw. Use the allocator directly + tracker.
  BlockMetadata meta{
      .offset = result->offset,
      .size = size,
      .alignment = alignment,
      .actual_size = result->actual_size,
      .tag = std::string{tag},
      .timestamp = std::chrono::steady_clock::now(),
  };
  tracker_->record_alloc(std::move(meta));

  {
    std::lock_guard lock(diag_mutex_);
    alloc_sizes_[result->ptr] = size;
  }

  return result->ptr;
}

void VisualizationArena::dealloc_raw(void *ptr, std::size_t size) {
  if (ptr == nullptr) {
    return;
  }

  auto *byte_ptr = static_cast<std::byte *>(ptr);

  // Compute offset from arena base — this is the canonical key for tracker.
  auto offset = static_cast<std::size_t>(byte_ptr - arena_->base());
  auto event = tracker_->record_dealloc(offset);

  // Use the actual size (including padding) if available, otherwise fallback.
  // This ensures the allocator's free bytes accounting stays consistent.
  std::size_t dealloc_size =
      (event.block.actual_size > 0) ? event.block.actual_size : size;

  (void)allocator_->deallocate(byte_ptr, dealloc_size);

  {
    std::lock_guard lock(diag_mutex_);
    alloc_sizes_.erase(ptr);
  }
}

// ─── PMR interop ─────────────────────────────────────────────────────────

auto VisualizationArena::resource() noexcept -> std::pmr::memory_resource * {
  return resource_.get();
}

// ─── Diagnostics ─────────────────────────────────────────────────────────

auto VisualizationArena::padding_report() const -> PaddingReport {
  auto blocks = tracker_->snapshot();
  return compute_padding_report(blocks);
}

auto VisualizationArena::cache_report() const -> CacheReport {
  auto blocks = tracker_->snapshot();
  return cache_analyzer_.analyze(blocks, arena_->capacity());
}

auto VisualizationArena::snapshot_json() const -> std::string {
  auto blocks = tracker_->snapshot();
  auto j = snapshot_to_json(blocks, allocator_->bytes_allocated(),
                            allocator_->bytes_free(), allocator_->capacity(), 0,
                            allocator_->free_block_count());
  return j.dump();
}

auto VisualizationArena::event_log_json() const -> std::string {
  const auto &log = tracker_->event_log();
  nlohmann::json j = nlohmann::json::array();
  for (const auto &evt : log) {
    j.push_back(nlohmann::json(evt));
  }
  return j.dump();
}

// ─── Accessors ───────────────────────────────────────────────────────────

auto VisualizationArena::capacity() const noexcept -> std::size_t {
  return arena_ ? arena_->capacity() : 0;
}

auto VisualizationArena::bytes_allocated() const noexcept -> std::size_t {
  return allocator_ ? allocator_->bytes_allocated() : 0;
}

auto VisualizationArena::bytes_free() const noexcept -> std::size_t {
  return allocator_ ? allocator_->bytes_free() : 0;
}

auto VisualizationArena::active_block_count() const noexcept -> std::size_t {
  return tracker_ ? tracker_->active_block_count() : 0;
}

auto VisualizationArena::cache_line_size() const noexcept -> std::size_t {
  return cache_analyzer_.line_size();
}

auto VisualizationArena::base() const noexcept -> std::byte * {
  return arena_ ? arena_->base() : nullptr;
}

} // namespace mmap_viz
