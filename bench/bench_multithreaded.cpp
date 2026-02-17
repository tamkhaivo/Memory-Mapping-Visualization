#include "interface/visualization_arena.hpp"
#include <benchmark/benchmark.h>
#include <thread>
#include <vector>

using namespace mmap_viz;

static void BM_MultiThreaded_Visualization(benchmark::State &state) {
  static auto va =
      VisualizationArena::create({.arena_size = 1024 * 1024 * 1024, // 1GB
                                  .enable_server = false,
                                  .sampling = 1})
          .value();

  for (auto _ : state) {
    void *ptr = va.alloc_raw(64, 8, "test");
    if (ptr) {
      va.dealloc_raw(ptr, 64);
    }
  }
}

BENCHMARK(BM_MultiThreaded_Visualization)
    ->Threads(1)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8);

BENCHMARK_MAIN();
