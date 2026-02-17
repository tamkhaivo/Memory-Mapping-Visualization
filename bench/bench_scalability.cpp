/// @file bench_scalability.cpp
/// @brief Benchmark to verify O(log N) behavior of FreeListAllocator.

#include "allocator/arena.hpp"
#include "allocator/free_list.hpp"

#include <benchmark/benchmark.h>
#include <random>
#include <vector>

using namespace mmap_viz;

// Benchmark allocation time with N free blocks
static void BM_Scalability(benchmark::State &state) {
  std::size_t num_blocks = state.range(0);
  // Determine arena size needed
  // We want 'num_blocks' free blocks.
  // To achieve this, we can allocate 2*num_blocks items, then free every second
  // one. Block size = 64. Total size = 2 * num_blocks * 64 + overhead. Safe
  // margin: 256 bytes per block.

  std::size_t arena_size = num_blocks * 512 + 1024 * 1024;
  auto result = Arena::create(arena_size);
  if (!result.has_value()) {
    state.SkipWithError("Failed to create arena (too large?)");
    return;
  }
  Arena &arena =
      result.value(); // Keep it alive? No, result.value() is temporary?
  // Arena is a class wrapping a span/pointer?
  // Arena::create returns std::expected<Arena, ...>
  // We need to keep the arena object alive.
  Arena my_arena = std::move(result.value());

  FreeListAllocator alloc{my_arena.base(), my_arena.capacity()};

  // Setup fragmentation
  std::vector<void *> keep_ptrs;
  keep_ptrs.reserve(num_blocks);

  std::vector<void *> free_soon_ptrs;
  free_soon_ptrs.reserve(num_blocks);

  // Allocate 2*N blocks
  for (std::size_t i = 0; i < num_blocks; ++i) {
    auto r1 = alloc.allocate(64, 16);
    auto r2 = alloc.allocate(64, 16);
    if (!r1 || !r2) {
      state.SkipWithError("Setup OOM");
      return;
    }
    free_soon_ptrs.push_back(r1->ptr);
    keep_ptrs.push_back(r2->ptr);
  }

  // Free the 'free_soon' ones to create holes
  // Each free creates a node in the RB tree (or linked list).
  // None should coalesce because 'keep_ptrs' are in between.
  for (void *ptr : free_soon_ptrs) {
    alloc.deallocate(static_cast<std::byte *>(ptr), 64);
  }

  // Now we have ~num_blocks free blocks in the structure.
  // Measure time to allocate one more block (filling a hole).
  // And then free it back to restore state?
  // Or just allocate until full?
  // If we allocate and don't free, N decreases.
  // If we alloc and free immediately, we measure both.

  for (auto _ : state) {
    auto r = alloc.allocate(64, 16);
    benchmark::DoNotOptimize(r);
    if (r.has_value()) {
      alloc.deallocate(r->ptr, 64);
    } else {
      state.SkipWithError("Benchmark OOM");
      break;
    }
  }
}

// Range: 100 to 100,000 free blocks
BENCHMARK(BM_Scalability)->RangeMultiplier(10)->Range(100, 10000);

BENCHMARK_MAIN();
