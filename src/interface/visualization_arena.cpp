/// @file visualization_arena.cpp
/// @brief Implementation of the VisualizationArena façade.

#include "interface/visualization_arena.hpp"
#include "serialization/json_serializer.hpp"
#include "server/ws_server.hpp"

#include <nlohmann/json.hpp>

#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace mmap_viz {

// Forward declare ThreadContext
// No need, it's in header as member.

std::atomic<std::size_t> VisualizationArena::global_generation_{1};

static constexpr std::size_t kMaxShards = 256;

// ─── Impl Definition ─────────────────────────────────────────────────────

struct VisualizationArena::Impl {
  Impl(ArenaConfig cfg) : config(cfg) {}

  ArenaConfig config;

  // Global state
  std::unique_ptr<Arena> arena;
  CacheAnalyzer cache_analyzer;

  // Sharding
  struct Shard {
    alignas(64) std::mutex mutex;
    std::unique_ptr<FreeListAllocator> allocator;
  };
  std::vector<std::unique_ptr<Shard>> shards;
  std::atomic<std::size_t> next_shard_idx{0};

  // Server & Aggregation
  struct Batcher {
    std::mutex mutex;
    std::vector<AllocationEvent> events;
  };
  std::shared_ptr<Batcher> batcher;
  std::unique_ptr<WsServer> server;

  // Registration for aggregation
  std::mutex contexts_mutex;
  std::vector<std::weak_ptr<ThreadContext>> active_contexts;

  // PMR Resource
  std::unique_ptr<TrackedResource> resource;

  // Control
  std::atomic<bool> running{true};
  std::size_t generation = 0;

  // Methods moved to Impl to simplify callbacks
  auto snapshot_json() const -> std::string;
  auto event_log_json() const -> std::string;
};

// ─── ThreadContext Definition ────────────────────────────────────────────

struct VisualizationArena::ThreadContext {
  std::size_t generation = 0;
  Impl::Shard *shard = nullptr;
  std::unique_ptr<LocalTracker> tracker;
};

thread_local std::shared_ptr<VisualizationArena::ThreadContext>
    VisualizationArena::tls_context_ = nullptr;

// ─── Impl Methods ────────────────────────────────────────────────────────

auto VisualizationArena::Impl::snapshot_json() const -> std::string {
  std::vector<BlockMetadata> blocks;
  std::size_t total_allocated = 0;
  std::size_t total_free = 0;
  std::size_t free_blocks = 0;

  for (const auto &shard : shards) {
    if (!shard)
      continue;
    std::lock_guard lock(shard->mutex);
    auto *alloc = shard->allocator.get();

    total_allocated += alloc->bytes_allocated();
    total_free += alloc->bytes_free();
    free_blocks += alloc->free_block_count();

    // Walk heap
    auto *base = alloc->base();
    std::size_t cap = alloc->capacity();
    std::size_t offset = 0;

    while (offset + sizeof(AllocationHeader) <= cap) {
      auto *ptr = base + offset;
      auto *header = reinterpret_cast<AllocationHeader *>(ptr);
      std::size_t block_size = 0;
      bool is_allocated = false;

      if (header->magic == AllocationHeader::kMagicValue) {
        block_size = header->size;
        is_allocated = true;
      } else {
        struct GenericHeader {
          std::size_t size;
        };
        auto *generic = reinterpret_cast<GenericHeader *>(ptr);
        block_size = generic->size;
      }

      if (block_size == 0 || block_size > cap || offset + block_size > cap) {
        break;
      }

      if (is_allocated) {
        BlockMetadata meta;
        meta.offset = static_cast<std::size_t>(ptr - arena->base());
        meta.actual_size = block_size;
        meta.size = block_size > sizeof(AllocationHeader)
                        ? block_size - sizeof(AllocationHeader)
                        : 0;

        char safe_tag[33] = {};
        std::memcpy(safe_tag, header->tag, sizeof(header->tag));
        safe_tag[32] = '\0';

        // Sanitize tag for JSON
        for (int i = 0; i < 32 && safe_tag[i] != '\0'; ++i) {
          if (static_cast<unsigned char>(safe_tag[i]) < 32 ||
              static_cast<unsigned char>(safe_tag[i]) > 126) {
            safe_tag[i] = '?';
          }
        }

        meta.set_tag(safe_tag);

        blocks.push_back(meta);
      }
      offset += block_size;
    }
  }

  auto j = snapshot_to_json(blocks, total_allocated, total_free,
                            arena->capacity(), 0, free_blocks);
  return j.dump();
}

auto VisualizationArena::Impl::event_log_json() const -> std::string {
  // Lock contexts to prevent batcher thread from draining concurrently
  std::lock_guard lock(const_cast<std::mutex &>(contexts_mutex));
  std::lock_guard batch_lock(const_cast<std::mutex &>(batcher->mutex));

  // Drain all contexts into batcher
  for (const auto &weak_ctx : active_contexts) {
    if (auto ctx = weak_ctx.lock()) {
      if (ctx->tracker) {
        ctx->tracker->drain_to(batcher->events);
      }
    }
  }

  // Serialize
  std::stringstream ss;
  ss << "[";
  for (size_t i = 0; i < batcher->events.size(); ++i) {
    nlohmann::json j = batcher->events[i];
    ss << j.dump();
    if (i < batcher->events.size() - 1) {
      ss << ",";
    }
  }
  ss << "]";
  return ss.str();
}

// ─── VisualizationArena ──────────────────────────────────────────────────

auto VisualizationArena::create(ArenaConfig cfg)
    -> std::expected<VisualizationArena, std::error_code> {

  // 1. Create the mmap-backed arena.
  auto arena_result = Arena::create(cfg.arena_size);
  if (!arena_result.has_value()) {
    return std::unexpected(arena_result.error());
  }

  // 2. Initialize Impl
  auto impl = std::make_unique<Impl>(cfg);
  impl->arena = std::make_unique<Arena>(std::move(*arena_result));
  impl->shards.resize(kMaxShards);

  // 3. Resolve cache-line size.
  auto line_sz = (cfg.cache_line_size == 0) ? CacheAnalyzer::detect_line_size()
                                            : cfg.cache_line_size;
  impl->cache_analyzer = CacheAnalyzer{line_sz};

  // 4. Build server and batcher
  impl->batcher = std::make_shared<Impl::Batcher>();

  // Initialize all shards upfront to avoid races and O(1) allocation path
  std::byte *base = impl->arena->base();
  std::size_t total_cap = impl->arena->capacity();
  std::size_t shard_size = total_cap / kMaxShards;

  for (std::size_t i = 0; i < kMaxShards; ++i) {
    auto shard = std::make_unique<Impl::Shard>();
    std::byte *shard_base = base + (i * shard_size);
    shard->allocator =
        std::make_unique<FreeListAllocator>(shard_base, shard_size);
    impl->shards[i] = std::move(shard);
  }

  if (cfg.enable_server) {
    impl->server = std::make_unique<WsServer>(cfg.port, cfg.web_root, nullptr);

    // Snapshot provider
    impl->server->set_snapshot_provider(
        [raw_impl = impl.get()]() -> std::string {
          return raw_impl->snapshot_json();
        });
  }

  // 5. Build PMR resource (needs facade for set_arena later, but construction
  // just needs valid object) Trick: TrackedResource expects
  // VisualizationArena&. We can't pass 'va' yet because it's not constructed.
  // But TrackedResource stores a pointer.
  // We can initialize it later or pass a placeholder?
  // Actually, implementation of TrackedResource constructor: it stores &arena.
  // We need to construct VA first, then Resource.
  // But Resource is in Impl.
  // Impl is created before VA.
  // So Impl cannot hold Resource initialized with VA?
  // Circular dependency.
  // Fix: Initialize Resource AFTER VA is created.
  // But Resource is unique_ptr in Impl. We can construct it later.

  impl->generation = global_generation_.fetch_add(1);

  VisualizationArena va;
  va.impl_ = std::move(impl);

  // Now we can initialize Resource with 'va'
  va.impl_->resource = std::make_unique<TrackedResource>(va);

  // 6. Start threads if enabled
  if (cfg.enable_server) {
    auto *raw_impl = va.impl_.get();

    // Server thread
    va.server_thread_ = std::thread([raw_impl]() {
      if (raw_impl->server)
        raw_impl->server->run();
    });

    // Batcher thread
    va.batcher_thread_ = std::thread([raw_impl]() {
      while (raw_impl->running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));

        // 1. Drain all TLS buffers into batcher
        {
          std::lock_guard lock(raw_impl->contexts_mutex);
          std::lock_guard batch_lock(raw_impl->batcher->mutex);

          auto it = raw_impl->active_contexts.begin();
          while (it != raw_impl->active_contexts.end()) {
            if (auto ctx = it->lock()) {
              ctx->tracker->drain_to(raw_impl->batcher->events);
              ++it;
            } else {
              it = raw_impl->active_contexts.erase(it);
            }
          }
        }

        // 2. Flush batcher to server
        std::vector<AllocationEvent> batch;
        {
          std::lock_guard lock(raw_impl->batcher->mutex);
          if (raw_impl->batcher->events.empty())
            continue;
          batch.swap(raw_impl->batcher->events);
        }

        if (raw_impl->server) {
          std::string payload = "[";
          for (size_t i = 0; i < batch.size(); ++i) {
            nlohmann::json j = batch[i];
            payload += j.dump();
            if (i < batch.size() - 1)
              payload += ",";
          }
          payload += "]";
          raw_impl->server->broadcast(payload);
        }
      }
    });
  }

  return va;
}

VisualizationArena::~VisualizationArena() {
  if (impl_) {
    impl_->running = false;
    if (impl_->server) {
      impl_->server->stop();
    }
  }
  if (batcher_thread_.joinable()) {
    batcher_thread_.join();
  }
  if (server_thread_.joinable()) {
    server_thread_.join();
  }
}

VisualizationArena::VisualizationArena(VisualizationArena &&other) noexcept {
  *this = std::move(other);
}

VisualizationArena &
VisualizationArena::operator=(VisualizationArena &&other) noexcept {
  if (this != &other) {
    // Stop current threads
    if (impl_) {
      impl_->running = false;
      if (impl_->server)
        impl_->server->stop();
    }
    if (batcher_thread_.joinable())
      batcher_thread_.join();
    if (server_thread_.joinable())
      server_thread_.join();

    // Move state
    impl_ = std::move(other.impl_);
    batcher_thread_ = std::move(other.batcher_thread_);
    server_thread_ = std::move(other.server_thread_);

    // Update resource back-pointer
    if (impl_ && impl_->resource) {
      impl_->resource->set_arena(this);
    }
  }
  return *this;
}

// ─── TLS Init ────────────────────────────────────────────────────────────

void VisualizationArena::init_tls_context() {
  auto idx = impl_->next_shard_idx.fetch_add(1);
  if (idx >= kMaxShards) {
    // Wrap around or handle overflow
    idx = idx % kMaxShards;
  }

  if (!impl_->shards[idx]) {
    return; // Should not happen with upfront init
  }

  // Create new context
  tls_context_ = std::make_shared<ThreadContext>();
  tls_context_->generation = impl_->generation;
  tls_context_->shard = impl_->shards[idx].get();

  tls_context_->tracker = std::make_unique<LocalTracker>(
      *tls_context_->shard->allocator, impl_->config.sampling);

  {
    std::lock_guard lock(impl_->contexts_mutex);
    // Registration remains the same
    impl_->active_contexts.push_back(tls_context_);
  }
}

auto VisualizationArena::get_shard_idx(void *ptr) const -> std::size_t {
  auto offset = static_cast<std::size_t>(static_cast<std::byte *>(ptr) -
                                         impl_->arena->base());
  std::size_t shard_size = impl_->arena->capacity() / kMaxShards;
  return offset / shard_size;
}

// ─── Raw allocation ──────────────────────────────────────────────────────

auto VisualizationArena::alloc_raw(std::size_t size, std::size_t alignment,
                                   std::string_view tag) -> void * {
  if (!tls_context_ || tls_context_->generation != impl_->generation) {
    init_tls_context();
  }
  if (!tls_context_)
    return nullptr;

  auto *allocator = tls_context_->shard->allocator.get();

  std::size_t header_size = sizeof(AllocationHeader);
  std::size_t footer_size = sizeof(std::uint32_t);
  std::size_t base_overhead = header_size + footer_size;
  std::size_t padding = 0;

  if (alignment > 0) {
    std::size_t remainder = base_overhead % alignment;
    if (remainder != 0) {
      padding = alignment - remainder;
    }
  }

  std::size_t offset_to_user = base_overhead + padding;
  std::size_t total_alloc_size = size + offset_to_user;

  std::lock_guard lock(tls_context_->shard->mutex);
  auto result = allocator->allocate(total_alloc_size, alignment);

  if (!result.has_value()) {
    return nullptr;
  }

  std::byte *raw_ptr = result->ptr;
  std::byte *user_ptr = raw_ptr + offset_to_user;

  std::uint32_t offset_val = static_cast<std::uint32_t>(offset_to_user);
  std::memcpy(user_ptr - sizeof(std::uint32_t), &offset_val,
              sizeof(std::uint32_t));

  auto *header = reinterpret_cast<AllocationHeader *>(raw_ptr);
  header->magic = AllocationHeader::kMagicValue;
  header->size = total_alloc_size;

  std::size_t len = std::min(tag.size(), sizeof(header->tag) - 1);
  std::memcpy(header->tag, tag.data(), len);
  header->tag[len] = '\0';

  BlockMetadata meta{
      .offset = result->offset,
      .size = size,
      .alignment = alignment,
      .actual_size = result->actual_size,
      .timestamp = std::chrono::steady_clock::now(),
  };
  meta.set_tag(tag);
  tls_context_->tracker->record_alloc(std::move(meta));

  return user_ptr;
}

void VisualizationArena::dealloc_raw(void *ptr, std::size_t size) {
  if (ptr == nullptr)
    return;

  std::uint32_t offset_val = 0;
  std::byte *user_ptr = static_cast<std::byte *>(ptr);
  std::memcpy(&offset_val, user_ptr - sizeof(std::uint32_t),
              sizeof(std::uint32_t));

  std::byte *raw_ptr = user_ptr - offset_val;
  auto *header = reinterpret_cast<AllocationHeader *>(raw_ptr);

  if (header->magic != AllocationHeader::kMagicValue) {
    // Invalid magic
  }

  std::size_t idx = get_shard_idx(raw_ptr);
  if (idx >= kMaxShards || !impl_->shards[idx]) {
    return;
  }

  auto *shard = impl_->shards[idx].get();

  if (tls_context_) {
    auto offset = static_cast<std::size_t>(raw_ptr - impl_->arena->base());
    tls_context_->tracker->record_dealloc(offset, size);
  }

  std::lock_guard lock(shard->mutex);
  (void)shard->allocator->deallocate(raw_ptr, header->size);
}

// ─── PMR interop ─────────────────────────────────────────────────────────

auto VisualizationArena::resource() noexcept -> std::pmr::memory_resource * {
  return impl_ ? impl_->resource.get() : nullptr;
}

// ─── Diagnostics ─────────────────────────────────────────────────────────

auto VisualizationArena::padding_report() const -> PaddingReport { return {}; }

auto VisualizationArena::cache_report() const -> CacheReport { return {}; }

auto VisualizationArena::snapshot_json() const -> std::string {
  return impl_ ? impl_->snapshot_json() : "{}";
}

auto VisualizationArena::event_log_json() const -> std::string {
  return impl_ ? impl_->event_log_json() : "[]";
}

void VisualizationArena::set_command_handler(
    std::function<void(const std::string &)> handler) {
  if (impl_ && impl_->server) {
    impl_->server->set_command_handler(std::move(handler));
  }
}

// ─── Accessors ───────────────────────────────────────────────────────────

auto VisualizationArena::capacity() const noexcept -> std::size_t {
  return impl_ && impl_->arena ? impl_->arena->capacity() : 0;
}

auto VisualizationArena::bytes_allocated() const noexcept -> std::size_t {
  if (!impl_)
    return 0;
  std::size_t sum = 0;
  for (const auto &s : impl_->shards)
    if (s)
      sum += s->allocator->bytes_allocated();
  return sum;
}

auto VisualizationArena::bytes_free() const noexcept -> std::size_t {
  if (!impl_)
    return 0;
  std::size_t sum = 0;
  for (const auto &s : impl_->shards)
    if (s)
      sum += s->allocator->bytes_free();
  return sum;
}

auto VisualizationArena::active_block_count() const noexcept -> std::size_t {
  return 0;
}

auto VisualizationArena::cache_line_size() const noexcept -> std::size_t {
  return impl_ ? impl_->cache_analyzer.line_size() : 0;
}

auto VisualizationArena::base() const noexcept -> std::byte * {
  return impl_ && impl_->arena ? impl_->arena->base() : nullptr;
}

} // namespace mmap_viz
