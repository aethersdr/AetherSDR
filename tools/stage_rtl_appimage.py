#!/usr/bin/env python3
"""Stage the pinned libusb SONAME that linuxdeploy's excludelist omits."""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def elf_dynamic(path):
    return subprocess.check_output(["readelf", "-d", str(path)], text=True)


def stage_libusb(prefix, appdir, read_dynamic=elf_dynamic):
    libdir = (prefix / "lib").resolve()
    driver = libdir / "librtlsdr.so.0"
    soname = "libusb-1.0.so.0"
    source = libdir / soname
    for path in (driver, source):
        if not path.is_file() or not path.resolve().is_relative_to(libdir):
            raise ValueError(f"Missing pinned RTL dependency inside {libdir}: {path.name}")
    needed = re.findall(r"\(NEEDED\).*?\[(.*?)\]", read_dynamic(driver))
    if [name for name in needed if name.startswith("libusb")] != [soname]:
        raise ValueError(f"Pinned RTL driver must require {soname}: {needed}")
    provided = re.findall(r"\(SONAME\).*?\[(.*?)\]", read_dynamic(source))
    if provided != [soname]:
        raise ValueError(f"Pinned libusb SONAME does not match {soname}: {provided}")

    destination = appdir / "usr/lib" / soname
    destination.parent.mkdir(parents=True, exist_ok=True)
    # Dereference the build-prefix symlink and install under the runtime SONAME.
    # Replace atomically, including a previous staging symlink if one exists.
    with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as handle:
        temporary = Path(handle.name)
    try:
        shutil.copy2(source, temporary)
        temporary.replace(destination)
    finally:
        temporary.unlink(missing_ok=True)
    return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("prefix", type=Path)
    parser.add_argument("appdir", type=Path)
    args = parser.parse_args()
    try:
        destination = stage_libusb(args.prefix, args.appdir)
    except ValueError as error:
        raise SystemExit(str(error)) from error
    print("RTL-APPIMAGE-LIBUSB:", destination)


if __name__ == "__main__":
    main()
