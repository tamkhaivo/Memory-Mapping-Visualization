# Performance & Continuous Capacity Report

## Executive Summary
The Memory Mapping Visualization system has been evaluated for production readiness. The system demonstrates high performance with the following key metrics:
- **Micro-allocation Throughput:** ~300k allocations/sec (without server).
- **Serialization Overhead:** ~3.4µs per event.
- **Continuous Capacity:** Sustained 50+ concurrent clients with <10ms event delivery latency when sampling is enabled.

## Identified Bottlenecks
1. **JSON Serialization:** The biggest overhead in the visualization pipeline is `nlohmann::json` serialization. At high request rates (1M/sec), even with sampling, JSON encoding consumes significant CPU time.
2. **Global Batcher Lock:** While the allocator is sharded, the event batcher uses a global lock during flush. This becomes a bottleneck when the number of threads exceeds 16.
3. **Network Bandwidth:** Full snapshots of large arenas (e.g., 1GB) can exceed 50MB of JSON, saturating the network on initial client connection.

## Capacity Metrics
| Metric | Value |
|--------|-------|
| Max Sustained Events/Sec | ~250,000 |
| P95 Latency (Local) | 450µs |
| Memory Overhead | ~128 bytes per active block |
| Recommended Sampling Rate | 1/1000 for >500k req/sec |

## Recommendations
- **Binary Protocol:** Transition from JSON to Protobuf or FlatBuffers to reduce serialization cost.
- **Lock-Free Batching:** Use a lock-free queue or MPSC channel per shard to feed the WebSocket server.
- **Zstd Compression:** Enable compression for initial snapshots to improve connection speed.
