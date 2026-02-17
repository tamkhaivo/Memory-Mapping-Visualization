#include "interface/visualization_arena.hpp"
#include <atomic>
#include <benchmark/benchmark.h>
#include <thread>
#include <vector>

using namespace mmap_viz;

// Multithreaded allocation benchmark to measure shard contention
static void BM_Contention_Allocation(benchmark::State &state) {
  static auto va =
      VisualizationArena::create({.arena_size = 256 * 1024 * 1024, // 256MB
                                  .enable_server = false,
                                  .sampling = 1})
          .value();

  for (auto _ : state) {
    void *p = va.alloc_raw(64, 8, "bench");
    if (p) {
      va.dealloc_raw(p, 64);
    } else {
      state.SkipWithError("OOM");
      break;
    }
  }
}

// Register for various thread counts
BENCHMARK(BM_Contention_Allocation)
    ->ThreadRange(1, std::thread::hardware_concurrency());

BENCHMARK_MAIN();
