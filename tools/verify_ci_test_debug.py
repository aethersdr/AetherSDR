#!/usr/bin/env python3
"""Verify CI test stripping and app diagnostics against the actual linked ELFs."""
from pathlib import Path
import re
import subprocess


def sections(binary):
    return subprocess.check_output(["readelf", "--wide", "--section-headers", str(binary)], text=True)


def main():
    binaries = Path("build/ci-stripped-test-executables.txt").read_text().splitlines()
    if not binaries:
        raise SystemExit("No CI test executables were inventoried")
    for binary in binaries:
        listing = sections(binary)
        if re.search(r"\]\s+\.(?:debug_|zdebug_)", listing):
            raise SystemExit(f"Debug sections remain in {binary}")
        if not re.search(r"\]\s+\.symtab\s", listing):
            raise SystemExit(f"Non-debug symbols were stripped from {binary}")
    print(f"Verified debug stripping and retained symbol tables in {len(binaries)} test executables")

    app = Path("build/AetherSDR")
    if not re.search(r"\]\s+\.debug_line\s+.*\bC\b", sections(app)):
        raise SystemExit("AetherSDR has no compressed debug line section")
    symbols = subprocess.check_output(["nm", "-n", str(app)], text=True)
    address = next((line.split()[0] for line in symbols.splitlines()
                    if len(line.split()) == 3 and line.split()[2] == "main"), None)
    if address is None:
        raise SystemExit("AetherSDR has no main symbol")
    location = subprocess.check_output(["addr2line", "-e", str(app), address], text=True).strip()
    print(f"main: {location}")
    if not re.search(r"/src/main\.cpp:[1-9][0-9]*(?:\s|$)", location):
        raise SystemExit("AetherSDR main did not resolve to a source line")


if __name__ == "__main__":
    main()

