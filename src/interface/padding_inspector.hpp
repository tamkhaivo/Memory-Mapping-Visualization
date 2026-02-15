#pragma once
/// @file padding_inspector.hpp
/// @brief Compile-time struct layout inspector and runtime padding reporter.
///
/// Provides two complementary analysis tools:
/// 1. Runtime PaddingReport — aggregates wasted bytes across live allocations
///    from the allocator's actual_size vs requested size delta.
/// 2. Compile-time LayoutInfo via MMAP_VIZ_INSPECT() — enumerates struct
/// fields,
///    computes per-field padding gaps, tail padding, and overall efficiency
///    without requiring C++ reflection.

#include "tracker/block_metadata.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace mmap_viz {

// ─── Runtime padding analysis (allocator-level) ─────────────────────────

/// @brief Padding detail for a single allocated block.
struct BlockPaddingInfo {
  std::size_t offset;         ///< Offset from arena base.
  std::size_t requested_size; ///< User-requested size.
  std::size_t actual_size;    ///< Allocator-assigned size (includes padding).
  std::size_t alignment;      ///< Requested alignment.
  std::size_t padding_bytes;  ///< actual_size - requested_size.
  float efficiency;           ///< requested / actual (0.0–1.0).
  std::string tag;            ///< Block tag for identification.
};

/// @brief Aggregate padding waste report across all live allocations.
struct PaddingReport {
  std::size_t total_requested; ///< Sum of all requested sizes.
  std::size_t total_actual;    ///< Sum of all actual sizes.
  std::size_t total_wasted;    ///< total_actual - total_requested.
  float efficiency;            ///< total_requested / total_actual (0.0–1.0).
  std::vector<BlockPaddingInfo> blocks; ///< Per-block detail.
};

/// @brief Generate a padding waste report from the current block snapshot.
/// @param blocks Snapshot of active allocations.
/// @return PaddingReport with per-block and aggregate metrics.
[[nodiscard]] inline auto
compute_padding_report(std::span<const BlockMetadata> blocks) -> PaddingReport {
  PaddingReport report{};

  for (const auto &block : blocks) {
    const auto wasted = (block.actual_size > block.size)
                            ? (block.actual_size - block.size)
                            : std::size_t{0};

    const auto eff = (block.actual_size > 0)
                         ? static_cast<float>(block.size) /
                               static_cast<float>(block.actual_size)
                         : 0.0f;

    report.blocks.push_back(BlockPaddingInfo{
        .offset = block.offset,
        .requested_size = block.size,
        .actual_size = block.actual_size,
        .alignment = block.alignment,
        .padding_bytes = wasted,
        .efficiency = eff,
        .tag = block.tag,
    });

    report.total_requested += block.size;
    report.total_actual += block.actual_size;
  }

  report.total_wasted = (report.total_actual > report.total_requested)
                            ? (report.total_actual - report.total_requested)
                            : 0;

  report.efficiency = (report.total_actual > 0)
                          ? static_cast<float>(report.total_requested) /
                                static_cast<float>(report.total_actual)
                          : 0.0f;

  return report;
}

// ─── Compile-time struct layout analysis ────────────────────────────────

/// @brief Describes a single field within a struct.
struct FieldInfo {
  const char *name;           ///< Field name (string literal from macro).
  std::size_t offset;         ///< Byte offset within the struct.
  std::size_t size;           ///< sizeof(field).
  std::size_t alignment;      ///< alignof(field).
  std::size_t padding_before; ///< Gap from previous field end to this offset.
};

/// @brief Complete layout description for a struct type.
struct LayoutInfo {
  const char *type_name;  ///< Struct type name (string literal from macro).
  std::size_t total_size; ///< sizeof(T).
  std::size_t total_alignment; ///< alignof(T).
  std::size_t useful_bytes;    ///< Sum of all field sizes.
  std::size_t padding_bytes;   ///< total_size - useful_bytes.
  std::size_t tail_padding;    ///< Padding after the last field.
  float efficiency;            ///< useful_bytes / total_size (0.0–1.0).
  std::vector<FieldInfo> fields;
};

namespace detail {

/// @brief Build a LayoutInfo from a list of FieldInfo entries.
/// @param type_name  Name of the type being inspected.
/// @param total_size sizeof(T).
/// @param total_align alignof(T).
/// @param fields     Vector of fields (offset, size filled in by macro).
inline auto build_layout(const char *type_name, std::size_t total_size,
                         std::size_t total_align, std::vector<FieldInfo> fields)
    -> LayoutInfo {
  std::size_t useful = 0;
  std::size_t prev_end = 0;

  for (auto &f : fields) {
    f.padding_before = (f.offset >= prev_end) ? (f.offset - prev_end) : 0;
    useful += f.size;
    prev_end = f.offset + f.size;
  }

  const auto tail = (total_size >= prev_end) ? (total_size - prev_end) : 0;
  const auto pad = total_size - useful;
  const auto eff = (total_size > 0) ? static_cast<float>(useful) /
                                          static_cast<float>(total_size)
                                    : 0.0f;

  return LayoutInfo{
      .type_name = type_name,
      .total_size = total_size,
      .total_alignment = total_align,
      .useful_bytes = useful,
      .padding_bytes = pad,
      .tail_padding = tail,
      .efficiency = eff,
      .fields = std::move(fields),
  };
}

} // namespace detail
} // namespace mmap_viz

// ─── Registration macro ────────────────────────────────────────────────
//
// Usage:
//   struct MyStruct {
//     int a;
//     double b;
//     char c;
//   };
//   auto info = MMAP_VIZ_INSPECT(MyStruct, a, b, c);
//
// Produces a mmap_viz::LayoutInfo with field offsets, sizes, padding gaps,
// and tail padding computed at compile time via offsetof().

/// @brief Helper that expands a single field into a FieldInfo initializer.
#define MMAP_VIZ_FIELD_(Type, Field)                                           \
  ::mmap_viz::FieldInfo{#Field, offsetof(Type, Field),                         \
                        sizeof(std::declval<Type>().Field),                    \
                        alignof(decltype(std::declval<Type>().Field)), 0}

/// @brief Inspect the memory layout of a struct type.
/// @param Type  The struct/class type.
/// @param ...   Comma-separated list of field names.
/// @return mmap_viz::LayoutInfo describing the layout.
#define MMAP_VIZ_INSPECT(Type, ...)                                            \
  ::mmap_viz::detail::build_layout(                                            \
      #Type, sizeof(Type), alignof(Type),                                      \
      std::vector<::mmap_viz::FieldInfo>{                                      \
          MMAP_VIZ_FIELD_EXPAND_(Type, __VA_ARGS__)})

// ─── Variadic expansion helpers ─────────────────────────────────────────

#define MMAP_VIZ_FIELD_EXPAND_(Type, ...)                                      \
  MMAP_VIZ_MAP_(MMAP_VIZ_FIELD_, Type, __VA_ARGS__)

#define MMAP_VIZ_MAP_(Macro, Type, ...)                                        \
  MMAP_VIZ_MAP_IMPL_(Macro, Type, __VA_ARGS__)

// Support up to 16 fields — sufficient for most struct inspections.
#define MMAP_VIZ_MAP_IMPL_(Macro, Type, ...)                                   \
  MMAP_VIZ_SELECT_(__VA_ARGS__, MMAP_VIZ_MAP_16_, MMAP_VIZ_MAP_15_,            \
                   MMAP_VIZ_MAP_14_, MMAP_VIZ_MAP_13_, MMAP_VIZ_MAP_12_,       \
                   MMAP_VIZ_MAP_11_, MMAP_VIZ_MAP_10_, MMAP_VIZ_MAP_9_,        \
                   MMAP_VIZ_MAP_8_, MMAP_VIZ_MAP_7_, MMAP_VIZ_MAP_6_,          \
                   MMAP_VIZ_MAP_5_, MMAP_VIZ_MAP_4_, MMAP_VIZ_MAP_3_,          \
                   MMAP_VIZ_MAP_2_, MMAP_VIZ_MAP_1_)(Macro, Type, __VA_ARGS__)

#define MMAP_VIZ_SELECT_(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12,    \
                         _13, _14, _15, _16, N, ...)                           \
  N

#define MMAP_VIZ_MAP_1_(M, T, a) M(T, a)
#define MMAP_VIZ_MAP_2_(M, T, a, ...)                                          \
  M(T, a), MMAP_VIZ_MAP_1_(M, T, __VA_ARGS__)
#define MMAP_VIZ_MAP_3_(M, T, a, ...)                                          \
  M(T, a), MMAP_VIZ_MAP_2_(M, T, __VA_ARGS__)
#define MMAP_VIZ_MAP_4_(M, T, a, ...)                                          \
  M(T, a), MMAP_VIZ_MAP_3_(M, T, __VA_ARGS__)
#define MMAP_VIZ_MAP_5_(M, T, a, ...)                                          \
  M(T, a), MMAP_VIZ_MAP_4_(M, T, __VA_ARGS__)
#define MMAP_VIZ_MAP_6_(M, T, a, ...)                                          \
  M(T, a), MMAP_VIZ_MAP_5_(M, T, __VA_ARGS__)
#define MMAP_VIZ_MAP_7_(M, T, a, ...)                                          \
  M(T, a), MMAP_VIZ_MAP_6_(M, T, __VA_ARGS__)
#define MMAP_VIZ_MAP_8_(M, T, a, ...)                                          \
  M(T, a), MMAP_VIZ_MAP_7_(M, T, __VA_ARGS__)
#define MMAP_VIZ_MAP_9_(M, T, a, ...)                                          \
  M(T, a), MMAP_VIZ_MAP_8_(M, T, __VA_ARGS__)
#define MMAP_VIZ_MAP_10_(M, T, a, ...)                                         \
  M(T, a), MMAP_VIZ_MAP_9_(M, T, __VA_ARGS__)
#define MMAP_VIZ_MAP_11_(M, T, a, ...)                                         \
  M(T, a), MMAP_VIZ_MAP_10_(M, T, __VA_ARGS__)
#define MMAP_VIZ_MAP_12_(M, T, a, ...)                                         \
  M(T, a), MMAP_VIZ_MAP_11_(M, T, __VA_ARGS__)
#define MMAP_VIZ_MAP_13_(M, T, a, ...)                                         \
  M(T, a), MMAP_VIZ_MAP_12_(M, T, __VA_ARGS__)
#define MMAP_VIZ_MAP_14_(M, T, a, ...)                                         \
  M(T, a), MMAP_VIZ_MAP_13_(M, T, __VA_ARGS__)
#define MMAP_VIZ_MAP_15_(M, T, a, ...)                                         \
  M(T, a), MMAP_VIZ_MAP_14_(M, T, __VA_ARGS__)
#define MMAP_VIZ_MAP_16_(M, T, a, ...)                                         \
  M(T, a), MMAP_VIZ_MAP_15_(M, T, __VA_ARGS__)
