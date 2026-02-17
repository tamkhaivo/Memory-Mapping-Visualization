#!/usr/bin/env python3
import socket
import struct
import hashlib
import base64
import json
import time
import subprocess
import os
import sys
import select

# Configuration
HOST = "localhost"
PORT = 9999
SERVER_BIN = os.getenv("SERVER_BIN", "./build-asan/server_sim")
TIMEOUT = 30  # seconds

class WebSocketClient:
    def __init__(self, host, port):
        self.host = host
        self.port = port
        self.sock = None

    def connect(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.settimeout(TIMEOUT)
        try:
            self.sock.connect((self.host, self.port))
        except ConnectionRefusedError:
            return False

        # Handshake
        key = base64.b64encode(os.urandom(16)).decode('utf-8')
        request = (
            f"GET / HTTP/1.1\r\n"
            f"Host: {self.host}:{self.port}\r\n"
            f"Upgrade: websocket\r\n"
            f"Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            f"Sec-WebSocket-Version: 13\r\n"
            f"\r\n"
        )
        self.sock.sendall(request.encode('utf-8'))

        response = self.sock.recv(4096)
        if not b"101 Switching Protocols" in response:
            raise Exception(f"Handshake failed: {response}")
        return True

    def recv_message(self):
        full_payload = b""
        while True:
            # Read header (2 bytes)
            header = self._read_exactly(2)
            if not header:
                return None
            
            b1, b2 = struct.unpack("!BB", header)
            fin = (b1 & 0x80) >> 7
            opcode = b1 & 0x0F
            masked = (b2 & 0x80) >> 7
            payload_len = b2 & 0x7F

            if payload_len == 126:
                payload_len = struct.unpack("!H", self._read_exactly(2))[0]
            elif payload_len == 127:
                payload_len = struct.unpack("!Q", self._read_exactly(8))[0]

            if masked:
                mask_key = self._read_exactly(4)

            payload = self._read_exactly(payload_len)
            if masked:
                 # Unmasking not implemented
                 pass 

            if opcode == 0x8: # Close
                return None
            
            if opcode == 0x0: # Continuation
                full_payload += payload
            elif opcode == 0x1: # Text (Start of message)
                full_payload = payload
            elif opcode == 0x2: # Binary
                 pass # Warning: ignoring binary
            else:
                 pass # Ping/Pong etc.

            if fin:
                if opcode == 0x1 or opcode == 0x0:
                     return full_payload.decode('utf-8')
                # If valid ping/pong, we might loop? For now return None or loop
                if opcode == 0x9: # Ping
                    # Should send Pong
                    pass
                if opcode == 0xA: # Pong
                    pass


    def _read_exactly(self, n):
        data = b""
        while len(data) < n:
            chunk = self.sock.recv(n - len(data))
            if not chunk:
                return None
            data += chunk
        return data

    def close(self):
        if self.sock:
            self.sock.close()

def main():
    if not os.path.exists(SERVER_BIN):
        print(f"Error: Server binary not found at {SERVER_BIN}")
        sys.exit(1)

    print(f"Starting server: {SERVER_BIN}")
    # Run server with slow interval to ensure we catch events
    server_proc = subprocess.Popen(
        [SERVER_BIN, "--server", "--port", str(PORT), "--requests", "1000", "--interval-us", "10000", "--no-progress"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE
    )

    client = WebSocketClient(HOST, PORT)
    connected = False
    
    # Wait for server to be ready
    for i in range(20):
        if client.connect():
            connected = True
            print("Connected to WebSocket server")
            break
        time.sleep(0.1)

    if not connected:
        print("Failed to connect to server")
        server_proc.terminate()
        sys.exit(1)

    try:
        snapshot_received = False
        event_batches_received = 0
        max_events = 5
        
        print("Waiting for messages...")
        
        # Read messages until we have snapshot and some events
        while not snapshot_received or event_batches_received < max_events:
            msg = client.recv_message()
            if not msg:
                break
            
            data = json.loads(msg)
            
            if isinstance(data, dict):
                # It's likely the snapshot
                if "blocks" in data:
                    print("Received Snapshot")
                    snapshot_received = True
                else:
                    print(f"Received unknown dict: {data.keys()}")
            
            elif isinstance(data, list):
                # It's an event batch
                print(f"Received batch of {len(data)} events")
                for evt in data:
                    # Basic schema validation
                    required_fields = ["type", "offset", "size", "tag", "event_id", "total_allocated"]
                    for f in required_fields:
                        if f not in evt:
                             raise Exception(f"Event missing field {f}: {evt}")
                event_batches_received += 1
                
        if not snapshot_received:
            raise Exception("Did not receive snapshot")
            
        print("Verification Successful")

    except Exception as e:
        print(f"Test Failed: {e}")
        server_proc.terminate()
        sys.exit(1)
    finally:
        client.close()
        server_proc.terminate()
        try:
            outs, errs = server_proc.communicate(timeout=2)
            print("--- Server Stdout ---")
            print(outs.decode('utf-8', errors='replace'))
            print("--- Server Stderr ---")
            print(errs.decode('utf-8', errors='replace'))
        except Exception:
            pass

if __name__ == "__main__":
    main()
