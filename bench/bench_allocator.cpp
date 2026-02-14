/// @file bench_allocator.cpp
/// @brief Micro-benchmarks for the free-list allocator hot paths.

#include "allocator/arena.hpp"
#include "allocator/free_list.hpp"

#include <benchmark/benchmark.h>
#include <vector>

using namespace mmap_viz;

static void BM_Allocate64B(benchmark::State &state) {
  auto arena = Arena::create(1024 * 1024).value();
  FreeListAllocator alloc{arena};

  for (auto _ : state) {
    auto r = alloc.allocate(64, 16);
    benchmark::DoNotOptimize(r);
    if (!r.has_value()) {
      state.SkipWithError("OOM");
      break;
    }
  }
}
BENCHMARK(BM_Allocate64B);

static void BM_AllocateDealloc64B(benchmark::State &state) {
  auto arena = Arena::create(1024 * 1024).value();
  FreeListAllocator alloc{arena};

  for (auto _ : state) {
    auto r = alloc.allocate(64, 16);
    benchmark::DoNotOptimize(r);
    if (r.has_value()) {
      alloc.deallocate(r->ptr, 64);
    }
  }
}
BENCHMARK(BM_AllocateDealloc64B);

static void BM_AllocateVarySizes(benchmark::State &state) {
  auto arena = Arena::create(4 * 1024 * 1024).value();
  FreeListAllocator alloc{arena};

  const std::size_t sizes[] = {32, 64, 128, 256, 512, 1024, 2048, 4096};
  std::size_t idx = 0;

  for (auto _ : state) {
    auto r = alloc.allocate(sizes[idx % 8], 16);
    benchmark::DoNotOptimize(r);
    if (!r.has_value()) {
      state.SkipWithError("OOM");
      break;
    }
    ++idx;
  }
}
BENCHMARK(BM_AllocateVarySizes);

static void BM_FragmentedAllocDealloc(benchmark::State &state) {
  auto arena = Arena::create(1024 * 1024).value();
  FreeListAllocator alloc{arena};

  // Pre-fragment: allocate 100 blocks, free every other.
  std::vector<AllocationResult> blocks;
  for (int i = 0; i < 100; ++i) {
    auto r = alloc.allocate(256, 16);
    if (r.has_value())
      blocks.push_back(*r);
  }
  for (std::size_t i = 0; i < blocks.size(); i += 2) {
    alloc.deallocate(blocks[i].ptr, 256);
  }

  for (auto _ : state) {
    auto r = alloc.allocate(128, 16);
    benchmark::DoNotOptimize(r);
    if (r.has_value()) {
      alloc.deallocate(r->ptr, 128);
    }
  }
}
BENCHMARK(BM_FragmentedAllocDealloc);
