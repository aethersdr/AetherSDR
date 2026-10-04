# Building AetherSDR

Platform setup, troubleshooting and reference detail for building from source.
The quick start — dependencies and the build itself — is in
[`README.md`](../README.md#building-from-source). Everything here is what you
need only on a specific platform, or when something goes wrong.

Compile-time switches, defaults and prerequisites are listed in
[BUILD-OPTIONS.md](../BUILD-OPTIONS.md).

- [macOS: Qt and qtkeychain](#macos-qt-and-qtkeychain)
- [Windows 11](#windows-11)
- [What each dependency enables](#what-each-dependency-enables)
- [Distro notes](#distro-notes)
- [The release Qt: `setup-qt.sh`](#the-release-qt-setup-qtsh)
- [GPU spectrum rendering](#gpu-spectrum-rendering)
- [Wayland and XWayland](#wayland-and-xwayland)

---

## macOS: Qt and qtkeychain

Qt and qtkeychain do **not** come from Homebrew. Homebrew's `qt`
formula (aliased `qt6` and `qt@6`) is a *rolling* release — 6.11.2 at the time
of writing — while the DMG ships 6.12.0 LTS like every other artifact. Building
against Homebrew's Qt means testing a Qt no release ships. Install the matching
one with [`scripts/setup/setup-qt.sh`](#the-release-qt-setup-qtsh), which also
builds qtkeychain against it, then configure as usual:

```bash
scripts/setup/setup-qt.sh
cmake -B build -G Ninja \
  -DCMAKE_PREFIX_PATH="$(scripts/setup/setup-qt.sh --print-prefix);$(brew --prefix)"
```

The pinned Qt goes first in that list on purpose. `$(brew --prefix)` is there
for fftw, librtlsdr, portaudio and hidapi, but a Homebrew `qt` formula (often
pulled in by something else) lives under the same prefix; naming the pinned Qt
first keeps it the one CMake finds.

Qt 6.12 needs **Xcode 16** (the macOS 15 SDK): Qt's own CMake stops at
configure with "Qt requires at least version 16 of Xcode" on anything older,
and the script checks before it downloads. Xcode 16 itself needs a macOS 14.5+
host, so a Mac below macOS 14.5 cannot build AetherSDR from source — use the
DMG if it runs there. Anything built against 6.12 runs on macOS 14.4+ only.

`clang_64` is the only macOS desktop build Qt publishes, and it is universal2 —
there is no separate arm64 archive to pick. `$(brew --prefix)` stays on the
path for fftw, librtlsdr, portaudio and hidapi.

Homebrew's `qtkeychain` is left out for a related reason: the formula depends
on `qtbase`, so installing it pulls a second Qt in behind your back.
`setup-qt.sh` builds qtkeychain against the pinned Qt instead
(`--no-keychain` skips it, at the cost of SmartLink credential persistence).

**Two Qt installations visible to CMake at once is a real failure, not a
theoretical one** — it is what #711 and #812 were, and `CMakeLists.txt` puts
`$(brew --prefix)/include` on the global include path on macOS, so a Homebrew
Qt is discoverable whether or not you asked for it. If you have one,
`brew uninstall qt` (plus whatever pulled it in) before building. The release
workflow asserts this; your machine will not.

---

## Windows 11

Prerequisites: Visual Studio 2022 **17.14 or newer** (Build Tools, Community,
or higher) with the MSVC C++ workload, CMake 3.25+, Ninja, Git, and Python 3.
7-Zip is recommended. The 17.14 floor comes from Qt 6.12 itself: its static
`Qt6EntryPoint.lib`, which every Windows GUI app links, is built by MSVC 14.44,
and an MSVC linker must be at least as new as the compiler behind any input.

Qt 6.12 is the last Qt release that supports Windows 10 (1809 or later), so
the next binary Qt bump will make AetherSDR's Windows builds Windows 11-only.

```bat
:: 1. Activate the MSVC environment. Adjust the edition (BuildTools / Community /
::    Professional / Enterprise) to match your install; run "vswhere" if unsure.
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

:: 2. Install the release Qt and build qtkeychain against it. Checks Visual
::    Studio, Python, disk space and the Qt build before downloading ~2 GB into
::    %LOCALAPPDATA%\aethersdr\qt\ (AETHER_QT_CACHE overrides). Re-running is a
::    no-op once installed.
powershell -File scripts\setup\setup-qt.ps1

:: 3. Generate the single-precision FFTW import lib (needed by NR4/libspecbleach)
powershell -File scripts\setup\setup-fftw.ps1

:: 4. Configure. CMake finds the Qt from step 2 on its own. Ninja is required:
::    the default Visual Studio generator is multi-config (it ignores
::    CMAKE_BUILD_TYPE) and takes a different manifest-embed path.
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo

:: 5. Build
cmake --build build --target AetherSDR
```

**Why a script and not "install Qt with aqt":** the newest aqtinstall on PyPI
(3.3.0) cannot install Qt 6.11 or newer on Windows — it stops with *Failed to
locate XML data for Qt version*, because Qt moved its Windows repository to one
index per architecture. `setup-qt.ps1` installs aqt from the commit CI uses
(`AQTINSTALL_GIT_REF` in [`cmake/qt-pin.env`](../cmake/qt-pin.env)), and
extracts with 7-Zip because aqt's built-in extractor fails at random on Windows
Qt archives.

**Using a Qt you installed yourself** (6.12 or newer, e.g. from the Qt Online
Installer, which needs a Qt account): skip step 2, then point both qtkeychain
and CMake at the kit, with
forward slashes (CMake reads the path literally):

```bat
set "QT_ROOT_DIR=C:/Qt/6.12.0/msvc2022_64"
powershell -File scripts\setup\setup-qtkeychain.ps1
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_PREFIX_PATH="%QT_ROOT_DIR%"
```

An explicit `CMAKE_PREFIX_PATH` always wins over the cached release Qt;
`-DAETHER_USE_PINNED_QT=OFF` ignores the cache outright.

---

## What each dependency enables

| Package | Feature |
|---------|---------|
| qt6-base, qt6-multimedia | Core application (required) |
| qt6-base-private-dev | GPU-accelerated spectrum/waterfall (QRhi) |
| qt6-shadertools-dev | GPU shader compilation |
| qt6-websockets-dev | TCI server, FreeDV Reporter spots |
| qt6-serialport-dev | FlexControl, serial PTT/CW, MIDI controllers |
| libfftw3-dev | NR2 spectral noise reduction |
| librtlsdr-dev | RTL-SDR USB receiver backend (optional) |
| portaudio19-dev | PortAudio audio backend |
| libhidapi-dev | USB HID encoders (RC-28, PowerMate, FlexControl) |
| qtkeychain-qt6-dev | SmartLink credential persistence |
| libopengl0 | GLVND-split desktop OpenGL runtime (GPU spectrum/waterfall) |

## Distro notes

**Linux Mint / Ubuntu note:** If PC audio devices show as "Dummy Output",
install `gstreamer1.0-pulseaudio`. For PipeWire systems, also install `gstreamer1.0-pipewire`.

**Ubuntu 26.04 note:** If AetherSDR fails to start with a missing
`libOpenGL.so.0` error, install `libopengl0`.  26.04 stopped pulling it in
by default for the desktop image; the build-deps line above includes it
explicitly so this only bites users who install just the AppImage.

## The release Qt: `setup-qt.sh`

AetherSDR requires Qt 6.12, the Qt every release is built against, and few
distros package it yet (Debian Trixie ships 6.8, Ubuntu 24.04 6.4, Arch and
Debian sid 6.11 at the time of writing). Install the pinned release Qt with
one command:

```bash
scripts/setup/setup-qt.sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

The script reads [`cmake/qt-pin.env`](../cmake/qt-pin.env) — the same pin
every CI leg and release workflow is checked against — installs that Qt with
a pinned aqtinstall, and builds qtkeychain against it. CMake then finds it on
its own; no `CMAKE_PREFIX_PATH` needed. Expect ~2 GB on disk and well under a
minute on a fast connection.

- **Where it goes:** `~/.cache/aethersdr/qt/` on Linux,
  `~/Library/Caches/aethersdr/qt/` on macOS, shared by every checkout. Set
  `AETHER_QT_CACHE` to put it elsewhere. Each install is a generation under
  `gen/`, and `<version>-<revision>.current` names the live one. A reinstall
  builds the new generation alongside and switches the pointer in one atomic
  step, so an interrupted or failed reinstall leaves the working Qt in place;
  existing build directories follow the pointer on their next configure.
  Superseded generations are kept, because built binaries link Qt by absolute
  path and must keep launching until rebuilt; `--prune` (`-Prune` on Windows)
  deletes every generation but the live one when you want the ~2 GB back.
- **What it checks first**, so an unsupported machine is told before the
  download rather than after: glibc 2.34+ (x86_64) or 2.38+ (aarch64), Xcode
  16+ on macOS, a working `python3 -m venv` (on Debian, Ubuntu and Raspberry Pi
  OS: `sudo apt install python3-venv`), ~3 GB free, and that Qt's repository
  still serves the exact build the pin names.
- **Re-running** is a no-op once installed. `--print-prefix` prints the Qt
  path CMake will use.
- **Using another Qt:** any Qt 6.12+ works — a distro's, once it ships one, or
  a Qt Online Installer kit. Pass `-DAETHER_USE_PINNED_QT=OFF`, or point
  `CMAKE_PREFIX_PATH`/`Qt6_DIR` at it; an explicit choice always wins.
- **Let CMake run it:** `-DAETHER_FETCH_QT=ON` runs the script at configure
  time when the Qt is missing. Off by default — a plain configure should never
  start a 2 GB download.
- **An existing build directory** remembers the Qt it first found; reconfigure
  with `cmake --fresh -B build` after installing.

Qt's binaries also need the X11/xcb, GL and PulseAudio libraries a distro Qt
would have pulled in; the README's per-distro install lines include them, and
`.github/docker/Dockerfile` is the set CI builds with.

On Windows, use `setup-qt.ps1` instead — see [Windows 11](#windows-11).

*Note: GPU rendering also needs the private QtGui headers. The release Qt and the Qt Online Installer include them; a distro Qt needs its private-headers package (`qt6-base-private-dev` on Debian-family).*

---

## GPU spectrum rendering

GPU-accelerated spectrum/waterfall rendering requires Qt 6.7 or greater (`QRhiWidget`). The build requires Qt 6.12, so no build is held back by the Qt version — the aarch64 AppImage included. What decides whether a given binary renders via QRhi is the `AETHER_GPU_SPECTRUM` build option, and for a source build whether Qt's private GUI headers are installed: CMake turns the option off with `GPU spectrum rendering disabled — Qt6GuiPrivate not found` when they are missing (install `qt6-base-private-dev` / `qt6-qtbase-private-devel`).

The CPU `QPainter` path is a **build-time alternative, not a runtime fallback**. `AETHER_GPU_SPECTRUM` selects `SpectrumWidget`'s base class — `QRhiWidget` or `QWidget` — and `SpectrumWidget::paintEvent()`, which is what draws the spectrum on the CPU, is compiled only into the `QWidget` build. (A GPU build still uses `QPainter`, but only to rasterise overlays into textures QRhi then composites.) Of the shipped artifacts only the Intel macOS DMG is built the other way, and deliberately: `QRhiWidget` misbehaves on older Metal/OpenGL hardware.

Having no GPU is usually a non-event, because in practice "no GPU" means a software rasterizer rather than nothing. QRhi comes up on whatever the platform provides — llvmpipe or softpipe (Mesa), WARP or Microsoft Basic Render (D3D11), SwiftShader — and the app detects it and says so: **Help ▸ About** shows a `Renderer:` line reading `CPU QRhi (…)` rather than `GPU QRhi (…)`, naming the backend and device. Rendering is correct, just slow.

If QRhi cannot initialise at all — no usable GL/D3D/Metal, as on a headless host, in some VMs, or behind a broken driver — there is nothing to fall back to. The spectrum does not draw, and the failure is reported by Qt rather than by AetherSDR: the log records `QRhiWidget: QRhi is not supported on this platform.` or `QRhiWidget: No QRhi`, and `QRhiWidget::renderFailed()` fires with nothing listening, so there is no notice in the UI. The rest of the app (controls, audio, radio I/O) is unaffected.

`AETHER_NO_GPU=1` forces software OpenGL on an already-built binary, without a rebuild:

```bash
AETHER_NO_GPU=1 ./AetherSDR-*.AppImage
```

That is the escape hatch if a GPU or driver renders the spectrum incorrectly — worth trying first on Raspberry Pi and other systems whose Mesa driver is newer than its hardware.

Trace thickness is the **FFT Line** slider under the spectrum's right-click **Display** panel (Off to 5.0 px, per panadapter).

---

## Wayland and XWayland

On a Wayland session AetherSDR chooses the Qt platform based on whether a
display is attached:

- **A display is connected** → `wayland;xcb` (native Wayland when the platform
  plugin is available, XWayland otherwise). Native Wayland avoids the GLX
  `BadAccess` crash that XWayland can produce when opening child dialogs on some
  compositors, and renders correctly under fractional scaling instead of being
  bitmap-scaled by the compositor.
- **Headless** — no connected display, e.g. a remote Raspberry Pi reached over
  VNC — → `xcb;wayland`. With no DRM scanout, native-Wayland hardware GL cannot
  allocate a window surface and the spectrum renders black under an
  `EGL_BAD_MATCH` error storm; XWayland allocates its buffers through the X
  server and works. AetherSDR detects this from the DRM connector status and
  flips the order automatically; the chosen platform is recorded at startup in
  the log (`Platform: Wayland session, display presence …`).

Setting `QT_QPA_PLATFORM` yourself always wins — override in either direction:

```bash
QT_QPA_PLATFORM=xcb ./AetherSDR-*.AppImage            # force XWayland
QT_QPA_PLATFORM='wayland;xcb' ./AetherSDR-*.AppImage  # force native Wayland
```

The second form is the way back to native Wayland on a headless session whose
XWayland mishandles child dialogs (the GLX `BadAccess` above) — the automatic
choice there is `xcb;wayland`, so you would otherwise be on XWayland.
