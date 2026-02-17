#!/usr/bin/env python3
import asyncio
import websockets
import json
import time
import argparse
import statistics
from collections import deque

class LoadTester:
    def __init__(self, url, num_clients, duration):
        self.url = url
        self.num_clients = num_clients
        self.duration = duration
        self.events_received = 0
        self.latencies = []
        self.start_time = None
        self.stop_event = asyncio.Event()

    async def client_session(self, client_id):
        try:
            async with websockets.connect(self.url) as websocket:
                print(f"Client {client_id} connected")
                while not self.stop_event.is_set():
                    try:
                        msg = await asyncio.wait_for(websocket.recv(), timeout=1.0)
                        data = json.loads(msg)
                        
                        receive_time = time.time() * 1000000 # us
                        
                        if isinstance(data, list):
                            self.events_received += len(data)
                            for evt in data:
                                if "timestamp_us" in evt:
                                    latency = receive_time - evt["timestamp_us"]
                                    self.latencies.append(latency)
                        elif isinstance(data, dict) and data.get("type") == "snapshot":
                            # Ignore snapshot for throughput counting
                            pass
                            
                    except asyncio.TimeoutError:
                        continue
                    except Exception as e:
                        print(f"Client {client_id} error: {e}")
                        break
        except Exception as e:
            print(f"Client {client_id} failed to connect: {e}")

    async def run(self):
        print(f"Starting load test on {self.url} with {self.num_clients} clients for {self.duration}s")
        self.start_time = time.time()
        
        clients = [self.client_session(i) for i in range(self.num_clients)]
        
        # Run clients for the specified duration
        await asyncio.gather(
            asyncio.wait_for(asyncio.gather(*clients), timeout=self.duration + 5),
            self.stop_timer()
        )
        
        self.report()

    async def stop_timer(self):
        await asyncio.sleep(self.duration)
        self.stop_event.set()
        print("\nStopping load test...")

    def report(self):
        end_time = time.time()
        total_time = end_time - self.start_time
        eps = self.events_received / total_time
        
        print("\n--- Load Test Report ---")
        print(f"Total Duration: {total_time:.2f}s")
        print(f"Total Events:   {self.events_received}")
        print(f"Events/Sec:     {eps:.2f}")
        
        if self.latencies:
            print(f"Latency (us):")
            print(f"  Mean: {statistics.mean(self.latencies):.2f}")
            print(f"  P50:  {statistics.median(self.latencies):.2f}")
            if len(self.latencies) > 1:
                p95 = sorted(self.latencies)[int(len(self.latencies) * 0.95)]
                print(f"  P95:  {p95:.2f}")
        else:
            print("No latency data received.")

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", default="ws://localhost:9999", help="WebSocket URL")
    parser.add_argument("--clients", type=int, default=10, help="Number of concurrent clients")
    parser.add_argument("--duration", type=int, default=10, help="Test duration in seconds")
    args = parser.parse_args()

    tester = LoadTester(args.url, args.clients, args.duration)
    asyncio.run(tester.run())
