# Building AetherSDR

Platform setup, troubleshooting and reference detail for building from source.
The quick start — dependencies and the build itself — is in
[`README.md`](../README.md#building-from-source). Everything here is what you
need only on a specific platform, or when something goes wrong.

- [macOS: Qt and qtkeychain](#macos-qt-and-qtkeychain)
- [Windows 11](#windows-11)
- [What each dependency enables](#what-each-dependency-enables)
- [Distro notes](#distro-notes)
- [Older distro Qt (Ubuntu 24.04 LTS)](#older-distro-qt-ubuntu-2404-lts)
- [GPU spectrum rendering](#gpu-spectrum-rendering)
- [Wayland and XWayland](#wayland-and-xwayland)

---

## macOS: Qt and qtkeychain

Qt and qtkeychain do **not** come from Homebrew. Homebrew's `qt`
formula (aliased `qt6` and `qt@6`) is a *rolling* release — 6.11.2 at the time
of writing — while the DMG ships 6.12.0 LTS like every other artifact. Building
against Homebrew's Qt means testing a Qt no release ships. Install the matching
one and point CMake at it:

```bash
# A venv rather than a bare `pip install`: a PEP 668 python3 refuses the latter.
python3 -m venv ~/.venv/aqt && ~/.venv/aqt/bin/pip install aqtinstall
~/.venv/aqt/bin/aqt install-qt mac desktop 6.12.0 clang_64 \
  -m qtmultimedia qtwebsockets qtserialport qtshadertools \
  --outputdir ~/Qt
cmake -B build -DCMAKE_PREFIX_PATH="$HOME/Qt/6.12.0/macos;$(brew --prefix)"
```

`clang_64` is the only macOS desktop build Qt publishes, and it is universal2 —
there is no separate arm64 archive to pick. `$(brew --prefix)` stays on the
path for fftw, librtlsdr, portaudio and hidapi.

Homebrew's `qtkeychain` is left out for a related reason: the formula depends
on `qtbase`, so installing it pulls a second Qt in behind your back. Build it
against the Qt you just installed instead — or skip it and build without
SmartLink credential persistence:

```bash
CMAKE_PREFIX_PATH="$HOME/Qt/6.12.0/macos" bash scripts/setup/setup-qtkeychain.sh
```

**Two Qt installations visible to CMake at once is a real failure, not a
theoretical one** — it is what #711 and #812 were, and `CMakeLists.txt` puts
`$(brew --prefix)/include` on the global include path on macOS, so a Homebrew
Qt is discoverable whether or not you asked for it. If you have one,
`brew uninstall qt` (plus whatever pulled it in) before building. The release
workflow asserts this; your machine will not.

---

## Windows 11

Prerequisites: Visual Studio 2022 (Build Tools, Community, or higher) with the
MSVC C++ workload, CMake 3.25+, Ninja, and Qt 6.8+ (`msvc2022_64`; both CI and
the release binaries use 6.12.0 LTS).

```bat
:: 1. Activate the MSVC environment. Adjust the edition (BuildTools / Community /
::    Professional / Enterprise) to match your install; run "vswhere" if unsure.
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

:: 2. Point at your Qt kit once, with forward slashes (CMake reads the path
::    literally, so backslashes would be taken as escape sequences). Change the
::    version/edition here to match your install; both steps below reuse it.
::    setup-qtkeychain.ps1 (step 4) reads QT_ROOT_DIR; on CI that variable is
::    exported by install-qt-action, so a local build has to set it explicitly
::    or the script exits with "Qt not found".
set "QT_KIT=C:/Qt/6.12.0/msvc2022_64"
set "QT_ROOT_DIR=%QT_KIT%"

:: 3. Generate the single-precision FFTW import lib (needed by NR4/libspecbleach)
powershell -File scripts\setup\setup-fftw.ps1

:: 4. Build qtkeychain (needed for QRZ/SmartLink credential persistence).
::    Downloads source and builds it against your Qt kit into third_party\qtkeychain\.
::    Skip this step and the build still succeeds, but QRZ/SmartLink passwords
::    won't be saved between runs.
powershell -File scripts\setup\setup-qtkeychain.ps1

:: 5. Configure. Ninja is required: the default Visual Studio generator is
::    multi-config (it ignores CMAKE_BUILD_TYPE) and takes a different
::    manifest-embed path. Point CMAKE_PREFIX_PATH at your Qt kit so
::    find_package(Qt6) resolves.
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_PREFIX_PATH="%QT_KIT%"

:: 6. Build
cmake --build build --target AetherSDR
```

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

## Older distro Qt (Ubuntu 24.04 LTS)

On a distribution whose Qt is older than the required 6.8 (notably Ubuntu 24.04
LTS at 6.4.2), install a newer Qt manually:

1. **Option 1: Using a PPA (Ubuntu/Mint)**
   The `kubuntu-backports` PPA may provide a newer Qt — verify the version it ships before relying on it.

2. **Option 2: Using the Qt Online Installer**
   Install Qt into your home directory (e.g., `~/Qt/6.12.0/gcc_64`). Because CMake otherwise defaults to the system-provided Qt, point it at the newer install with `-DCMAKE_PREFIX_PATH`:

   ```bash
   cmake -B build -G Ninja \
       -DCMAKE_PREFIX_PATH="$HOME/Qt/6.12.0/gcc_64" \
       -DCMAKE_BUILD_TYPE=RelWithDebInfo
   ```

   Make sure the `qtshadertools` and `qt5compat` (or equivalent) modules are selected in the Qt Online Installer along with `qtbase`.

*Note: GPU rendering also needs the private QtGui headers (`qt6-base-private-dev` on Debian-family, included by default in the Qt Online Installer).*

---

## GPU spectrum rendering

GPU-accelerated spectrum/waterfall rendering requires Qt 6.7 or greater (`QRhiWidget`). Since the build now requires Qt 6.8 as a minimum, no build is held back by the Qt version any more — the aarch64 AppImage included. What decides whether a given binary renders via QRhi is the `AETHER_GPU_SPECTRUM` build option, and for a source build whether Qt's private GUI headers are installed: CMake turns the option off with `GPU spectrum rendering disabled — Qt6GuiPrivate not found` when they are missing (install `qt6-base-private-dev` / `qt6-qtbase-private-devel`).

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
