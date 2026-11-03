#!/usr/bin/env python3
"""Build the full Linux graph and report sampled disk growth from cleanup."""
import json
import os
from pathlib import Path
import stat
import subprocess
import time


def available_bytes():
    stats = os.statvfs(".")
    return stats.f_bavail * stats.f_frsize


def build_file_sizes():
    """Final allocated/logical bytes, counting each regular-file inode once."""
    groups = {}
    seen = set()
    for path in Path("build").rglob("*"):
        try:
            info = path.lstat()
        except OSError:
            continue
        inode = (info.st_dev, info.st_ino)
        if not stat.S_ISREG(info.st_mode) or inode in seen:
            continue
        seen.add(inode)
        category = {".o": "objects", ".a": "static_archives"}.get(path.suffix)
        if category is None:
            category = "executables" if info.st_mode & 0o111 else "other"
        totals = groups.setdefault(category, {"files": 0, "logical_bytes": 0,
                                              "allocated_bytes": 0})
        totals["files"] += 1
        totals["logical_bytes"] += info.st_size
        totals["allocated_bytes"] += info.st_blocks * 512
    return groups


def main():
    baseline = int(Path("/tmp/aether-ci-free-bytes").read_text())
    minimum = available_bytes()
    started = time.monotonic()
    status = 1
    try:
        jobs = subprocess.check_output(["nproc"], text=True).strip()
        child = subprocess.Popen(["cmake", "--build", "build", "--parallel", jobs])
        while True:
            minimum = min(minimum, available_bytes())
            try:
                status = child.wait(timeout=0.25)
                break
            except subprocess.TimeoutExpired:
                pass
    finally:
        minimum = min(minimum, available_bytes())
        metrics = {
            "cleanup_available_bytes": baseline,
            "sampled_minimum_available_bytes": minimum,
            "sampled_peak_growth_bytes": baseline - minimum,
            "build_elapsed_seconds": round(time.monotonic() - started, 2),
            "sampling_interval_seconds": 0.25,
            "build_exit_code": status,
        }
        print("CI_DISK_METRICS " + json.dumps(metrics), flush=True)
        try:
            Path("build").mkdir(exist_ok=True)
            Path("build/ci-disk-metrics.json").write_text(json.dumps(metrics, indent=2) + "\n")
        except OSError as error:
            print(f"Could not save disk metrics: {error}", flush=True)
        subprocess.run(["df", "-B1", "."], check=False)
        subprocess.run(["du", "-sh", "build", "/ccache"], check=False)
        try:
            print("CI_FINAL_BUILD_FILES " + json.dumps(build_file_sizes()), flush=True)
        except OSError as error:
            print(f"Could not inventory final build files: {error}", flush=True)
    return status


if __name__ == "__main__":
    raise SystemExit(main())

