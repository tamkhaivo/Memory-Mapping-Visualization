#pragma once
/// @file server_sim.hpp
/// @brief Simulated high-bandwidth server backed by VisualizationArena.
///
/// Each request allocates from the arena (tagged by endpoint + type),
/// simulates processing, writes a response, and records metrics.

#include "interface/visualization_arena.hpp"
#include "simulation/metrics.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mmap_viz::sim {

// ─── Request / Response types ───────────────────────────────────────────

/// @brief Simulated HTTP-like request types.
enum class RequestType : std::uint8_t {
  GET,    ///< Read — small response buffer.
  POST,   ///< Write — variable payload.
  PUT,    ///< Update — variable payload.
  DELETE, ///< Remove — tiny metadata.
  STREAM, ///< Long-lived streaming buffer.
};

/// @brief Human-readable request type.
[[nodiscard]] constexpr auto to_string(RequestType t) -> const char * {
  switch (t) {
  case RequestType::GET:
    return "GET";
  case RequestType::POST:
    return "POST";
  case RequestType::PUT:
    return "PUT";
  case RequestType::DELETE:
    return "DELETE";
  case RequestType::STREAM:
    return "STREAM";
  }
  return "UNKNOWN";
}

/// @brief Response status codes.
enum class StatusCode : std::uint16_t {
  Ok = 200,
  NotFound = 404,
  ServerError = 500,
  OutOfMemory = 503,
};

/// @brief A simulated inbound request.
struct Request {
  std::uint64_t id;         ///< Monotonic request ID.
  RequestType type;         ///< GET/POST/PUT/DELETE/STREAM.
  std::size_t payload_size; ///< Inbound payload bytes.
  std::string endpoint;     ///< e.g. "/api/data", "/api/upload".
};

/// @brief A simulated server response.
struct Response {
  std::uint64_t request_id; ///< ID of the originating request.
  StatusCode status;        ///< Response status.
  std::size_t body_size;    ///< Outbound response body bytes.
};

// ─── Server ─────────────────────────────────────────────────────────────

/// @brief Simulated server configuration.
struct ServerConfig {
  /// Simulate processing latency (microseconds) per request type.
  /// If 0, no artificial delay is added.
  std::size_t base_latency_us = 0;
};

/// @brief Simulated request/response server backed by VisualizationArena.
///
/// All request and response buffers are allocated from the arena,
/// making every allocation visible to the visualization frontend.
/// Response buffers are freed after each request completes (short-lived),
/// except STREAM responses which are accumulated and freed on cleanup().
class ServerSim {
public:
  /// @brief Construct a server simulation.
  /// @param arena  Reference to the backing VisualizationArena.
  /// @param cfg    Server configuration.
  explicit ServerSim(VisualizationArena &arena, ServerConfig cfg = {}) noexcept;

  /// @brief Process a single request.
  /// @param req The inbound request.
  /// @return The response (body already freed for non-STREAM types).
  auto handle_request(const Request &req) -> Response;

  /// @brief Free all outstanding STREAM buffers.
  void cleanup_streams();

  /// @brief Access the metrics collector.
  [[nodiscard]] auto metrics() const -> const MetricsCollector &;
  [[nodiscard]] auto metrics() -> MetricsCollector &;

private:
  /// @brief Determine response size based on request type.
  [[nodiscard]] auto response_size_for(const Request &req) const -> std::size_t;

  VisualizationArena &arena_;
  ServerConfig cfg_;
  MetricsCollector metrics_;

  /// Outstanding STREAM allocations (ptr → size).
  std::vector<std::pair<void *, std::size_t>> stream_buffers_;
};

} // namespace mmap_viz::sim
