#!/usr/bin/env python3
"""Compile the actual pinned RTL source against a hardware-free libusb seam."""
import argparse
import json
import os
import shlex
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--rtl-source", type=Path)
    parser.add_argument("--usb-source", type=Path)
    parser.add_argument("--harness", type=Path)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    recipe = Path(__file__).resolve().parent
    root = recipe.parent.parent
    pins = json.loads((recipe / "rtl-dependencies.json").read_text())
    rtl = args.rtl_source or root / ".rtl-deps-build" / pins["rtl"]["directory"]
    usb = args.usb_source or root / ".rtl-deps-build" / pins["libusb"]["directory"]
    harness = args.harness or root / "tests/rtl_async_cancellation_test.c"
    source = rtl.resolve() / "src/librtlsdr.c"
    if not source.is_file() or not (usb / "libusb/libusb.h").is_file():
        print("SKIP: pinned dependency sources absent; run setup-rtl-deps first")
        return 77
    variants = ("native",) if os.name == "nt" else ("posix", "windows-simulated")
    with tempfile.TemporaryDirectory(prefix="rtl-cancellation-") as directory:
        define = f'RTL_ASYNC_SOURCE="{source.as_posix()}"'
        for variant in variants:
            exe = Path(directory) / ("regression.exe" if os.name == "nt" else variant)
            if os.name == "nt":
                compiler = shutil.which("cl")
                if not compiler:
                    raise RuntimeError("Run from an MSVC developer environment")
                command = [compiler, "/nologo", "/O1", "/Gy", "/D" + define,
                           "/I" + str(usb.resolve() / "libusb"), "/I" + str(rtl.resolve() / "include"),
                           str(harness.resolve()), "/Fe:" + str(exe), "/link", "/OPT:REF"]
            else:
                compiler = shlex.split(os.environ.get("CC", "cc"))
                command = [*compiler, "-O1", "-g", "-ffunction-sections", "-fdata-sections",
                           "-D" + define, "-I" + str(usb.resolve() / "libusb"),
                           "-I" + str(rtl.resolve() / "include"), str(harness.resolve()), "-o", str(exe),
                           "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"]
                if variant == "windows-simulated":
                    command.append("-DRTL_SIMULATE_WINDOWS")
                if args.sanitize:
                    command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
                    if sys.platform != "darwin":
                        # ASan's global registration keeps the driver's tuner
                        # table alive, so --gc-sections cannot drop the tuner
                        # code this harness does not compile. Heap checks,
                        # which catch the early frees, are unaffected.
                        version = subprocess.run([*compiler, "--version"], capture_output=True,
                                                 text=True).stdout
                        command += (["-mllvm", "-asan-globals=0"] if "clang" in version
                                    else ["--param=asan-globals=0"])
            subprocess.run(command, cwd=directory, check=True)
            print(f"RTL cancellation variant: {variant}", flush=True)
            subprocess.run([str(exe)], check=True, timeout=20)
    return 0


if __name__ == "__main__":
    sys.exit(main())
