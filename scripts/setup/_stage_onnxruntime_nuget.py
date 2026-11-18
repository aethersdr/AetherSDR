#!/usr/bin/env python3
"""Stage the pinned official NuGet runtime when a release asset is unavailable."""

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import tempfile
from urllib.request import urlopen
import zipfile


VERSION = "1.27.0"
PACKAGE = f"microsoft.ml.onnxruntime.{VERSION}.nupkg"
URL = f"https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime/{VERSION}/{PACKAGE}"
SHA256 = "c725e8cefdfe4de08befb9f79b3f218c162439cdcdb97cbbb5192888d58ea669"
MAX_BYTES = 160 * 1024 * 1024


def verify(path):
    if path.stat().st_size > MAX_BYTES:
        raise ValueError("ONNX Runtime package exceeds the download limit")
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    if digest.hexdigest() != SHA256:
        raise ValueError("ONNX Runtime NuGet package SHA256 mismatch")


def download(cache):
    cache.mkdir(parents=True, exist_ok=True)
    path = cache / PACKAGE
    if path.exists():
        verify(path)
        return path
    stream = tempfile.NamedTemporaryFile(dir=cache, prefix=PACKAGE, suffix=".part", delete=False)
    partial = Path(stream.name)
    try:
        with stream:
            with urlopen(URL, timeout=30) as response:
                if response.status != 200 or not response.url.startswith("https://"):
                    raise ValueError("ONNX Runtime download did not return HTTPS success")
                count = 0
                while chunk := response.read(1024 * 1024):
                    count += len(chunk)
                    if count > MAX_BYTES:
                        raise ValueError("ONNX Runtime package exceeds the download limit")
                    stream.write(chunk)
        verify(partial)
        os.replace(partial, path)
    finally:
        partial.unlink(missing_ok=True)
    return path


def stage(package, platform, destination):
    verify(package)
    if platform not in ("win-x64", "osx-arm64"):
        raise ValueError("Unsupported ONNX Runtime NuGet platform")
    destination = destination.absolute()
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="onnxruntime-stage-", dir=destination.parent) as temporary:
        root = Path(temporary)
        prepared = root / "prepared"
        (prepared / "include").mkdir(parents=True)
        (prepared / "lib").mkdir()
        with zipfile.ZipFile(package) as archive:
            # Copy only selected entries into fixed paths; never extract archive paths.
            def copy(member, target):
                entry = archive.getinfo(member)
                if entry.file_size > MAX_BYTES:
                    raise ValueError("ONNX Runtime archive member exceeds the size limit")
                target.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(entry) as source, target.open("wb") as output:
                    shutil.copyfileobj(source, output)

            prefix = "build/native/include/"
            for name in archive.namelist():
                leaf = name.removeprefix(prefix)
                if name.startswith(prefix) and "/" not in leaf and "\\" not in leaf and leaf.endswith(".h"):
                    copy(name, prepared / "include" / leaf)
            for name in ("onnxruntime_cxx_api.h", "onnxruntime_cxx_inline.h", "onnxruntime_c_api.h"):
                if not (prepared / "include" / name).is_file():
                    raise ValueError("ONNX Runtime package is missing required headers")
            for name in ("LICENSE", "ThirdPartyNotices.txt"):
                copy(name, prepared / name)
            native = f"runtimes/{platform}/native/"
            if platform == "win-x64":
                copy(native + "onnxruntime.lib", prepared / "lib/onnxruntime.lib")
                for name in ("onnxruntime.dll", "onnxruntime_providers_shared.dll"):
                    copy(native + name, prepared / "bin" / name)
            else:
                versioned = f"libonnxruntime.{VERSION}.dylib"
                copy(native + "libonnxruntime.dylib", prepared / "lib" / versioned)
                (prepared / "lib/libonnxruntime.dylib").symlink_to(versioned)
                (prepared / "lib/libonnxruntime.1.dylib").symlink_to(versioned)
        previous = root / "previous"
        if destination.exists():
            os.replace(destination, previous)
        try:
            os.replace(prepared, destination)
        except OSError:
            if previous.exists():
                os.replace(previous, destination)
            raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform", choices=("win-x64", "osx-arm64"), required=True)
    parser.add_argument("--destination", type=Path, required=True)
    parser.add_argument("--cache", type=Path, required=True)
    args = parser.parse_args()
    try:
        stage(download(args.cache), args.platform, args.destination)
    except (OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        parser.exit(1, f"ONNX Runtime NuGet fallback failed: {error}\n")
    print(f"ONNX Runtime {VERSION} ({args.platform}) staged from verified official NuGet package")


if __name__ == "__main__":
    main()
