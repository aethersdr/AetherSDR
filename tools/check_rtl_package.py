#!/usr/bin/env python3
"""Assert that a staged RTL application actually links its shipped dependencies."""
import argparse
from pathlib import Path
import re
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("root", type=Path)
p.add_argument("--platform", choices=("macos", "linux", "windows"), required=True)
a = p.parse_args()
root = a.root.resolve()
patterns = {"macos": ("librtlsdr*.dylib", "libusb-1.0*.dylib", "libfftw3f*.dylib"),
            "linux": ("librtlsdr.so*", "libusb-1.0.so*", "libfftw3f.so*"),
            "windows": ("rtlsdr.dll", "libusb-1.0.dll", "libfftw3f-3.dll")}[a.platform]
libs = []
for pattern in patterns:
    found = sorted(x for x in root.rglob(pattern) if x.is_file())
    if not found:
        raise SystemExit(f"Missing RTL package dependency: {pattern}")
    libs.append(found[0])
app_name = "AetherSDR.exe" if a.platform == "windows" else "AetherSDR"
apps = [x for x in root.rglob("*") if x.is_file() and x.name in ({"AetherSDR", "aethersdr"} if a.platform == "linux" else {app_name})]
if len(apps) != 1:
    raise SystemExit(f"Expected one {app_name}, got {apps}")
def imports(path):
    command = {"macos": ["otool", "-L"], "linux": ["readelf", "-d"],
               "windows": ["dumpbin", "/nologo", "/dependents"]}[a.platform]
    return subprocess.check_output(command + [str(path)], text=True).lower()
app_imports = imports(apps[0])
for token in ("rtlsdr", "fftw3f"):
    if token not in app_imports:
        raise SystemExit(f"Application does not link {token}")
if "libusb" not in imports(libs[0]):
    raise SystemExit("RTL driver does not link libusb")
if a.platform == "macos":
    executable_dir = apps[0].parent
    def rpaths(path):
        data = subprocess.check_output(["otool", "-l", str(path)], text=True)
        return re.findall(r"cmd LC_RPATH\s+cmdsize \d+\s+path (.*?) \(offset", data)
    def expand(value, loader):
        return Path(value.replace("@loader_path", str(loader.parent)).replace("@executable_path", str(executable_dir)))
    main_rpaths = [expand(value, apps[0]) for value in rpaths(apps[0])]
    magic = (b"\xfe\xed\xfa\xce", b"\xce\xfa\xed\xfe", b"\xfe\xed\xfa\xcf", b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca")
    for path in root.rglob("*"):
        if not path.is_file() or path.is_symlink():
            continue
        with path.open("rb") as handle:
            if handle.read(4) not in magic:
                continue
        search = [expand(value, path) for value in rpaths(path)] + main_rpaths
        ids = subprocess.check_output(["otool", "-D", str(path)], text=True).splitlines()[1:]
        own_id = ids[0].strip() if ids else None
        for line in subprocess.check_output(["otool", "-L", str(path)], text=True).splitlines()[1:]:
            name = line.strip().split(" (", 1)[0]
            if name == own_id or name.startswith(("/usr/lib/", "/System/Library/")):
                continue
            candidates = [base / name[7:] for base in search] if name.startswith("@rpath/") else [expand(name, path)]
            if not any(x.exists() and x.resolve().is_relative_to(root) for x in candidates):
                raise SystemExit(f"Unreachable runtime dependency: {path}: {name}")
if a.platform == "linux":
    # AppRun supplies usr/lib. Resolve the actual application/driver imports
    # through that directory; a copy hidden elsewhere is not sufficient.
    libdir = root / "usr/lib"
    for binary in (apps[0], libs[0]):
        needed = re.findall(r"Shared library: \[(.*?)\]", subprocess.check_output(["readelf", "-d", str(binary)], text=True))
        for name in needed:
            if any(token in name for token in ("rtlsdr", "fftw3f", "libusb")) and not (libdir / name).is_file():
                raise SystemExit(f"Unreachable RTL dependency: {binary}: {name}")
if a.platform == "windows":
    for path in libs:
        if path.parent != apps[0].parent:
            raise SystemExit(f"RTL runtime is not beside the executable: {path}")
print("RTL-PACKAGE-OK:", root)
