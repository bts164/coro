#!/usr/bin/env python3
"""
TCP echo server test suite for the Pico W echo server example.

Usage:
    python3 test_echo_server.py <host> [port]
    python3 test_echo_server.py 192.168.1.42
    python3 test_echo_server.py 192.168.1.42 8080
"""

import socket
import threading
import time
import sys
import os

HOST = sys.argv[1] if len(sys.argv) > 1 else None
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8080
TIMEOUT = 5.0  # seconds per operation

if not HOST:
    print("Usage: python3 test_echo_server.py <host> [port]")
    sys.exit(1)

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

passed = 0
failed = 0

def connect() -> socket.socket:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(TIMEOUT)
    s.connect((HOST, PORT))
    return s

def send_recv(s: socket.socket, data: bytes) -> bytes:
    """Send data and read back exactly len(data) bytes."""
    s.sendall(data)
    received = b""
    while len(received) < len(data):
        chunk = s.recv(len(data) - len(received))
        if not chunk:
            raise ConnectionError("Connection closed before full echo received")
        received += chunk
    return received

def run_test(name: str, fn):
    global passed, failed
    print(f"  {name} ... ", end="", flush=True)
    try:
        fn()
        print("PASS")
        passed += 1
    except Exception as e:
        print(f"FAIL — {e}")
        failed += 1

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def test_basic_echo():
    with connect() as s:
        msg = b"hello pico\n"
        assert send_recv(s, msg) == msg

def test_multiple_messages_one_connection():
    with connect() as s:
        for msg in [b"one\n", b"two\n", b"three\n"]:
            assert send_recv(s, msg) == msg

def test_empty_message():
    # Single newline — smallest valid message
    with connect() as s:
        msg = b"\n"
        assert send_recv(s, msg) == msg

def test_large_message():
    # 8 KB — exercises TCP segmentation
    with connect() as s:
        msg = (b"A" * 79 + b"\n") * 100  # 8000 bytes
        assert send_recv(s, msg) == msg

def test_binary_data():
    # All byte values 0x01-0xFF
    with connect() as s:
        msg = bytes(range(1, 256))
        assert send_recv(s, msg) == msg

def test_rapid_reconnect():
    # Connect, send one message, close, repeat — tests clean teardown
    for i in range(5):
        with connect() as s:
            msg = f"reconnect {i}\n".encode()
            assert send_recv(s, msg) == msg

def test_concurrent_connections():
    # Open several connections simultaneously — exercises JoinSet
    results = {}
    errors  = {}

    def client(idx):
        try:
            with connect() as s:
                msg = f"client {idx} hello\n".encode()
                echo = send_recv(s, msg)
                results[idx] = echo == msg
        except Exception as e:
            errors[idx] = str(e)

    threads = [threading.Thread(target=client, args=(i,)) for i in range(5)]
    for t in threads: t.start()
    for t in threads: t.join(timeout=TIMEOUT + 1)

    if errors:
        raise AssertionError(f"client errors: {errors}")
    if not all(results.values()):
        raise AssertionError(f"echo mismatch: {results}")

def test_server_handles_client_disconnect():
    # Connect, send partial data, then close without waiting for echo.
    # Server should handle the abrupt close without crashing.
    with connect() as s:
        s.sendall(b"goodbye")
    # Give the server a moment to process the close, then verify it's
    # still accepting new connections.
    time.sleep(0.2)
    with connect() as s:
        msg = b"still alive\n"
        assert send_recv(s, msg) == msg

def test_max_message_size():
    # Single message equal to the server's read buffer (4096 bytes)
    with connect() as s:
        msg = b"X" * 4096
        assert send_recv(s, msg) == msg

def test_multi_segment_large_transfer():
    # Send more data than TCP_SND_BUF (4*MSS ≈ 5840 bytes) in one write,
    # forcing write_impl to chunk across multiple send-buffer fills.
    with connect() as s:
        msg = b"B" * 32768
        assert send_recv(s, msg) == msg

# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------

print(f"\nTCP echo server test — {HOST}:{PORT}\n")

run_test("basic echo",                      test_basic_echo)
run_test("multiple messages, one conn",     test_multiple_messages_one_connection)
run_test("empty message (single newline)",  test_empty_message)
run_test("large message (8 KB)",            test_large_message)
run_test("binary data (bytes 1-255)",       test_binary_data)
run_test("rapid reconnect (x5)",            test_rapid_reconnect)
run_test("concurrent connections (x5)",     test_concurrent_connections)
run_test("client disconnect mid-stream",    test_server_handles_client_disconnect)
run_test("max read buffer (4096 bytes)",    test_max_message_size)
run_test("multi-segment transfer (32 KB)",  test_multi_segment_large_transfer)

print(f"\n{passed + failed} tests: {passed} passed, {failed} failed")
sys.exit(0 if failed == 0 else 1)
