/// @file sim_main.cpp
/// @brief Entry point for the server simulation.
///
/// Creates a VisualizationArena (with optional WebSocket server for live
/// visualization), wires up a ServerSim + RequestGenerator, runs the
/// simulation, and prints a comprehensive metrics report.

#include "interface/visualization_arena.hpp"
#include "simulation/request_generator.hpp"
#include "simulation/server_sim.hpp"

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace mmap_viz;
using namespace mmap_viz::sim;

// ─── CLI argument parsing ───────────────────────────────────────────────

struct SimArgs {
  std::size_t arena_mb = 4; // Arena size in MB.
  std::size_t requests = 1000;
  TrafficPattern pattern = TrafficPattern::Mixed;
  bool enable_server = false;
  unsigned short port = 8080;
  std::size_t burst_size = 50;
  bool show_progress = true;
  std::size_t interval_us = 100; // Default 100us
  std::size_t sampling = 1;      // Default 1 (no sampling)
};

void print_usage(const char *prog) {
  std::cout
      << "Usage: " << prog << " [options]\n\n"
      << "Options:\n"
      << "  --arena-mb <N>       Arena size in MB (default: 4)\n"
      << "  --requests <N>       Total requests to simulate (default: 1000)\n"
      << "  --pattern <P>        Traffic pattern: steady|burst|ramp|mixed "
         "(default: mixed)\n"
      << "  --burst-size <N>     Requests per burst (default: 50)\n"
      << "  --interval-us <N>    Request interval in microseconds (default: "
         "100)\n"
      << "  --sampling <N>       Event sampling rate (default: 1)\n"
      << "  --server             Enable WebSocket visualization server\n"
      << "  --port <N>           Server port (default: 8080)\n"
      << "  --no-progress        Disable progress output\n"
      << "  --help               Show this help\n";
}

auto parse_pattern(const std::string &s) -> TrafficPattern {
  if (s == "steady")
    return TrafficPattern::Steady;
  if (s == "burst")
    return TrafficPattern::Burst;
  if (s == "ramp")
    return TrafficPattern::Ramp;
  return TrafficPattern::Mixed;
}

auto pattern_name(TrafficPattern p) -> const char * {
  switch (p) {
  case TrafficPattern::Steady:
    return "Steady";
  case TrafficPattern::Burst:
    return "Burst";
  case TrafficPattern::Ramp:
    return "Ramp";
  case TrafficPattern::Mixed:
    return "Mixed";
  }
  return "Unknown";
}

auto parse_args(int argc, char *argv[]) -> SimArgs {
  SimArgs args;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      std::exit(0);
    } else if (arg == "--arena-mb" && i + 1 < argc) {
      args.arena_mb = std::stoull(argv[++i]);
    } else if (arg == "--requests" && i + 1 < argc) {
      args.requests = std::stoull(argv[++i]);
    } else if (arg == "--pattern" && i + 1 < argc) {
      args.pattern = parse_pattern(argv[++i]);
    } else if (arg == "--burst-size" && i + 1 < argc) {
      args.burst_size = std::stoull(argv[++i]);
    } else if (arg == "--interval-us" && i + 1 < argc) {
      args.interval_us = std::stoull(argv[++i]);
    } else if (arg == "--sampling" && i + 1 < argc) {
      args.sampling = std::stoull(argv[++i]);
    } else if (arg == "--server") {
      args.enable_server = true;
    } else if (arg == "--port" && i + 1 < argc) {
      args.port = static_cast<unsigned short>(std::stoul(argv[++i]));
    } else if (arg == "--no-progress") {
      args.show_progress = false;
    }
  }
  return args;
}

// ─── Report formatting ─────────────────────────────────────────────────

void print_separator() { std::cout << std::string(60, '=') << '\n'; }

void print_report(const RequestMetrics &m, const VisualizationArena &arena) {
  std::cout << '\n';
  print_separator();
  std::cout << "  SERVER SIMULATION RESULTS\n";
  print_separator();

  std::cout << std::fixed << std::setprecision(1);

  // Request stats.
  std::cout << "\n  Requests\n"
            << "    Total:       " << m.total_requests << '\n'
            << "    Successful:  " << m.successful << '\n'
            << "    Failed:      " << m.failed << '\n'
            << "    Success Rate:" << std::setw(7) << m.success_rate() * 100
            << " %\n";

  // Throughput.
  std::cout << "\n  Throughput\n"
            << "    Duration:    " << std::setprecision(3) << m.elapsed_seconds
            << " s\n"
            << "    Rate:        " << std::setprecision(0) << m.throughput_rps()
            << " req/s\n";

  // Bandwidth.
  std::cout << std::setprecision(2);
  std::cout << "\n  Bandwidth\n"
            << "    Inbound:     " << m.bandwidth_in_mbps() << " MB/s\n"
            << "    Outbound:    " << m.bandwidth_out_mbps() << " MB/s\n"
            << "    Combined:    " << m.bandwidth_mbps() << " MB/s\n"
            << "    Total In:    " << m.total_bytes_in / 1024 << " KB\n"
            << "    Total Out:   " << m.total_bytes_out / 1024 << " KB\n";

  // Latency percentiles.
  std::cout << std::setprecision(1);
  std::cout << "\n  Latency\n"
            << "    Min:         " << m.min_latency_us << " µs\n"
            << "    Avg:         " << m.avg_latency_us << " µs\n"
            << "    P50:         " << m.p50_latency_us << " µs\n"
            << "    P95:         " << m.p95_latency_us << " µs\n"
            << "    P99:         " << m.p99_latency_us << " µs\n"
            << "    Max:         " << m.max_latency_us << " µs\n";

  // Arena health.
  std::cout << std::setprecision(1);
  auto pad = arena.padding_report();
  auto cache = arena.cache_report();
  std::cout << "\n  Arena\n"
            << "    Capacity:    " << arena.capacity() / 1024 << " KB\n"
            << "    Allocated:   " << arena.bytes_allocated() / 1024 << " KB\n"
            << "    Free:        " << arena.bytes_free() / 1024 << " KB\n"
            << "    Pad Eff:     " << pad.efficiency * 100 << " %\n"
            << "    Cache Util:  " << cache.avg_utilization * 100 << " %\n"
            << "    Cache Lines: " << cache.active_lines << " active / "
            << cache.total_lines << " total\n";

  print_separator();
  std::cout << std::endl;
}

} // namespace

// ─── Main ───────────────────────────────────────────────────────────────

int main(int argc, char *argv[]) {
  auto args = parse_args(argc, argv);

  std::cout << "\n  Server Simulation\n"
            << "  Pattern:    " << pattern_name(args.pattern) << '\n'
            << "  Requests:   " << args.requests << '\n'
            << "  Arena:      " << args.arena_mb << " MB\n";

  if (args.enable_server) {
    std::cout << "  Server:     http://localhost:" << args.port << '\n';
    if (args.sampling > 1) {
      std::cout << "  Sampling:   1/" << args.sampling << " events\n";
    }
  }
  std::cout << '\n';

  // 1. Create the arena.
  auto arena_result = mmap_viz::VisualizationArena::create({
      .arena_size = args.arena_mb * 1024 * 1024,
      .enable_server = args.enable_server,
      .port = args.port,
      .sampling = args.sampling,
  });

  if (!arena_result.has_value()) {
    std::cerr << "ERROR: Failed to create arena: "
              << arena_result.error().message() << '\n';
    return 1;
  }

  auto arena = std::move(*arena_result);

  // 2. Create the server.
  ServerSim server{arena};

  // 3. Create the request generator.
  RequestGenerator generator{{
      .pattern = args.pattern,
      .total_requests = args.requests,
      .steady_interval = std::chrono::microseconds(args.interval_us),
      .burst_size = args.burst_size,
  }};

  // 5. Run the simulation.
  std::size_t progress_interval = std::max<std::size_t>(1, args.requests / 20);

  generator.run(server, [&](std::uint64_t id, std::size_t total, bool ok) {
    if (args.show_progress && id % progress_interval == 0) {
      auto pct = static_cast<int>(100.0 * static_cast<double>(id) /
                                  static_cast<double>(total));
      std::cout << "\r  Progress: " << pct << "% (" << id << "/" << total << ")"
                << std::flush;
    }
    (void)ok;
  });

  if (args.show_progress) {
    std::cout << "\r  Progress: 100% (" << args.requests << "/" << args.requests
              << ")    \n";
  }

  // 6. Print results.
  auto metrics = server.metrics().snapshot();
  print_report(metrics, arena);

  // 7. If server is running, keep alive for inspection.
  if (args.enable_server) {
    std::cout << "  Server running at http://localhost:" << args.port
              << " — press Enter to exit.\n";
    std::cin.get();
  }

  return 0;
}
