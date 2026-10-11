---
title: "Building from Source"
slug: "/building-from-source"
description: "Most operators should use the pre-built downloads on Installation."
---

Most operators should use the pre-built downloads on [Installation](./installation.md). Build from source if you want to test a branch, develop AetherSDR, or package it for a distribution.

The authoritative instructions are in the repository and are kept current with every change. This page is a summary:

- [README: Building from Source](https://github.com/aethersdr/AetherSDR#building-from-source): dependency lines and the quick build
- [docs/BUILDING.md](https://github.com/aethersdr/AetherSDR/blob/main/docs/BUILDING.md): Windows 11 and macOS step-by-step, what each dependency enables, distro notes, GPU rendering and Wayland
- [BUILD-OPTIONS.md](https://github.com/aethersdr/AetherSDR/blob/main/BUILD-OPTIONS.md): every compile-time switch, its default and its prerequisites

## Requirements

### Qt 6.12

Every source build needs **Qt 6.12**, the same Qt every release is built with. Few distributions package it yet, so the repository provides a script that installs exactly the release Qt plus qtkeychain into a per-user cache that CMake finds on its own:

- Linux and macOS: `scripts/setup/setup-qt.sh`
- Windows: `scripts\setup\setup-qt.ps1`

The script checks your machine before downloading (about 2 GB): glibc 2.34+ on x86_64 or 2.38+ on aarch64, Xcode 16+ on macOS, a working `python3 -m venv` (on Debian, Ubuntu and Raspberry Pi OS: `sudo apt install python3-venv`) and enough free disk space. Re-running it is a no-op once Qt is installed. A distribution Qt that is already 6.12 or newer also works.

Machines the release Qt cannot support cannot build AetherSDR from source: Linux below those glibc versions, CPU architectures Qt publishes no binaries for, and Macs below macOS 14.5 (Xcode 16 needs 14.5).

### Dependencies

Everything except Qt and qtkeychain comes from the system. Copy the current line for your distribution from the [README](https://github.com/aethersdr/AetherSDR#dependencies). It covers Arch / CachyOS / Manjaro, Debian / Ubuntu / Linux Mint, Fedora and macOS (Homebrew), and includes FFTW, RTL-SDR, PortAudio, hidapi, PipeWire and the X11/xcb libraries Qt's binaries need.

Optional libraries enable optional features; the build succeeds without them and the corresponding feature is left out. [docs/BUILDING.md](https://github.com/aethersdr/AetherSDR/blob/main/docs/BUILDING.md#what-each-dependency-enables) has the dependency-to-feature table.

On macOS, do not also install Homebrew's `qt`. Two Qt installations visible to CMake at once break the build.

### Windows toolchain

You need Visual Studio 2022 **17.14 or newer** with the C++ workload, CMake 3.25+, Ninja, Git and Python 3.

## Setup

### Build on Linux and macOS

```bash
git clone https://github.com/aethersdr/AetherSDR.git
cd AetherSDR
scripts/setup/setup-qt.sh            # Qt 6.12 (cached per user) + qtkeychain
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j$(nproc)
./build/AetherSDR
```

### Build on Windows 11

From an MSVC developer prompt:

```bat
powershell -File scripts\setup\setup-qt.ps1
powershell -File scripts\setup\setup-fftw.ps1
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target AetherSDR
```

See [docs/BUILDING.md § Windows 11](https://github.com/aethersdr/AetherSDR/blob/main/docs/BUILDING.md#windows-11) for the full sequence, including the optional dependency scripts.

### Install (optional, Linux)

```bash
sudo cmake --install build
```

This installs the binary, a desktop entry and the icon.

## Developing AetherSDR

AetherSDR is C++20 and Qt 6. Key source directories:

- `src/core/`: protocol, audio, networking and the radio backends
- `src/models/`: radio, slice, meter and transmit models
- `src/gui/`: widgets, applets and dialogs

Start with [AGENTS.md](https://github.com/aethersdr/AetherSDR/blob/main/AGENTS.md), the canonical project guide, and [docs/DEVELOPER-GUIDE.md](https://github.com/aethersdr/AetherSDR/blob/main/docs/DEVELOPER-GUIDE.md) for architecture and conventions. See [Contributing Guide](./contributing-guide.md) before opening a pull request.

## Reference

### Build options

[BUILD-OPTIONS.md](https://github.com/aethersdr/AetherSDR/blob/main/BUILD-OPTIONS.md) lists every switch and is checked against the build files, so it stays accurate. A few you may want:

| Option | Default | Purpose |
|--------|---------|---------|
| `CMAKE_BUILD_TYPE` | — | `RelWithDebInfo` for normal use, `Debug` for development |
| `AETHER_GPU_SPECTRUM` | ON | QRhi GPU spectrum and waterfall. Needs Qt's private GUI headers; without them the CPU renderer is built. |
| `ENABLE_RTL` | ON | Experimental receive-only RTL-SDR backend, when `librtlsdr` and single-precision FFTW are found |
| `ENABLE_ASR` | ON | Copy Assist on-device speech-to-text |
| `ENABLE_NVIDIA_AFX` | ON | BNR NVIDIA GPU noise removal (x86-64 Linux and Windows) |
| `AETHER_FETCH_QT` | OFF | Run the Qt setup script during configure if the pinned Qt is missing |
| `USE_SYSTEM_*` | OFF | For distribution packagers: use system zlib, libmspack, libmosquitto, RtMidi, libwhisper or SQLite instead of the bundled copies |
| `LOWER_CASE_BINARY_NAME` | OFF | Lower-case executable name on Linux |

Builds honour `SOURCE_DATE_EPOCH` for reproducible output.

## Troubleshooting

### The build still uses an old Qt after you installed Qt 6.12

An existing build directory remembers the Qt it first found.

1. Reconfigure from scratch: `cmake --fresh -B build`.
2. Build again.

### `setup-qt.sh` stops because `python3 -m venv` does not work

Debian, Ubuntu and Raspberry Pi OS ship Python without the `venv` module.

1. Run `sudo apt install python3-venv`.
2. Run `scripts/setup/setup-qt.sh` again.

### The macOS build fails with two Qt installations

Homebrew's `qt` is installed alongside the Qt from `setup-qt.sh`, and CMake sees both.

1. Remove Homebrew's Qt: `brew uninstall qt`.
2. Reconfigure with `cmake --fresh -B build` and build again.

## See also

- [Installation](./installation.md)
- [Contributing Guide](./contributing-guide.md)
- [AI-Assisted Development](./ai-assisted-development.md)
- [GPU Rendering](./gpu-rendering.md)
- [docs/BUILDING.md](https://github.com/aethersdr/AetherSDR/blob/main/docs/BUILDING.md)
- [BUILD-OPTIONS.md](https://github.com/aethersdr/AetherSDR/blob/main/BUILD-OPTIONS.md)
