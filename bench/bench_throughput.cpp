#include "allocator/free_list.hpp"
#include "interface/visualization_arena.hpp"
#include <benchmark/benchmark.h>
#include <random>
#include <vector>

using namespace mmap_viz;

// Baseline: Just the allocator (no tracking, no visualization)
static void BM_AllocatorOnly(benchmark::State &state) {
  auto arena = Arena::create(64 * 1024 * 1024).value(); // 64MB
  FreeListAllocator allocator(arena.base(), arena.capacity());

  std::vector<void *> ptrs;
  ptrs.reserve(10000);

  for (auto _ : state) {
    // pattern: 100 allocs, 100 frees
    for (int i = 0; i < 100; ++i) {
      auto res = allocator.allocate(64);
      if (res)
        ptrs.push_back(res->ptr);
    }

    state.PauseTiming();
    for (void *p : ptrs) {
      allocator.deallocate(static_cast<std::byte *>(p), 64);
    }
    ptrs.clear();
    state.ResumeTiming();
  }
}

// Full Stack: VisualizationArena with Serialization overhead (but no network if
// server not enabled)
static void BM_VisualizationArena_NoServer(benchmark::State &state) {
  auto va = VisualizationArena::create({.arena_size = 64 * 1024 * 1024,
                                        .enable_server = false,
                                        .sampling = 1})
                .value();

  std::vector<void *> ptrs;
  ptrs.reserve(10000);

  for (auto _ : state) {
    for (int i = 0; i < 100; ++i) {
      void *p = va.alloc_raw(64, 8, "test");
      if (p)
        ptrs.push_back(p);
    }

    state.PauseTiming();
    for (void *p : ptrs) {
      va.dealloc_raw(p, 64);
    }
    ptrs.clear();
    state.ResumeTiming();
  }
}

// Full Stack with Sampling
static void BM_VisualizationArena_Sampled(benchmark::State &state) {
  auto va = VisualizationArena::create({
                                           .arena_size = 64 * 1024 * 1024,
                                           .enable_server = false,
                                           .sampling = 1000 // Sample 1 in 1000
                                       })
                .value();

  std::vector<void *> ptrs;
  ptrs.reserve(10000);

  for (auto _ : state) {
    for (int i = 0; i < 100; ++i) {
      void *p = va.alloc_raw(64, 8, "test");
      if (p)
        ptrs.push_back(p);
    }

    state.PauseTiming();
    for (void *p : ptrs) {
      va.dealloc_raw(p, 64);
    }
    ptrs.clear();
    state.ResumeTiming();
  }
}

BENCHMARK(BM_AllocatorOnly);
BENCHMARK(BM_VisualizationArena_NoServer);
BENCHMARK(BM_VisualizationArena_Sampled);

BENCHMARK_MAIN();
