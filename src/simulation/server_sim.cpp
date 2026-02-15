/// @file server_sim.cpp
/// @brief Implementation of the simulated request/response server.

#include "simulation/server_sim.hpp"

#include <chrono>
#include <cstring>
#include <random>
#include <string>
#include <thread>

namespace mmap_viz::sim {

// ─── Helpers ────────────────────────────────────────────────────────────

namespace {

/// Thread-local RNG for jittering response sizes.
auto &rng() {
  thread_local std::mt19937 gen{std::random_device{}()};
  return gen;
}

/// Random value in [lo, hi].
auto rand_between(std::size_t lo, std::size_t hi) -> std::size_t {
  std::uniform_int_distribution<std::size_t> dist(lo, hi);
  return dist(rng());
}

} // namespace

// ─── ServerSim ──────────────────────────────────────────────────────────

ServerSim::ServerSim(VisualizationArena &arena, ServerConfig cfg) noexcept
    : arena_{arena}, cfg_{cfg} {}

auto ServerSim::response_size_for(const Request &req) const -> std::size_t {
  switch (req.type) {
  case RequestType::GET:
    return rand_between(64, 512);
  case RequestType::POST:
    // Response acknowledges the write — echo size + small header.
    return rand_between(32, 256);
  case RequestType::PUT:
    return rand_between(32, 256);
  case RequestType::DELETE:
    return rand_between(16, 64);
  case RequestType::STREAM:
    return rand_between(4096, 65536);
  }
  return 128;
}

auto ServerSim::handle_request(const Request &req) -> Response {
  using Clock = std::chrono::steady_clock;
  const auto t0 = Clock::now();

  // Build arena tag: "GET /api/data #42"
  std::string tag = std::string{to_string(req.type)} + " " + req.endpoint +
                    " #" + std::to_string(req.id);

  // 1. Allocate request buffer (simulates receiving payload).
  void *req_buf = nullptr;
  if (req.payload_size > 0) {
    req_buf = arena_.alloc_raw(req.payload_size, 16, tag + " [req]");
    if (req_buf == nullptr) {
      // Arena OOM — record failure.
      const auto latency = Clock::now() - t0;
      metrics_.record(latency, req.payload_size, 0, false);
      return Response{
          .request_id = req.id,
          .status = StatusCode::OutOfMemory,
          .body_size = 0,
      };
    }
    // Simulate writing payload into the buffer.
    std::memset(req_buf, 0xAA, req.payload_size);
  }

  // 2. Allocate response buffer.
  auto resp_size = response_size_for(req);
  void *resp_buf = arena_.alloc_raw(resp_size, 16, tag + " [resp]");
  if (resp_buf == nullptr) {
    // Free request buffer if allocated, then fail.
    if (req_buf != nullptr) {
      arena_.dealloc_raw(req_buf, req.payload_size);
    }
    const auto latency = Clock::now() - t0;
    metrics_.record(latency, req.payload_size, 0, false);
    return Response{
        .request_id = req.id,
        .status = StatusCode::OutOfMemory,
        .body_size = 0,
    };
  }

  // 3. Simulate processing — write response data.
  std::memset(resp_buf, 0xBB, resp_size);

  // 4. Simulate base processing latency if configured.
  if (cfg_.base_latency_us > 0) {
    std::this_thread::sleep_for(
        std::chrono::microseconds(cfg_.base_latency_us));
  }

  // 5. Free request buffer (we've "consumed" the payload).
  if (req_buf != nullptr) {
    arena_.dealloc_raw(req_buf, req.payload_size);
  }

  // 6. For STREAM requests, keep the response buffer alive.
  //    For all others, free it immediately (short-lived).
  if (req.type == RequestType::STREAM) {
    stream_buffers_.emplace_back(resp_buf, resp_size);
  } else {
    arena_.dealloc_raw(resp_buf, resp_size);
  }

  // 7. Record metrics.
  const auto latency = Clock::now() - t0;
  metrics_.record(latency, req.payload_size, resp_size, true);

  return Response{
      .request_id = req.id,
      .status = StatusCode::Ok,
      .body_size = resp_size,
  };
}

void ServerSim::cleanup_streams() {
  for (auto &[ptr, size] : stream_buffers_) {
    arena_.dealloc_raw(ptr, size);
  }
  stream_buffers_.clear();
}

auto ServerSim::metrics() const -> const MetricsCollector & { return metrics_; }

auto ServerSim::metrics() -> MetricsCollector & { return metrics_; }

} // namespace mmap_viz::sim
