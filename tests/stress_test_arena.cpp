#include "interface/visualization_arena.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <random>
#include <thread>
#include <vector>

using namespace mmap_viz;

void run_stress_client(VisualizationArena &va, std::atomic<bool> &running,
                       int id) {
  std::mt19937 gen(id +
                   std::chrono::steady_clock::now().time_since_epoch().count());
  std::uniform_int_distribution<std::size_t> size_dist(4, 4096);
  std::uniform_int_distribution<std::size_t> align_dist(
      0, 5); // 1, 2, 4, 8, 16, 32

  struct Alloc {
    void *ptr;
    std::size_t size;
  };
  std::vector<Alloc> allocs;
  allocs.reserve(1000);

  while (running) {
    // Randomly allocate or deallocate
    if (allocs.empty() ||
        (allocs.size() < 500 &&
         std::uniform_int_distribution<int>(0, 1)(gen) == 0)) {
      std::size_t sz = size_dist(gen);
      std::size_t al = 1 << align_dist(gen);
      void *p = va.alloc_raw(sz, al, "stress");
      if (p) {
        allocs.push_back({p, sz});
      }
    } else {
      std::size_t idx =
          std::uniform_int_distribution<std::size_t>(0, allocs.size() - 1)(gen);
      va.dealloc_raw(allocs[idx].ptr, allocs[idx].size);
      allocs[idx] = allocs.back();
      allocs.pop_back();
    }

    // Small yield to prevent 100% CPU lock in tight loop
    if (std::uniform_int_distribution<int>(0, 100)(gen) == 0) {
      std::this_thread::yield();
    }
  }

  // Cleanup remaining
  for (const auto &a : allocs) {
    va.dealloc_raw(a.ptr, a.size);
  }
}

int main(int argc, char **argv) {
  int duration_sec = 10;
  int num_threads = std::thread::hardware_concurrency();

  if (argc > 1)
    duration_sec = std::atoi(argv[1]);
  if (argc > 2)
    num_threads = std::atoi(argv[2]);

  std::cout << "Starting Stress Test: " << num_threads << " threads for "
            << duration_sec << "s" << std::endl;

  auto va =
      VisualizationArena::create({.arena_size = 512 * 1024 * 1024, // 512MB
                                  .enable_server = false,
                                  .sampling = 100})
          .value();

  std::atomic<bool> running{true};
  std::vector<std::thread> threads;
  for (int i = 0; i < num_threads; ++i) {
    threads.emplace_back(run_stress_client, std::ref(va), std::ref(running), i);
  }

  std::this_thread::sleep_for(std::chrono::seconds(duration_sec));
  running = false;

  for (auto &t : threads) {
    t.join();
  }

  std::cout << "Stress Test Completed Successfully" << std::endl;
  std::cout << "Bytes Allocated: " << va.bytes_allocated() << std::endl;
  std::cout << "Bytes Free:      " << va.bytes_free() << std::endl;

  return 0;
}
