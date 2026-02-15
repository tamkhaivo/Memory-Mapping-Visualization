#pragma once
/// @file request_generator.hpp
/// @brief Configurable load generator for the simulated server.
///
/// Produces request streams with varying traffic patterns (steady, burst,
/// ramp, mixed) and records end-to-end metrics per request.

#include "simulation/metrics.hpp"
#include "simulation/server_sim.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace mmap_viz::sim {

/// @brief Traffic pattern for the load generator.
enum class TrafficPattern : std::uint8_t {
  Steady, ///< Constant request rate.
  Burst,  ///< High-intensity bursts with cooldowns.
  Ramp,   ///< Gradually increasing load.
  Mixed,  ///< Realistic mix of request types and pacing.
};

/// @brief Load generator configuration.
struct GeneratorConfig {
  TrafficPattern pattern = TrafficPattern::Mixed;
  std::size_t total_requests = 1000;

  // Steady pattern: interval between requests.
  std::chrono::microseconds steady_interval{100};

  // Burst pattern: requests per burst, cooldown between bursts.
  std::size_t burst_size = 50;
  std::chrono::milliseconds burst_cooldown{10};

  // Ramp pattern: starting and ending requests-per-second.
  std::size_t ramp_start_rps = 100;
  std::size_t ramp_end_rps = 5000;

  // Payload size range for generated requests.
  std::size_t min_payload = 32;
  std::size_t max_payload = 8192;
};

/// @brief Per-request callback (for live progress reporting).
/// Arguments: request_id, total_requests, was_successful.
using ProgressCallback = std::function<void(std::uint64_t, std::size_t, bool)>;

/// @brief Load generator that fires requests at a ServerSim.
class RequestGenerator {
public:
  explicit RequestGenerator(GeneratorConfig cfg = {});

  /// @brief Run the full traffic pattern against the server.
  /// @param server The target server simulation.
  /// @param on_progress Optional per-request callback.
  void run(ServerSim &server, ProgressCallback on_progress = nullptr);

  /// @brief Access the collected results.
  [[nodiscard]] auto results() const -> RequestMetrics;

private:
  void run_steady(ServerSim &server, ProgressCallback &cb);
  void run_burst(ServerSim &server, ProgressCallback &cb);
  void run_ramp(ServerSim &server, ProgressCallback &cb);
  void run_mixed(ServerSim &server, ProgressCallback &cb);

  /// @brief Generate a single random request.
  [[nodiscard]] auto make_request(std::uint64_t id) -> Request;

  GeneratorConfig cfg_;
  std::uint64_t next_id_ = 1;
};

} // namespace mmap_viz::sim
