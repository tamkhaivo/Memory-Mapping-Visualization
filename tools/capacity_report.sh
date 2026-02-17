#!/bin/bash
set -e

# Configuration
PORT=9999
BUILD_DIR="./build-rel"
# Check if build-rel exists, otherwise try build
if [ ! -d "$BUILD_DIR" ]; then
    BUILD_DIR="./build"
fi

echo "--- Starting Performance & Capacity Report ---"

# 1. Run Micro-benchmarks
echo "[1/4] Running Micro-benchmarks..."
$BUILD_DIR/memory_mapper_bench_contention --benchmark_out=contention.json
$BUILD_DIR/memory_mapper_bench_serialization --benchmark_out=serialization.json

# 2. Run Stress Test
echo "[2/4] Running Stress Test for 10 seconds..."
$BUILD_DIR/stress_test_arena 10 8

# 3. Run Load Test (Capacity)
echo "[3/4] Running Load Test (10 clients, 5s)..."
# Start server in background
$BUILD_DIR/server_sim --server --port $PORT --requests 1000000 --sampling 100 --no-progress &
SERVER_PID=$!
sleep 2

python3 tools/load_tester.py --url ws://localhost:$PORT --clients 10 --duration 5

kill $SERVER_PID || true

# 4. Gradual Load Increase (Continuous Capacity)
echo "[4/4] Testing Continuous Capacity (50 clients, 5s)..."
$BUILD_DIR/server_sim --server --port $PORT --requests 2000000 --sampling 500 --no-progress &
SERVER_PID=$!
sleep 2

python3 tools/load_tester.py --url ws://localhost:$PORT --clients 50 --duration 5

kill $SERVER_PID || true

echo "--- Report Generation Complete ---"
echo "Check contention.json and serialization.json for raw data."
