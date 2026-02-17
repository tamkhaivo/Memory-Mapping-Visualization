/// @file tracked_resource.cpp
/// @brief Implementation of TrackedResource.

#include "allocator/tracked_resource.hpp"
#include "interface/visualization_arena.hpp"
#include <new>

namespace mmap_viz {

TrackedResource::TrackedResource(VisualizationArena &arena) noexcept
    : arena_{&arena} {}

TrackedResource::~TrackedResource() = default;

void TrackedResource::set_next_tag(std::string tag) {
  next_tag_ = std::move(tag);
}

void TrackedResource::set_arena(VisualizationArena *arena) noexcept {
  arena_ = arena;
}

void *TrackedResource::do_allocate(std::size_t bytes, std::size_t alignment) {
  if (!arena_)
    throw std::bad_alloc{};
  void *ptr = arena_->alloc_raw(bytes, alignment, next_tag_);
  next_tag_.clear(); // Reset tag
  if (!ptr) {
    throw std::bad_alloc{};
  }
  return ptr;
}

void TrackedResource::do_deallocate(void *ptr, std::size_t bytes,
                                    std::size_t /*alignment*/) {
  if (arena_)
    arena_->dealloc_raw(ptr, bytes);
}

bool TrackedResource::do_is_equal(
    const std::pmr::memory_resource &other) const noexcept {
  return this == &other;
}

} // namespace mmap_viz
