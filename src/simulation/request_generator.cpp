/// @file request_generator.cpp
/// @brief Load generator implementation — produces configurable traffic
///        patterns and drives them through a ServerSim.

#include "simulation/request_generator.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <random>
#include <string>
#include <thread>

namespace mmap_viz::sim {

namespace {

auto &rng() {
  thread_local std::mt19937 gen{std::random_device{}()};
  return gen;
}

auto rand_between(std::size_t lo, std::size_t hi) -> std::size_t {
  std::uniform_int_distribution<std::size_t> dist(lo, hi);
  return dist(rng());
}

auto random_type() -> RequestType {
  // Weighted distribution: GET-heavy realistic traffic.
  //   GET=50%, POST=20%, PUT=15%, DELETE=10%, STREAM=5%.
  static constexpr std::array<RequestType, 20> pool = {
      RequestType::GET,    RequestType::GET,    RequestType::GET,
      RequestType::GET,    RequestType::GET,    RequestType::GET,
      RequestType::GET,    RequestType::GET,    RequestType::GET,
      RequestType::GET,    RequestType::POST,   RequestType::POST,
      RequestType::POST,   RequestType::POST,   RequestType::PUT,
      RequestType::PUT,    RequestType::PUT,    RequestType::DELETE,
      RequestType::DELETE, RequestType::STREAM,
  };
  return pool[rand_between(0, pool.size() - 1)];
}

auto random_endpoint() -> std::string {
  static constexpr std::array<const char *, 6> endpoints = {
      "/api/data",    "/api/users",    "/api/upload",
      "/api/metrics", "/api/sessions", "/api/stream",
  };
  return endpoints[rand_between(0, endpoints.size() - 1)];
}

} // namespace

// ─── RequestGenerator ───────────────────────────────────────────────────

RequestGenerator::RequestGenerator(GeneratorConfig cfg) : cfg_{cfg} {}

auto RequestGenerator::make_request(std::uint64_t id) -> Request {
  auto type = random_type();
  std::size_t payload = 0;

  switch (type) {
  case RequestType::GET:
    payload = rand_between(0, 64); // GETs have minimal payload.
    break;
  case RequestType::POST:
  case RequestType::PUT:
    payload = rand_between(cfg_.min_payload, cfg_.max_payload);
    break;
  case RequestType::DELETE:
    payload = rand_between(0, 32);
    break;
  case RequestType::STREAM:
    payload = rand_between(cfg_.min_payload, cfg_.max_payload / 2);
    break;
  }

  return Request{
      .id = id,
      .type = type,
      .payload_size = payload,
      .endpoint = random_endpoint(),
  };
}

// ─── Traffic pattern implementations ────────────────────────────────────

void RequestGenerator::run_steady(ServerSim &server, ProgressCallback &cb) {
  for (std::size_t i = 0; i < cfg_.total_requests; ++i) {
    auto req = make_request(next_id_++);
    auto resp = server.handle_request(req);
    if (cb)
      cb(req.id, cfg_.total_requests, resp.status == StatusCode::Ok);

    if (cfg_.steady_interval.count() > 0) {
      std::this_thread::sleep_for(cfg_.steady_interval);
    }
  }
}

void RequestGenerator::run_burst(ServerSim &server, ProgressCallback &cb) {
  std::size_t remaining = cfg_.total_requests;
  while (remaining > 0) {
    auto batch = std::min(cfg_.burst_size, remaining);

    // Fire burst.
    for (std::size_t i = 0; i < batch; ++i) {
      auto req = make_request(next_id_++);
      auto resp = server.handle_request(req);
      if (cb)
        cb(req.id, cfg_.total_requests, resp.status == StatusCode::Ok);
    }
    remaining -= batch;

    // Cooldown between bursts.
    if (remaining > 0) {
      std::this_thread::sleep_for(cfg_.burst_cooldown);
    }
  }
}

void RequestGenerator::run_ramp(ServerSim &server, ProgressCallback &cb) {
  // Linearly interpolate from ramp_start_rps to ramp_end_rps.
  std::size_t sent = 0;
  const auto total = cfg_.total_requests;
  const double start_rps = static_cast<double>(cfg_.ramp_start_rps);
  const double end_rps = static_cast<double>(cfg_.ramp_end_rps);

  while (sent < total) {
    double progress = static_cast<double>(sent) / static_cast<double>(total);
    double current_rps = start_rps + (end_rps - start_rps) * progress;

    // Send one request at the current rate.
    auto req = make_request(next_id_++);
    auto resp = server.handle_request(req);
    if (cb)
      cb(req.id, total, resp.status == StatusCode::Ok);
    ++sent;

    // Sleep for 1/current_rps seconds.
    if (current_rps > 0) {
      auto delay_us = static_cast<std::size_t>(1'000'000.0 / current_rps);
      std::this_thread::sleep_for(std::chrono::microseconds(delay_us));
    }
  }
}

void RequestGenerator::run_mixed(ServerSim &server, ProgressCallback &cb) {
  // Mixed pattern: alternate between bursts and steady segments.
  std::size_t remaining = cfg_.total_requests;
  std::size_t phase = 0;

  while (remaining > 0) {
    if (phase % 3 == 0) {
      // Burst phase: 20% of remaining or burst_size.
      auto batch = std::min({cfg_.burst_size, remaining,
                             std::max<std::size_t>(1, remaining / 5)});
      for (std::size_t i = 0; i < batch; ++i) {
        auto req = make_request(next_id_++);
        auto resp = server.handle_request(req);
        if (cb)
          cb(req.id, cfg_.total_requests, resp.status == StatusCode::Ok);
      }
      remaining -= batch;
      std::this_thread::sleep_for(cfg_.burst_cooldown);
    } else {
      // Steady phase: 10% of remaining with interval.
      auto batch =
          std::min(std::max<std::size_t>(1, remaining / 10), remaining);
      for (std::size_t i = 0; i < batch; ++i) {
        auto req = make_request(next_id_++);
        auto resp = server.handle_request(req);
        if (cb)
          cb(req.id, cfg_.total_requests, resp.status == StatusCode::Ok);
        std::this_thread::sleep_for(cfg_.steady_interval);
      }
      remaining -= batch;
    }
    ++phase;
  }
}

// ─── Public API ─────────────────────────────────────────────────────────

void RequestGenerator::run(ServerSim &server, ProgressCallback on_progress) {
  server.metrics().start();

  switch (cfg_.pattern) {
  case TrafficPattern::Steady:
    run_steady(server, on_progress);
    break;
  case TrafficPattern::Burst:
    run_burst(server, on_progress);
    break;
  case TrafficPattern::Ramp:
    run_ramp(server, on_progress);
    break;
  case TrafficPattern::Mixed:
    run_mixed(server, on_progress);
    break;
  }

  server.metrics().stop();
  server.cleanup_streams();
}

auto RequestGenerator::results() const -> RequestMetrics {
  // Results come from the server's metrics, not the generator.
  // The caller should use server.metrics().snapshot() directly.
  // This is a convenience that returns an empty snapshot.
  return {};
}

} // namespace mmap_viz::sim
