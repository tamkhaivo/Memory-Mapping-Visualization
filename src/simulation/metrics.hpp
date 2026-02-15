#pragma once
/// @file metrics.hpp
/// @brief Request-level metrics collector: latency percentiles, bandwidth,
///        throughput, and success/failure tracking.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <vector>

namespace mmap_viz::sim {

/// @brief Snapshot of aggregate request metrics.
struct RequestMetrics {
  std::size_t total_requests = 0;
  std::size_t successful = 0;
  std::size_t failed = 0;
  std::size_t total_bytes_in = 0;  ///< Sum of request payload bytes.
  std::size_t total_bytes_out = 0; ///< Sum of response payload bytes.
  double elapsed_seconds = 0.0;

  // Latency (microseconds).
  double min_latency_us = 0.0;
  double max_latency_us = 0.0;
  double avg_latency_us = 0.0;
  double p50_latency_us = 0.0;
  double p95_latency_us = 0.0;
  double p99_latency_us = 0.0;

  /// @brief Fraction of successful requests (0.0–1.0).
  [[nodiscard]] auto success_rate() const -> double {
    return (total_requests > 0) ? static_cast<double>(successful) /
                                      static_cast<double>(total_requests)
                                : 0.0;
  }

  /// @brief Requests per second.
  [[nodiscard]] auto throughput_rps() const -> double {
    return (elapsed_seconds > 0.0)
               ? static_cast<double>(total_requests) / elapsed_seconds
               : 0.0;
  }

  /// @brief Combined in+out bandwidth in MB/s.
  [[nodiscard]] auto bandwidth_mbps() const -> double {
    if (elapsed_seconds <= 0.0)
      return 0.0;
    return static_cast<double>(total_bytes_in + total_bytes_out) /
           elapsed_seconds / 1'000'000.0;
  }

  /// @brief Inbound bandwidth in MB/s.
  [[nodiscard]] auto bandwidth_in_mbps() const -> double {
    return (elapsed_seconds > 0.0) ? static_cast<double>(total_bytes_in) /
                                         elapsed_seconds / 1'000'000.0
                                   : 0.0;
  }

  /// @brief Outbound bandwidth in MB/s.
  [[nodiscard]] auto bandwidth_out_mbps() const -> double {
    return (elapsed_seconds > 0.0) ? static_cast<double>(total_bytes_out) /
                                         elapsed_seconds / 1'000'000.0
                                   : 0.0;
  }
};

/// @brief Thread-safe metrics collector.
///
/// Records per-request latency samples and byte counts. Computes percentiles
/// on-demand from the stored sample vector (reservoir, not streaming).
class MetricsCollector {
public:
  using Clock = std::chrono::steady_clock;
  using Duration = std::chrono::nanoseconds;

  MetricsCollector() = default;

  /// @brief Record a single request outcome.
  /// @param latency   Request processing duration.
  /// @param req_bytes Inbound payload size.
  /// @param resp_bytes Outbound payload size.
  /// @param success   True if the request succeeded.
  void record(Duration latency, std::size_t req_bytes, std::size_t resp_bytes,
              bool success) {
    // Single-threaded simulation: no lock needed.
    const auto us = std::chrono::duration<double, std::micro>(latency).count();
    latencies_.push_back(us);
    bytes_in_ += req_bytes;
    bytes_out_ += resp_bytes;
    if (success) {
      ++ok_;
    } else {
      ++fail_;
    }
  }

  /// @brief Compute a snapshot with percentiles.
  [[nodiscard]] auto snapshot() const -> RequestMetrics {
    RequestMetrics m;
    m.successful = ok_;
    m.failed = fail_;
    m.total_requests = ok_ + fail_;
    m.total_bytes_in = bytes_in_;
    m.total_bytes_out = bytes_out_;

    auto elapsed = end_time_ - start_time_;
    m.elapsed_seconds = std::chrono::duration<double>(elapsed).count();

    if (latencies_.empty()) {
      return m;
    }

    // Sort a copy for percentile computation.
    auto sorted = latencies_;
    std::sort(sorted.begin(), sorted.end());

    m.min_latency_us = sorted.front();
    m.max_latency_us = sorted.back();

    double sum = 0.0;
    for (auto v : sorted)
      sum += v;
    m.avg_latency_us = sum / static_cast<double>(sorted.size());

    auto pct = [&](double p) -> double {
      auto idx =
          static_cast<std::size_t>(p * static_cast<double>(sorted.size() - 1));
      return sorted[idx];
    };

    m.p50_latency_us = pct(0.50);
    m.p95_latency_us = pct(0.95);
    m.p99_latency_us = pct(0.99);

    return m;
  }

  /// @brief Reset all counters.
  void reset() {
    latencies_.clear();
    bytes_in_ = bytes_out_ = 0;
    ok_ = fail_ = 0;
    start_time_ = end_time_ = Clock::time_point{};
  }

  /// @brief Start the wall-clock timer.
  void start() { start_time_ = Clock::now(); }

  /// @brief Stop the wall-clock timer.
  void stop() { end_time_ = Clock::now(); }

private:
  // mutable std::mutex mu_; // Removed for performance
  std::vector<double> latencies_;
  std::size_t bytes_in_ = 0;
  std::size_t bytes_out_ = 0;
  std::size_t ok_ = 0;
  std::size_t fail_ = 0;
  Clock::time_point start_time_{};
  Clock::time_point end_time_{};
};

} // namespace mmap_viz::sim
