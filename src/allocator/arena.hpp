#pragma once
/// @file arena.hpp
/// @brief RAII wrapper around mmap/munmap for a contiguous virtual memory arena.

#include <cstddef>
#include <expected>
#include <system_error>

namespace mmap_viz {

/// @brief Owns a contiguous region of virtual memory obtained via mmap.
///
/// Move-only. The region is mapped with PROT_READ|PROT_WRITE,
/// MAP_ANONYMOUS|MAP_PRIVATE. Unmapped on destruction.
class Arena {
public:
    /// @brief Map a contiguous anonymous region of at least @p capacity bytes.
    /// @param capacity Requested size in bytes (rounded up to page boundary).
    /// @return Arena on success, or std::error_code on mmap failure.
    [[nodiscard]] static auto create(std::size_t capacity)
        -> std::expected<Arena, std::error_code>;

    ~Arena();

    // Move-only semantics.
    Arena(Arena&& other) noexcept;
    Arena& operator=(Arena&& other) noexcept;
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    /// @brief Base address of the mapped region.
    [[nodiscard]] auto base() const noexcept -> std::byte*;

    /// @brief Actual mapped capacity (page-aligned, >= requested).
    [[nodiscard]] auto capacity() const noexcept -> std::size_t;

    /// @brief System page size used for alignment.
    [[nodiscard]] static auto page_size() noexcept -> std::size_t;

private:
    Arena(std::byte* base, std::size_t capacity) noexcept;

    std::byte*  base_     = nullptr;
    std::size_t capacity_ = 0;
};

} // namespace mmap_viz
