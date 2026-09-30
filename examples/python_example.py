#!/usr/bin/env python3
"""
FluxIO Python API Example
Demonstrating:
1. High-level synchronous Direct I/O (write_block, read_block, fsync_block)
2. High-performance asynchronous pipeline (write, read, process_submissions, wait_completion)
3. Both native io_uring and POSIX Direct I/O engines.
"""

import os
import sys

# Ensure local build is found
sys.path.insert(0, os.path.abspath(os.path.dirname(__file__) + "/.."))

import fluxio

def test_engine(name, engine_cls):
    print(f"\n{'=' * 60}")
    print(f" Testing {name} ({engine_cls.__name__})")
    print(f"{'=' * 60}")
    
    safe_name = name.lower().replace(' ', '_').replace('/', '_')
    filename = f"test_{safe_name}.dat"
    with open(filename, "wb") as f:
        f.truncate(1024 * 1024) # 1MB

    engine = engine_cls()
    if not engine.register_file(0, os.path.abspath(filename)):
        print(f"Failed to register file: {filename}")
        return

    if not engine.setup():
        print(f"Failed to setup {name} (requires CAP_SYS_NICE/root if io_uring SQPOLL). Skipping.")
        os.remove(filename)
        return

    print(f"Engine setup successfully! Registered FD: {engine.get_fd(0)}")

    # 1. Synchronous High-Level API
    print("\n--- 1. Synchronous High-Level Direct I/O ---")
    payload = b"FLUXIO_PYTHON_DIRECT_IO_TEST" + b"0" * (4096 - 28)
    written = engine.write_block(slot=0, buffer=payload, offset=0)
    print(f"Synchronous Write: {written} bytes written at offset 0")

    read_data = engine.read_block(slot=0, size=4096, offset=0)
    print(f"Synchronous Read:  {len(read_data)} bytes read. Data matches: {read_data == payload}")

    engine.fsync_block(slot=0)
    print(f"Synchronous Fsync: Completed successfully")

    # 2. Asynchronous Pipeline API
    print("\n--- 2. Lock-Free Asynchronous Pipeline API ---")
    num_ops = 8
    buffers = [bytearray(f"ASYNC_BLOCK_{i:04d}".encode() + b"X" * (4096 - 16)) for i in range(num_ops)]

    for i in range(num_ops):
        ok = engine.write(slot=0, buffer=buffers[i], offset=i * 4096, user_data=100 + i)
        assert ok, f"Failed to enqueue write request {i}"

    processed = engine.process_submissions(max_batch=num_ops)
    print(f"Processed {processed} submissions to backend queue")

    completed = 0
    while completed < num_ops:
        comp = engine.wait_completion(timeout_ms=1000)
        if comp is not None:
            print(f"  Completion received: {comp} (latency: {comp.latency_cycles} cycles)")
            completed += 1

    print(f"Async Pipeline: Completed all {completed} operations successfully!")
    os.remove(filename)

def main():
    print(f"FluxIO Python Version: 1.0.0")
    print(f"Compiled with native io_uring support: {fluxio.has_io_uring()}")
    print(f"Current CPU core: {fluxio.get_current_core()}")

    # Test POSIX Direct I/O Engine (Always works across all platforms & unprivileged sandboxes)
    test_engine("POSIX Direct I/O Engine", fluxio.PosixStorageEngine)

    # Test io_uring Engine (Kernel SQPOLL, sub-10us latency on NVMe)
    if hasattr(fluxio, "StorageEngine"):
        test_engine("io_uring Engine", fluxio.StorageEngine)

if __name__ == "__main__":
    main()
