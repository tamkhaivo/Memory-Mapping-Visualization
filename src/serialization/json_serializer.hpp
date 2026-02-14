#pragma once
/// @file json_serializer.hpp
/// @brief nlohmann/json serialization for BlockMetadata and AllocationEvent.

#include "tracker/block_metadata.hpp"

#include <nlohmann/json.hpp>

namespace mmap_viz {

inline void to_json(nlohmann::json &j, const BlockMetadata &b) {
  j = nlohmann::json{
      {"offset", b.offset},
      {"size", b.size},
      {"alignment", b.alignment},
      {"actual_size", b.actual_size},
      {"tag", b.tag},
      {"timestamp_us", std::chrono::duration_cast<std::chrono::microseconds>(
                           b.timestamp.time_since_epoch())
                           .count()},
  };
}

inline void to_json(nlohmann::json &j, const AllocationEvent &e) {
  j = nlohmann::json{
      {"type", e.type == EventType::Allocate ? "allocate" : "deallocate"},
      {"event_id", e.event_id},
      {"offset", e.block.offset},
      {"size", e.block.size},
      {"alignment", e.block.alignment},
      {"actual_size", e.block.actual_size},
      {"tag", e.block.tag},
      {"timestamp_us", std::chrono::duration_cast<std::chrono::microseconds>(
                           e.block.timestamp.time_since_epoch())
                           .count()},
      {"total_allocated", e.total_allocated},
      {"total_free", e.total_free},
      {"fragmentation_pct", e.fragmentation_pct},
      {"free_block_count", e.free_block_count},
  };
}

/// @brief Serialize a full snapshot (vector of active blocks) for initial
/// client sync.
inline auto snapshot_to_json(const std::vector<BlockMetadata> &blocks,
                             std::size_t total_allocated,
                             std::size_t total_free, std::size_t capacity,
                             std::size_t fragmentation_pct,
                             std::size_t free_block_count) -> nlohmann::json {
  nlohmann::json j;
  j["type"] = "snapshot";
  j["capacity"] = capacity;
  j["total_allocated"] = total_allocated;
  j["total_free"] = total_free;
  j["fragmentation_pct"] = fragmentation_pct;
  j["free_block_count"] = free_block_count;
  j["blocks"] = nlohmann::json::array();

  for (const auto &block : blocks) {
    j["blocks"].push_back(nlohmann::json(block));
  }

  return j;
}

} // namespace mmap_viz
