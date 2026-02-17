#pragma once
/// @file tracked_resource.hpp
/// @brief std::pmr::memory_resource subclass that wraps VisualizationArena.

#include <memory_resource>
#include <string>

namespace mmap_viz {

class VisualizationArena;

/// @brief PMR memory resource that tracks every alloc/dealloc through the
/// Arena.
class TrackedResource final : public std::pmr::memory_resource {
public:
  /// @brief Construct a tracked resource.
  /// @param arena The backing visualization arena.
  explicit TrackedResource(VisualizationArena &arena) noexcept;
  ~TrackedResource() override;

  /// @brief Set a tag that will be applied to the next allocation.
  /// Resets after each allocation.
  void set_next_tag(std::string tag);

  /// @brief Update the backing arena pointer (used after move).
  void set_arena(VisualizationArena *arena) noexcept;

protected:
  void *do_allocate(std::size_t bytes, std::size_t alignment) override;

  void do_deallocate(void *ptr, std::size_t bytes,
                     std::size_t alignment) override;

  bool
  do_is_equal(const std::pmr::memory_resource &other) const noexcept override;

private:
  VisualizationArena *arena_;
  std::string next_tag_;
};

} // namespace mmap_viz
