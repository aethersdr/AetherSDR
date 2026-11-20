#!/usr/bin/env python3
"""Rebuild the ONNX Runtime 1.27.0 release archives that setup-onnxruntime.* pin.

Microsoft's GitHub release is gone, so the archives the setup scripts download
are hosted on this repo's `onnxruntime-1.27.0` release. linux-x64 is
Microsoft's original archive, unchanged. The other three are rebuilt here, in
the original layout, from two inputs checked against fixed SHA-256 pins:

  - the Microsoft-signed NuGet package Microsoft.ML.OnnxRuntime 1.27.0, the
    source of every native library;
  - the original linux-x64 archive, the source of the headers and docs (the
    package's headers are the same text with CRLF line endings, and it lacks
    include/core/providers/).

Output is byte-for-byte reproducible: entries sorted, owner root, one fixed
mtime, gzip mtime 0. Running this again must print the SHA-256 values the
setup scripts pin.

osx-arm64 has to run on macOS. The package's dylib calls itself
@rpath/libonnxruntime.1.dylib, but sherpa-onnx links
@rpath/libonnxruntime.1.27.0.dylib, which is the name the original archive
used. So the dylib is renamed, its install name set to match, and it is
ad-hoc signed again: changing the install name breaks Microsoft's signature,
and arm64 will not load an unsigned dylib. The macOS release build re-signs
every dylib with AetherSDR's identity anyway.

Usage: repack_onnxruntime.py --nupkg PATH --linux-x64-tgz PATH --out DIR
                             [--platform linux-aarch64|osx-arm64|win-x64 ...]
"""

import argparse
import gzip
import hashlib
import io
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import zipfile

VERSION = "1.27.0"
NUPKG_SHA256 = "c725e8cefdfe4de08befb9f79b3f218c162439cdcdb97cbbb5192888d58ea669"
LINUX_X64_SHA256 = "547e40a48f1fe73e3f812d7c88a948612c23f896b91e4e2ee1e232d7b468246f"
# Microsoft's 1.27.0 archives are dated 2026-06-15; keep that date.
MTIME = 1781481600  # 2026-06-15T00:00:00Z
ZIP_DATE = (2026, 6, 15, 0, 0, 0)
DOCS = ("GIT_COMMIT_ID", "LICENSE", "Privacy.md", "README.md",
        "ThirdPartyNotices.txt", "VERSION_NUMBER")


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        while chunk := stream.read(1 << 20):
            digest.update(chunk)
    return digest.hexdigest()


def check_pin(path, expected):
    actual = sha256(path)
    if actual != expected:
        sys.exit(f"{path}: SHA-256 {actual}, expected {expected}")


def original_tree(tgz):
    """{relative path: bytes} for headers and docs from the original archive."""
    files = {}
    root = f"onnxruntime-linux-x64-{VERSION}/"
    with tarfile.open(tgz) as archive:
        for member in archive.getmembers():
            name = member.name.removeprefix(root)
            if not member.isfile():
                continue
            if name in DOCS or name.startswith("include/") \
                    or name.startswith("lib/cmake/") or name.startswith("lib/pkgconfig/"):
                files[name] = archive.extractfile(member).read()
    return files


def native(nupkg, platform, name):
    with zipfile.ZipFile(nupkg) as package:
        return package.read(f"runtimes/{platform}/native/{name}")


def macos_dylib(data):
    """Rename the install name to the versioned one and ad-hoc sign again."""
    if sys.platform != "darwin":
        sys.exit("osx-arm64 must be built on macOS (install_name_tool, codesign)")
    with tempfile.TemporaryDirectory() as temporary:
        path = Path(temporary) / f"libonnxruntime.{VERSION}.dylib"
        path.write_bytes(data)
        subprocess.run(["codesign", "--remove-signature", path], check=True)
        subprocess.run(["install_name_tool", "-id",
                        f"@rpath/libonnxruntime.{VERSION}.dylib", path], check=True)
        subprocess.run(["codesign", "--force", "--sign", "-", path], check=True)
        return path.read_bytes()


def entries(platform, original, nupkg):
    """[(path, bytes | ('link', target), executable)] for one platform."""
    out = [(name, data, False) for name, data in original.items()
           if not name.startswith("lib/") or platform == "linux-aarch64"]
    if platform == "linux-aarch64":
        out += [
            (f"lib/libonnxruntime.so.{VERSION}",
             native(nupkg, "linux-arm64", "libonnxruntime.so"), True),
            ("lib/libonnxruntime.so.1", ("link", f"libonnxruntime.so.{VERSION}"), False),
            ("lib/libonnxruntime.so", ("link", "libonnxruntime.so.1"), False),
            ("lib/libonnxruntime_providers_shared.so",
             native(nupkg, "linux-arm64", "libonnxruntime_providers_shared.so"), True),
        ]
    elif platform == "osx-arm64":
        out += [
            (f"lib/libonnxruntime.{VERSION}.dylib",
             macos_dylib(native(nupkg, "osx-arm64", "libonnxruntime.dylib")), True),
            ("lib/libonnxruntime.dylib", ("link", f"libonnxruntime.{VERSION}.dylib"), False),
        ]
    elif platform == "win-x64":
        for name in ("onnxruntime.dll", "onnxruntime.lib",
                     "onnxruntime_providers_shared.dll", "onnxruntime_providers_shared.lib"):
            out.append((f"lib/{name}", native(nupkg, "win-x64", name), False))
    return sorted(out)


def directories(paths):
    dirs = set()
    for path in paths:
        parts = path.split("/")[:-1]
        for i in range(1, len(parts) + 1):
            dirs.add("/".join(parts[:i]))
    return sorted(dirs)


def write_tgz(target, top, items):
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w", format=tarfile.GNU_FORMAT) as archive:
        def add(info, data=None):
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            info.mtime = MTIME
            archive.addfile(info, io.BytesIO(data) if data is not None else None)

        for name in [""] + directories(p for p, _, _ in items):
            info = tarfile.TarInfo(f"{top}/{name}".rstrip("/"))
            info.type, info.mode = tarfile.DIRTYPE, 0o755
            add(info)
        for name, data, executable in items:
            info = tarfile.TarInfo(f"{top}/{name}")
            if isinstance(data, tuple):
                info.type, info.linkname, info.mode = tarfile.SYMTYPE, data[1], 0o777
                add(info)
            else:
                info.size, info.mode = len(data), 0o755 if executable else 0o644
                add(info, data)
    with open(target, "wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0, compresslevel=9) as gz:
            gz.write(buffer.getvalue())


def write_zip(target, top, items):
    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name in directories(p for p, _, _ in items):
            info = zipfile.ZipInfo(f"{top}/{name}/", ZIP_DATE)
            info.external_attr = (0o40755 << 16) | 0x10
            archive.writestr(info, b"")
        for name, data, _ in items:
            info = zipfile.ZipInfo(f"{top}/{name}", ZIP_DATE)
            info.external_attr = 0o100644 << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, data)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--nupkg", type=Path, required=True)
    parser.add_argument("--linux-x64-tgz", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--platform", action="append",
                        choices=("linux-aarch64", "osx-arm64", "win-x64"))
    args = parser.parse_args()
    check_pin(args.nupkg, NUPKG_SHA256)
    check_pin(args.linux_x64_tgz, LINUX_X64_SHA256)
    original = original_tree(args.linux_x64_tgz)
    args.out.mkdir(parents=True, exist_ok=True)
    for platform in args.platform or ("linux-aarch64", "osx-arm64", "win-x64"):
        top = f"onnxruntime-{platform}-{VERSION}"
        items = entries(platform, original, args.nupkg)
        if platform == "win-x64":
            target = args.out / f"{top}.zip"
            write_zip(target, top, items)
        else:
            target = args.out / f"{top}.tgz"
            write_tgz(target, top, items)
        print(f"{sha256(target)}  {target.name}")


if __name__ == "__main__":
    main()
