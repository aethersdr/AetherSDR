#!/usr/bin/env python3
"""Run a bounded AetherSDR automation-bridge memory soak.

The app samples itself, so this driver can disconnect/reconnect without losing
the in-process series. The final JSON is written atomically and includes both
the compact trend report and retained raw points for plotting.
"""

import argparse
import datetime as dt
import json
import math
import os
import sys
import tempfile
import time

from automation_probe import Bridge, discover_socket


def mib(value):
    return float(value or 0.0) / (1024.0 * 1024.0)


def atomic_write_json(path, payload):
    path = os.path.abspath(path)
    directory = os.path.dirname(path)
    os.makedirs(directory, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".aethersdr-memory-",
                                     suffix=".json.tmp", dir=directory)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as handle:
            json.dump(payload, handle, indent=2, sort_keys=True)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    except Exception:
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise
    return path


def progress_line(status, remaining):
    report = status.get("report", {})
    snapshot = status.get("snapshot", {})
    process = snapshot.get("process", {})
    return (f"samples={report.get('sampleCount', 0):4d} "
            f"rss={mib(process.get('residentBytes')):8.1f} MiB "
            f"private={mib(process.get('privateBytes')):8.1f} MiB "
            f"remaining={max(0, int(remaining)):4d}s")


def main():
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    parser = argparse.ArgumentParser(
        description="Profile AetherSDR memory through the automation bridge")
    parser.add_argument("--duration", type=float, default=300.0,
                        help="soak duration in seconds (default 300)")
    parser.add_argument("--interval", type=float, default=5.0,
                        help="in-app sample interval in seconds (default 5)")
    parser.add_argument("--progress", type=float, default=30.0,
                        help="progress print cadence in seconds (default 30)")
    parser.add_argument("--socket", help="override bridge socket / pipe")
    parser.add_argument(
        "--output",
        default=os.path.join(tempfile.gettempdir(),
                             f"aethersdr-memory-{stamp}.json"),
        help="final JSON path (default: platform temp directory)")
    args = parser.parse_args()

    if not 1.0 <= args.duration <= 7 * 24 * 3600:
        parser.error("--duration must be 1 second .. 7 days")
    if not 0.25 <= args.interval <= 3600.0:
        parser.error("--interval must be 0.25 .. 3600 seconds")
    if not 1.0 <= args.progress <= 3600.0:
        parser.error("--progress must be 1 .. 3600 seconds")

    interval_ms = int(round(args.interval * 1000.0))
    max_samples = int(math.ceil(args.duration / args.interval)) + 2
    if max_samples > 10000:
        parser.error("duration/interval needs more than 10000 samples; "
                     "increase --interval")

    socket_path = args.socket or discover_socket()
    if not socket_path:
        sys.exit("error: no automation bridge found; launch AetherSDR with "
                 "AETHER_AUTOMATION=1 or pass --socket")

    bridge = Bridge(socket_path)
    bridge_info = bridge.request({"cmd": "whoami"})
    if not bridge_info.get("ok"):
        bridge.close()
        sys.exit(f"error: bridge identity check failed: {bridge_info}")
    if bridge_info.get("txAllowed"):
        print("warning: this bridge reports TX automation enabled; "
              "the memory soak remains read-only", file=sys.stderr)
    started_wall = dt.datetime.now(dt.timezone.utc)
    started = bridge.request({"cmd": "memory", "action": "start",
                              "value": f"{interval_ms} {max_samples}"})
    if not started.get("ok"):
        bridge.close()
        sys.exit(f"error: memory profiler refused start: {started}")

    print(f"memory soak: {args.duration:.0f}s at {args.interval:.2f}s "
          f"({max_samples} samples max)")
    deadline = time.monotonic() + args.duration
    next_progress = time.monotonic()
    interrupted = False
    try:
        while True:
            now = time.monotonic()
            if now >= deadline:
                break
            if now >= next_progress:
                status = bridge.request({"cmd": "memory", "action": "status"})
                print(progress_line(status, deadline - now), flush=True)
                next_progress = now + args.progress
            time.sleep(min(1.0, max(0.0, deadline - now)))
    except KeyboardInterrupt:
        interrupted = True
        print("interrupted; collecting the partial profile", file=sys.stderr)

    stopped = bridge.request({"cmd": "memory", "action": "stop"})
    samples = bridge.request({"cmd": "memory", "action": "samples"})
    bridge.close()
    payload = {
        "format": "AetherSDR-memory-soak-v1",
        "startedUtc": started_wall.isoformat(),
        "finishedUtc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "requestedDurationSeconds": args.duration,
        "sampleIntervalMs": interval_ms,
        "interrupted": interrupted,
        "bridge": bridge_info,
        "start": started,
        "stop": stopped,
        "series": samples,
    }
    output = atomic_write_json(args.output, payload)

    report = stopped.get("report", {})
    resident = report.get("byteTrends", {}).get("process.residentBytes", {})
    print(f"resident delta: {mib(resident.get('delta')):+.1f} MiB; "
          f"slope: {mib(resident.get('slopeBytesPerHour')):+.1f} MiB/hour; "
          f"R²={float(resident.get('rSquared') or 0.0):.3f}")
    suspects = report.get("growthSuspects", [])
    print(f"growth suspects: {len(suspects)}")
    for suspect in suspects:
        print(f"  {suspect.get('metric')}: "
              f"delta={mib(suspect.get('deltaBytes')):+.1f} MiB "
              f"slope={mib(suspect.get('slopeBytesPerHour')):+.1f} MiB/hour "
              f"R²={float(suspect.get('rSquared') or 0.0):.3f}")
    print(f"profile: {output}")
    return 130 if interrupted else 0


if __name__ == "__main__":
    sys.exit(main())
