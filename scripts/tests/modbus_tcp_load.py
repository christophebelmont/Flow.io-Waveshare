#!/usr/bin/env python3
"""Read-only, one-to-six-client load probe for the Flow.io Modbus TCP/RTU gateway.

Standard-library only. No writes are transmitted; only FC03/FC04 requests.
Give one real, SAFE register range per enabled TCP listener. This is a hardware
test, not a simulator or a substitute for an ESP32/RS485 integration test.

Example (replace unit IDs and ranges with those of your devices):
  # Target only a port that is enabled (here port 502).
  python3 scripts/tests/modbus_tcp_load.py --host 192.168.1.71 \
    --target 502:17:500:28:3 --requests 8
  # Or exercise 502, 503, 504 together if all are enabled.
  python3 scripts/tests/modbus_tcp_load.py --host 192.168.1.71 \
    --target 502:17:52:97:3 --target 503:17:500:28:3 \
    --target 504:17:1000:3:3 --requests 8 --probe-third
"""
import argparse
import collections
import concurrent.futures
import dataclasses
import math
import queue
import socket
import statistics
import struct
import threading
import time


@dataclasses.dataclass(frozen=True)
class Target:
    port: int
    unit: int
    address: int
    count: int
    function: int


@dataclasses.dataclass
class Result:
    port: int
    worker: int
    samples_ms: list = dataclasses.field(default_factory=list)
    exceptions: collections.Counter = dataclasses.field(default_factory=collections.Counter)
    errors: collections.Counter = dataclasses.field(default_factory=collections.Counter)


def target_arg(text):
    try:
        port, unit, address, count, function = (int(x) for x in text.split(":"))
    except ValueError as exc:
        raise argparse.ArgumentTypeError("Expected PORT:UNIT:ADDRESS:COUNT:FC") from exc
    if port not in (502, 503, 504):
        raise argparse.ArgumentTypeError("Port must be 502, 503 or 504")
    if not 0 <= unit <= 247:
        raise argparse.ArgumentTypeError("TCP unit ID must be 0..247")
    if not 1 <= count <= 125 or not 0 <= address <= 65535 or address + count > 65536:
        raise argparse.ArgumentTypeError("Invalid read address/count (FC03/04 max 125)")
    if function not in (3, 4):
        raise argparse.ArgumentTypeError("Only read functions FC03/FC04 are allowed")
    return Target(port, unit, address, count, function)


def receive_exact(sock, count, deadline):
    result = bytearray()
    while len(result) < count:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("TCP response timed out")
        sock.settimeout(remaining)
        data = sock.recv(count - len(result))
        if not data:
            raise ConnectionError("TCP peer closed connection")
        result.extend(data)
    return bytes(result)


def read_registers(sock, target, transaction_id, timeout_s):
    frame = struct.pack(">HHHBBHH", transaction_id, 0, 6, target.unit,
                        target.function, target.address, target.count)
    start = time.perf_counter()
    deadline = time.monotonic() + timeout_s
    sock.settimeout(timeout_s)
    sock.sendall(frame)
    prefix = receive_exact(sock, 7, deadline)
    response_id, protocol, length, unit = struct.unpack(">HHHB", prefix)
    if (response_id, protocol, unit) != (transaction_id, 0, target.unit):
        raise ValueError("MBAP transaction/protocol/unit mismatch")
    if not 2 <= length <= 254:
        raise ValueError("Invalid MBAP length")
    pdu = receive_exact(sock, length - 1, deadline)
    if len(pdu) == 2 and pdu[0] == (target.function | 0x80):
        return (time.perf_counter() - start) * 1000.0, pdu[1]
    if len(pdu) != 2 + target.count * 2 or pdu[0] != target.function:
        raise ValueError("Unexpected function or number of returned registers")
    if pdu[1] != target.count * 2:
        raise ValueError("Incorrect Modbus byte count")
    return (time.perf_counter() - start) * 1000.0, None


def worker_task(host, target, worker_id, requests, timeout_s, pace_s,
                ready_queue, release):
    result = Result(target.port, worker_id)
    try:
        sock = socket.create_connection((host, target.port), timeout=timeout_s)
    except (OSError, TimeoutError) as exc:
        ready_queue.put((target.port, worker_id, False, type(exc).__name__))
        result.errors["connect_" + type(exc).__name__] += 1
        return result
    with sock:
        ready_queue.put((target.port, worker_id, True, ""))
        if not release.wait(timeout=timeout_s + 15):
            result.errors["startup_timeout"] += 1
            return result
        for index in range(requests):
            # Unique MBAP IDs within each persistent connection.
            transaction_id = ((worker_id + 1) * 1000 + index) & 0xFFFF
            try:
                elapsed, exception = read_registers(
                    sock, target, transaction_id, timeout_s)
                if exception is None:
                    result.samples_ms.append(elapsed)
                else:
                    result.exceptions[f"0x{exception:02X}"] += 1
            except (OSError, TimeoutError, ValueError) as exc:
                result.errors[type(exc).__name__] += 1
                # After a framing error/timeout the stream can be out of sync.
                break
            if pace_s and index + 1 < requests:
                time.sleep(pace_s)
    return result


def check_third_connection(host, target, timeout_s):
    """Best-effort capacity probe; TCP connect alone does not prove admission."""
    try:
        with socket.create_connection((host, target.port), timeout=timeout_s) as sock:
            sock.settimeout(min(timeout_s, 2.0))
            try:
                value = sock.recv(1)
                if value == b"":
                    return "closed by server (expected)"
                return "unexpected data"
            except ConnectionResetError:
                return "reset by server (expected)"
            except socket.timeout:
                return "inconclusive (connection not closed within 2 s)"
    except OSError as exc:
        return f"connection refused/failed ({type(exc).__name__})"


def percentile(values, fraction):
    ordered = sorted(values)
    return ordered[max(0, math.ceil(fraction * len(ordered)) - 1)]


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", required=True, help="Flow.io LAN IP/hostname (never the provisioning AP)")
    parser.add_argument("--target", type=target_arg, required=True, action="append",
                        help="PORT:TCP_UNIT:READ_ADDRESS:COUNT:FC (one per port)")
    parser.add_argument("--requests", type=int, default=8,
                        help="Read transactions per connection (default: 8; max: 1000)")
    parser.add_argument("--timeout", type=float, default=12.0,
                        help="End-to-end TCP timeout in seconds (default: 12)")
    parser.add_argument("--pace-ms", type=int, default=100,
                        help="Pause between requests per worker (default: 100 ms)")
    parser.add_argument("--probe-third", action="store_true",
                        help="While 2 clients/port are connected, probe rejection of a third")
    args = parser.parse_args()
    by_port = {target.port: target for target in args.target}
    if len(by_port) != len(args.target) or not 1 <= len(by_port) <= 3:
        parser.error("Provide one to three distinct ports from 502, 503 and 504")
    if not 1 <= args.requests <= 1000 or not 0.2 <= args.timeout <= 120:
        parser.error("--requests must be 1..1000 and --timeout 0.2..120 s")
    if not 0 <= args.pace_ms <= 60000:
        parser.error("--pace-ms must be 0..60000")

    print("Read-only FC03/FC04 test (no Modbus writes).")
    print(f"Two persistent TCP connections per configured port: "
          f"{2 * len(by_port)} clients total.")
    print("Use configured register ranges that the actual devices can read.")
    for target in args.target:
        print(f"  Port {target.port}: unit={target.unit}, FC{target.function:02d}, "
              f"address={target.address}, count={target.count}")

    ready_queue = queue.Queue()
    release = threading.Event()
    futures = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=2 * len(by_port)) as executor:
        for target in args.target:
            for worker_id in range(2):
                futures.append(executor.submit(worker_task, args.host, target,
                    worker_id, args.requests, args.timeout, args.pace_ms / 1000.0,
                    ready_queue, release))
        ready = []
        try:
            for _ in range(2 * len(by_port)):
                ready.append(ready_queue.get(timeout=args.timeout + 3))
            connected = sum(1 for item in ready if item[2])
            print(f"Connections established: {connected}/{2 * len(by_port)}")
            for port, worker_id, ok, reason in ready:
                if not ok:
                    print(f"  Port {port} client {worker_id + 1}: {reason}")
            if connected == 2 * len(by_port) and args.probe_third:
                for port in sorted(by_port):
                    outcome = check_third_connection(args.host, by_port[port], args.timeout)
                    print(f"  Port {port} third-client probe: {outcome}")
        except queue.Empty:
            print("Timed out waiting for all initial connections")
        finally:
            release.set()
        results = [future.result() for future in futures]

    for port in sorted(by_port):
        same_port = [x for x in results if x.port == port]
        latencies = [value for x in same_port for value in x.samples_ms]
        exceptions = sum((x.exceptions for x in same_port), collections.Counter())
        errors = sum((x.errors for x in same_port), collections.Counter())
        print(f"\nPort {port}: successful={len(latencies)}, "
              f"exceptions={sum(exceptions.values())}, errors={sum(errors.values())}")
        if latencies:
            print(f"  response ms: avg={statistics.mean(latencies):.1f}, "
                  f"p50={statistics.median(latencies):.1f}, "
                  f"p95={percentile(latencies, .95):.1f}, "
                  f"max={max(latencies):.1f}")
        if exceptions:
            print(f"  Modbus exceptions: {dict(exceptions)}")
        if errors:
            print(f"  TCP/protocol errors: {dict(errors)}")
    if any(x.errors for x in results):
        return 2
    if any(x.exceptions for x in results):
        print("\nModbus exceptions observed: check equipment ranges, RS485 queue and timeouts.")
        return 1
    print("\nAll attempted read transactions received valid responses.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
