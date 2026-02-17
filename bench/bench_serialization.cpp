#include "serialization/json_serializer.hpp"
#include "tracker/block_metadata.hpp"
#include <benchmark/benchmark.h>
#include <nlohmann/json.hpp>
#include <vector>

using namespace mmap_viz;

static void BM_Serialization_SingleEvent(benchmark::State &state) {
  AllocationEvent event;
  event.type = EventType::Allocate;
  event.event_id = 12345;
  event.block.offset = 1024;
  event.block.size = 64;
  event.block.alignment = 16;
  event.block.actual_size = 96;
  event.block.set_tag("test_tag");
  event.block.timestamp = std::chrono::steady_clock::now();
  event.total_allocated = 1024 * 1024;
  event.total_free = 1024 * 1024;
  event.fragmentation_pct = 5;
  event.free_block_count = 100;

  for (auto _ : state) {
    nlohmann::json j = event;
    std::string s = j.dump();
    benchmark::DoNotOptimize(s);
  }
}

static void BM_Serialization_Batch(benchmark::State &state) {
  size_t batch_size = state.range(0);
  std::vector<AllocationEvent> events;
  for (size_t i = 0; i < batch_size; ++i) {
    AllocationEvent event;
    event.type = EventType::Allocate;
    event.event_id = i;
    event.block.offset = i * 128;
    event.block.size = 64;
    event.block.set_tag("bench");
    events.push_back(event);
  }

  for (auto _ : state) {
    std::stringstream ss;
    ss << "[";
    for (size_t i = 0; i < events.size(); ++i) {
      nlohmann::json j = events[i];
      ss << j.dump();
      if (i < events.size() - 1) {
        ss << ",";
      }
    }
    ss << "]";
    std::string s = ss.str();
    benchmark::DoNotOptimize(s);
  }
}

BENCHMARK(BM_Serialization_SingleEvent);
BENCHMARK(BM_Serialization_Batch)->Arg(10)->Arg(100)->Arg(1000);

BENCHMARK_MAIN();
