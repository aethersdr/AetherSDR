#!/usr/bin/env python3
import json
from pathlib import Path
import shutil
import subprocess
import time

def call(args):
    subprocess.run(args, check=True)

def capture(args):
    return subprocess.check_output(args, text=True)

report = {"source_parent": "8672486cfcf49dc500d98ca00a6726ae4c48d2b3"}
report["nproc"] = capture(["nproc"]).strip()
report["compiler"] = capture(["c++", "--version"]).splitlines()[0]
report["linker"] = capture(["ld", "--version"]).splitlines()[0]
for path in ["/proc/meminfo", "/sys/fs/cgroup/memory.max", "/sys/fs/cgroup/cpu.max"]:
    p = Path(path)
    if p.exists():
        report[path] = p.read_text()
print("PROBE_PLATFORM " + json.dumps(report), flush=True)
flags = ["cmake", "-B", "build", "-G", "Ninja", "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
         "-DENABLE_NVIDIA_AFX=ON", "-DCMAKE_C_FLAGS_RELWITHDEBINFO=-O2 -g1 -DNDEBUG",
         "-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O2 -g1 -DNDEBUG",
         "-DCMAKE_EXE_LINKER_FLAGS=-Wl,--compress-debug-sections=zlib-gabi",
         "-DAETHER_GPU_SPECTRUM=ON", "-DCMAKE_C_COMPILER_LAUNCHER=ccache",
         "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"]
binary = Path("build/tci_rx_audio_test")
try:
    for mode in ["OFF", "ON"]:
        call(flags + ["-DAETHER_CI_STRIP_TEST_DEBUG=" + mode])
        commands = Path("build/compile_commands.json").read_text()
        Path("probe-commands-" + mode + ".json").write_text(commands)
        if mode == "ON":
            same = commands == Path("probe-commands-OFF.json").read_text()
            report["compiler_commands_identical"] = same
            print("COMPILER_COMMANDS_IDENTICAL " + str(same), flush=True)
        call(["ccache", "--zero-stats"])
        started = time.monotonic()
        call(["cmake", "--build", "build", "--target", "tci_rx_audio_test",
              "--parallel", report["nproc"]])
        stats = capture(["ccache", "--show-stats"])
        print(stats, flush=True)
        Path("probe-ccache-" + mode + ".txt").write_text(stats)
        rows = Path("build/.ninja_log").read_text().splitlines()
        links = [r.split("\t") for r in rows if "\ttci_rx_audio_test\t" in r]
        latest = links[-1]
        report[mode] = {
            "build_seconds": round(time.monotonic() - started, 3),
            "link_milliseconds": int(latest[1]) - int(latest[0]),
            "binary_bytes": binary.stat().st_size,
        }
        shutil.copyfile("build/.ninja_log", "probe-ninja-" + mode + ".log")
        Path("probe-sections-" + mode + ".txt").write_text(
            capture(["readelf", "--wide", "--section-headers", str(binary)]))
finally:
    Path("probe-report.json").write_text(json.dumps(report, indent=2) + "\n")
    print("LINK_PROBE_REPORT " + json.dumps(report), flush=True)

