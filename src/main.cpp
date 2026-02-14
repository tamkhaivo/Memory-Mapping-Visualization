/// @file main.cpp
/// @brief Demo program: creates the arena, allocator, tracker, and web server,
///        then runs an initial demo and accepts interactive stress test
///        commands from the browser frontend via WebSocket.

#include "allocator/arena.hpp"
#include "allocator/free_list.hpp"
#include "allocator/tracked_resource.hpp"
#include "serialization/json_serializer.hpp"
#include "server/ws_server.hpp"
#include "tracker/tracker.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory_resource>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace mmap_viz;
using namespace std::chrono_literals;

// ─── Shared live-allocation bookkeeping ─────────────────────────────────

struct LiveAlloc {
  void *ptr;
  std::size_t size;
};

static std::vector<LiveAlloc> g_live_allocs;
static std::mutex g_allocs_mutex;
static std::atomic<bool> g_stress_running{false};

static void sleep_ms(int ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// ─── Stress-test patterns ───────────────────────────────────────────────

/// Random burst: rapid random alloc/dealloc, 200 iterations.
static void stress_random_burst(TrackedResource &resource,
                                FreeListAllocator &allocator) {
  std::pmr::polymorphic_allocator<std::byte> alloc{&resource};
  std::mt19937 rng{static_cast<unsigned>(
      std::chrono::steady_clock::now().time_since_epoch().count())};
  std::uniform_int_distribution<std::size_t> size_dist{16, 2048};
  std::uniform_int_distribution<int> action{0, 2}; // 2/3 alloc, 1/3 dealloc

  std::cout << "\n[stress] random_burst: 200 iterations\n";

  for (int i = 0; i < 200 && g_stress_running.load(); ++i) {
    std::lock_guard lock(g_allocs_mutex);
    if (action(rng) > 0 || g_live_allocs.empty()) {
      auto sz = size_dist(rng);
      resource.set_next_tag("burst_" + std::to_string(i));
      try {
        void *p = alloc.allocate_bytes(sz, 16);
        g_live_allocs.push_back({p, sz});
      } catch (const std::bad_alloc &) { /* arena full */
      }
    } else {
      std::uniform_int_distribution<std::size_t> idx{0,
                                                     g_live_allocs.size() - 1};
      auto pick = idx(rng);
      auto &a = g_live_allocs[pick];
      alloc.deallocate_bytes(a.ptr, a.size, 16);
      g_live_allocs.erase(g_live_allocs.begin() +
                          static_cast<std::ptrdiff_t>(pick));
    }
    sleep_ms(30);
  }
  std::cout << "[stress] random_burst complete\n";
}

/// Fragmentation storm: fill arena with small blocks, then swiss-cheese it.
static void stress_frag_storm(TrackedResource &resource,
                              FreeListAllocator &allocator) {
  std::pmr::polymorphic_allocator<std::byte> alloc{&resource};
  std::cout << "\n[stress] frag_storm: filling arena then swiss-cheesing\n";

  // Phase A: fill with 128-byte blocks.
  int count = 0;
  while (g_stress_running.load()) {
    std::lock_guard lock(g_allocs_mutex);
    resource.set_next_tag("fill_" + std::to_string(count));
    try {
      void *p = alloc.allocate_bytes(128, 16);
      g_live_allocs.push_back({p, 128});
      ++count;
    } catch (const std::bad_alloc &) {
      break;
    }
    sleep_ms(15);
  }

  std::cout << "[stress] filled " << count << " blocks, now fragmenting\n";

  // Phase B: free every other block.
  {
    std::lock_guard lock(g_allocs_mutex);
    for (int i = static_cast<int>(g_live_allocs.size()) - 1;
         i >= 0 && g_stress_running.load(); i -= 2) {
      auto &a = g_live_allocs[i];
      alloc.deallocate_bytes(a.ptr, a.size, 16);
      a.ptr = nullptr;
      sleep_ms(10);
    }
    std::erase_if(g_live_allocs,
                  [](const auto &a) { return a.ptr == nullptr; });
  }

  std::cout << "[stress] frag_storm complete — " << g_live_allocs.size()
            << " blocks remain\n";
}

/// Large block stress: progressively larger allocations.
static void stress_large_blocks(TrackedResource &resource,
                                FreeListAllocator & /*allocator*/) {
  std::pmr::polymorphic_allocator<std::byte> alloc{&resource};
  std::cout << "\n[stress] large_blocks: exponential sizes\n";

  const std::size_t sizes[] = {256,  512,   1024,  2048,  4096,
                               8192, 16384, 32768, 65536, 131072};

  for (auto sz : sizes) {
    if (!g_stress_running.load())
      break;
    std::lock_guard lock(g_allocs_mutex);
    resource.set_next_tag("large_" + std::to_string(sz));
    try {
      void *p = alloc.allocate_bytes(sz, 16);
      g_live_allocs.push_back({p, sz});
      std::cout << "  [+] " << sz << "B\n";
    } catch (const std::bad_alloc &) {
      std::cout << "  [!] OOM at " << sz << "B\n";
    }
    sleep_ms(150);
  }
  std::cout << "[stress] large_blocks complete\n";
}

/// Cleanup: free all live allocations via the tracker-aware resource.
static void stress_cleanup(TrackedResource &resource,
                           FreeListAllocator & /*allocator*/) {
  std::pmr::polymorphic_allocator<std::byte> alloc{&resource};
  std::cout << "\n[stress] cleanup: freeing all " << g_live_allocs.size()
            << " blocks\n";
  std::lock_guard lock(g_allocs_mutex);
  for (auto &a : g_live_allocs) {
    alloc.deallocate_bytes(a.ptr, a.size, 16);
    sleep_ms(15);
  }
  g_live_allocs.clear();
  std::cout << "[stress] cleanup complete\n";
}

// ─── Command dispatcher ─────────────────────────────────────────────────

static void handle_command(const std::string &msg, TrackedResource &resource,
                           FreeListAllocator &allocator) {
  try {
    auto j = nlohmann::json::parse(msg);
    if (!j.contains("command"))
      return;

    auto cmd = j["command"].get<std::string>();

    if (cmd == "stress_test") {
      if (g_stress_running.load()) {
        std::cout << "[cmd] stress test already running, ignoring\n";
        return;
      }

      auto pattern = j.value("pattern", "random_burst");

      g_stress_running.store(true);

      std::thread([&resource, &allocator, pattern]() {
        if (pattern == "random_burst") {
          stress_random_burst(resource, allocator);
        } else if (pattern == "frag_storm") {
          stress_frag_storm(resource, allocator);
        } else if (pattern == "large_blocks") {
          stress_large_blocks(resource, allocator);
        } else {
          std::cout << "[cmd] unknown pattern: " << pattern << "\n";
        }
        g_stress_running.store(false);
      }).detach();

    } else if (cmd == "cleanup") {
      if (g_stress_running.load()) {
        std::cout << "[cmd] stopping running stress test first\n";
        g_stress_running.store(false);
        sleep_ms(200);
      }
      std::thread([&resource, &allocator]() {
        g_stress_running.store(true);
        stress_cleanup(resource, allocator);
        g_stress_running.store(false);
      }).detach();

    } else if (cmd == "stop") {
      g_stress_running.store(false);
      std::cout << "[cmd] stop requested\n";
    }
  } catch (const std::exception &e) {
    std::cerr << "[cmd] parse error: " << e.what() << "\n";
  }
}

// ─── Initial startup demo ───────────────────────────────────────────────

static void run_startup_demo(TrackedResource &resource,
                             FreeListAllocator & /*allocator*/) {
  std::pmr::polymorphic_allocator<std::byte> alloc{&resource};

  const std::size_t sizes[] = {64, 128, 256, 512, 1024, 2048, 4096};
  const char *tags[] = {"config",    "logger",    "thread_pool", "io_buffer",
                        "texture_a", "mesh_data", "audio_buf"};

  std::cout << "\n=== Startup demo: initial allocations ===\n";
  {
    std::lock_guard lock(g_allocs_mutex);
    for (int i = 0; i < 7; ++i) {
      resource.set_next_tag(tags[i]);
      void *p = alloc.allocate_bytes(sizes[i], 16);
      g_live_allocs.push_back({p, sizes[i]});
      std::cout << "  [+] " << sizes[i] << "B (" << tags[i] << ")\n";
      sleep_ms(300);
    }
  }

  std::cout << "\n=== Startup demo complete. Use the browser controls to "
               "run stress tests. ===\n\n";
}

// ─── main ───────────────────────────────────────────────────────────────

int main(int argc, char *argv[]) {
  constexpr std::size_t kArenaSize = 1024 * 1024; // 1 MB
  constexpr unsigned short kPort = 8080;

  auto exe_path = std::filesystem::path(argv[0]).parent_path();
  auto web_root = (exe_path / "web").string();

  if (!std::filesystem::exists(web_root)) {
    web_root = "web";
    if (!std::filesystem::exists(web_root)) {
      std::cerr << "Error: web/ directory not found.\n";
      return 1;
    }
  }

  std::cout << "=== Memory Mapping Visualization ===\n";
  std::cout << "Arena size:  " << kArenaSize << " bytes (" << kArenaSize / 1024
            << " KB)\n";
  std::cout << "Web root:    " << web_root << "\n";

  // ─── Create the pipeline ─────────────────────────────────────────────

  auto arena_result = Arena::create(kArenaSize);
  if (!arena_result.has_value()) {
    std::cerr << "Failed to create arena: " << arena_result.error().message()
              << "\n";
    return 1;
  }
  auto arena = std::move(*arena_result);

  FreeListAllocator allocator{arena};

  WsServer server{kPort, web_root, nullptr};

  AllocationTracker tracker{allocator, [&server](const AllocationEvent &event) {
                              nlohmann::json j = event;
                              server.broadcast(j.dump());
                            }};

  server.set_snapshot_provider([&tracker, &allocator]() -> std::string {
    auto blocks = tracker.snapshot();
    auto j = snapshot_to_json(blocks, allocator.bytes_allocated(),
                              allocator.bytes_free(), allocator.capacity(), 0,
                              allocator.free_block_count());
    return j.dump();
  });

  TrackedResource resource{allocator, tracker};

  // ─── Register command handler ────────────────────────────────────────

  server.set_command_handler([&resource, &allocator](const std::string &msg) {
    handle_command(msg, resource, allocator);
  });

  std::cout << "Page size:   " << Arena::page_size() << " bytes\n";
  std::cout << "Arena base:  " << static_cast<void *>(arena.base()) << "\n\n";

  // ─── Start server ────────────────────────────────────────────────────

  std::thread server_thread([&server]() { server.run(); });
  server_thread.detach();

  std::this_thread::sleep_for(200ms);

  std::cout << "Open http://localhost:" << kPort << " in your browser.\n";
  std::this_thread::sleep_for(1s);

  // ─── Run startup demo then wait for interactive commands ─────────────

  run_startup_demo(resource, allocator);

  // Keep server alive — interactive commands arrive via WebSocket.
  while (true) {
    std::this_thread::sleep_for(1s);
  }

  return 0;
}
