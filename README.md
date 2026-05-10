# AetherSDR

**A Linux-native client for FlexRadio Systems transceivers**

[![CI](https://github.com/ten9876/AetherSDR/actions/workflows/ci.yml/badge.svg)](https://github.com/ten9876/AetherSDR/actions/workflows/ci.yml)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![Qt6](https://img.shields.io/badge/Qt-6-green.svg)](https://www.qt.io/)
[![Signed Commits](https://img.shields.io/badge/commits-GPG%20signed-brightgreen?logo=gnuprivacyguard)](https://github.com/ten9876/AetherSDR/commits/main)

AetherSDR brings FlexRadio operation to Linux without Wine or virtual machines. Built from the ground up with Qt6 and C++20, it speaks the SmartSDR protocol natively and aims to replicate the full SmartSDR experience.

**Current version: 0.9.8** | [Download](https://github.com/ten9876/AetherSDR/releases/latest) | [Discussions](https://github.com/ten9876/AetherSDR/discussions) | [What's New](https://github.com/ten9876/AetherSDR/releases)

> **Cross-platform downloads available:** Linux AppImage, macOS universal DMG, Windows installer and portable ZIP.
> Linux is the primary supported platform. macOS and Windows builds are provided as a courtesy.

![AetherSDR Screenshot](docs/screenshot-v5.png)

<p><i>Native. Open. Yours.</i></p>

---

## Highlights

- **GPU-accelerated rendering** — QRhi waterfall + FFT spectrum on GPU (OpenGL/Metal/D3D11), 71% CPU reduction, heat map FFT display
- **Multi-slice operation** — color-coded VFO overlays, independent TX assignment, diversity mode with ESC beamforming
- **Multi-panadapter** — up to 8 pans (FLEX-6700) with 6 layout options, detachable pop-out windows, per-pan display controls, native VITA-49 waterfall tiles
- **Full RX/TX controls** — filter presets, custom filter edges, AGC, DSP, EQ, mic/compression gauges, ATU, TUNE/MOX
- **Aetherial Audio Channel Strip** — unified RX **and** TX DSP suite (gate, EQ, compressor, de-esser, tube, AetherVoice exciter, Freeverb on TX, plus brickwall limiter on TX / output trim on RX) with savable preset library and a wall-clock-accurate scope on each side
- **Client-side noise reduction** — NR2 (spectral), RN2 (RNNoise neural), NR4 (libspecbleach), DFNR (DeepFilterNet3 neural), BNR (NVIDIA GPU AI denoiser), MNR (macOS MMSE-Wiener)
- **AetherSweep** — in-panadapter SWR analyzer with log scale, threshold-band shading, interpolated bandwidth at SWR ≤ 1.5 / 2.0
- **Network Diagnostics** — per-metric trend graphs, packet loss / RTT / jitter metrics, live log tail
- **Memory channels + profiles** — memory bank with quick-recall, global / mic / TX profile management synced with the radio
- **CW operator suite** — real-time Morse decoder with auto pitch/speed detection, keyboard + MIDI-mapped straight key and iambic paddles with full break-in / QSK respect, optional Apollo-era Quindar tones on PTT
- **SpotHub** — DX Cluster, RBN, WSJT-X, POTA, and FreeDV Reporter spots with density badges, auto-mode switch, and auto-reconnect
- **DAX virtual audio** — 4 RX + 1 TX channels for WSJT-X, fldigi, VARA, JS8Call (Linux PipeWire native / PulseAudio, macOS CoreAudio; Windows via TCI / external bridges)
- **DAX IQ streaming** — raw I/Q to SDR apps at 24/48/96/192 kHz
- **SmartLink remote operation** — Auth0 login, TLS command channel, WAN UDP streaming with credential persistence
- **TCI server** — full TCI v2.0 protocol over WebSocket: CAT + audio + IQ + CW + spots in one connection
- **CAT control** — 4-channel rigctld TCP + virtual serial ports, CW macros for contest loggers
- **MIDI controller mapping** — Learn mode, 50+ parameters, named profiles
- **FlexControl USB tuning knob** — auto-detect, acceleration, configurable buttons
- **Serial PTT/CW keying** — USB-serial DTR/RTS for external keyers and foot switches
- **FreeDV RADE** — AI-based digital voice codec with client-side neural encoder/decoder
- **4o3a Tuner Genius XL** — relay control, autotune, 3x1 antenna switch, SWR/power gauges
- **Multi-Flex** — independent operation alongside SmartSDR/Maestro with clickable dashboard
- **Adaptive predistortion (APD)** — SmartSignal toggle and status display for FLEX-8000 series, including **External APD** sampler-port selection (firmware 4.2.18+) for training APD against an external linear amplifier's output

---

## Supported Radios

Works with any FlexRadio transceiver: FLEX-6300, 6400, 6400M, 6500, 6600, 6600M, 6700, 8400, 8400M, 8600, 8600M, Aurora (AU-510, AU-520), and ML- / CL- / RT-series. Active test target is FLEX-8600 firmware 4.2.18 (SmartSDR protocol v1.4.0.0); earlier 4.x firmware works; v3.x is unsupported.

---

## Download

Pre-built binaries are available from [Releases](https://github.com/ten9876/AetherSDR/releases/latest):

| Platform | Download | Notes |
|----------|----------|-------|
| **Linux x86_64** | `AetherSDR-*-x86_64.AppImage` | Single file, no install needed. `chmod +x` and run. |
| **Linux ARM** | `AetherSDR-*-aarch64.AppImage` | Raspberry Pi, ARM laptops. `chmod +x` and run. |
| **macOS** | `AetherSDR-*-macOS-apple-silicon.dmg` | Apple Silicon (M1+). Intel Macs via Rosetta. Signed & notarized. |
| **Windows Installer** | `AetherSDR-*-Windows-x64-setup.exe` | Setup wizard with Start Menu shortcut and uninstaller. |
| **Windows Portable** | `AetherSDR-*-Windows-x64-portable.zip` | No install needed. Extract and run. |

---


### Qt 6.7+ for GPU Spectrum Rendering

GPU-accelerated spectrum/waterfall rendering requires Qt 6.7 or greater. If your distribution ships with an older version (e.g., Ubuntu 24.04, Debian 12, or Mint 21-22 include Qt 6.4.2), the build system automatically disables GPU rendering and falls back to the CPU-based `QPainter` path.

To use GPU acceleration on these systems, you must install Qt 6.7+ manually:

1. **Option 1: Using PPA (Ubuntu/Mint)**
   You may be able to use the `kubuntu-backports` PPA to obtain a newer Qt version.

2. **Option 2: Using the Qt Online Installer**
   Install the newer Qt version into your home directory (e.g., `~/Qt/6.7.3/gcc_64`). Because CMake often defaults to the system-provided Qt, you must explicitly point CMake to the newer version using the `-DCMAKE_PREFIX_PATH` flag:

   ```bash
   cmake -B build -G Ninja \
       -DCMAKE_PREFIX_PATH="$HOME/Qt/6.7.3/gcc_64" \
       -DCMAKE_BUILD_TYPE=RelWithDebInfo
   ```

*Note: Ensure you have installed the necessary development headers (e.g., `qt6-base-private-dev` equivalent for your installed Qt version).*

### Building from Source


### Dependencies

Install all dependencies for a full-featured build. Optional packages are noted — the build succeeds without them but the corresponding features are disabled.

```bash
# Arch / CachyOS / Manjaro
sudo pacman -S qt6-base qt6-multimedia qt6-websockets qt6-serialport \
  qt6-shadertools cmake ninja pkgconf autoconf automake libtool \
  fftw portaudio hidapi qtkeychain-qt6

# Ubuntu 24.04+ / Debian / Linux Mint
sudo apt install qt6-base-dev qt6-base-private-dev qt6-multimedia-dev \
  qt6-websockets-dev qt6-serialport-dev qt6-shader-baker qt6-shadertools-dev \
  cmake ninja-build pkg-config autoconf automake libtool \
  libfftw3-dev portaudio19-dev libhidapi-dev qtkeychain-qt6-dev \
  libxkbcommon-dev libopengl0 \
  gstreamer1.0-pulseaudio gstreamer1.0-plugins-base

# Fedora
sudo dnf install qt6-qtbase-devel qt6-qtbase-private-devel qt6-qtmultimedia-devel \
  qt6-qtwebsockets-devel qt6-qtserialport-devel qt6-qtshadertools-devel \
  cmake ninja-build autoconf automake libtool \
  fftw3-devel portaudio-devel hidapi-devel qtkeychain-qt6-devel

# macOS (Homebrew)
brew install qt@6 ninja cmake pkgconf autoconf automake libtool \
  fftw portaudio hidapi qtkeychain
```

<details>
<summary>What each dependency enables</summary>

| Package | Feature |
|---------|---------|
| qt6-base, qt6-multimedia | Core application (required) |
| qt6-base-private-dev | GPU-accelerated spectrum/waterfall (QRhi) |
| qt6-shadertools-dev | GPU shader compilation |
| qt6-websockets-dev | TCI server, FreeDV Reporter spots |
| qt6-serialport-dev | FlexControl, serial PTT/CW, MIDI controllers |
| libfftw3-dev | NR2 spectral noise reduction |
| portaudio19-dev | PortAudio audio backend |
| libhidapi-dev | USB HID encoders (RC-28, PowerMate, FlexControl) |
| qtkeychain-qt6-dev | SmartLink credential persistence |
| libopengl0 | GLVND-split desktop OpenGL runtime (GPU spectrum/waterfall) |

</details>

> **Linux Mint / Ubuntu note:** If PC audio devices show as "Dummy Output",
> install `gstreamer1.0-pulseaudio`. For PipeWire systems, also install `gstreamer1.0-pipewire`.
>
> **Ubuntu 26.04 note:** If AetherSDR fails to start with a missing
> `libOpenGL.so.0` error, install `libopengl0`.  26.04 stopped pulling it in
> by default for the desktop image; the build-deps line above includes it
> explicitly so this only bites users who install just the AppImage.

### Build & Run

```bash
git clone https://github.com/ten9876/AetherSDR.git
cd AetherSDR
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j$(nproc)
./build/AetherSDR
```

RADE-enabled builds use a vendored Opus snapshot, so no additional Opus download
is required during configure or build.

### Install (optional, Linux)

```bash
sudo cmake --install build
```

---

## Roadmap

- [ ] CW ultimatic keyer mode (#416)
- [ ] Native DAX audio channels on Windows
- [ ] Stream Deck plugin (in development)

See the full [issue tracker](https://github.com/ten9876/AetherSDR/issues) for all planned features.

---

## Contributing

PRs, bug reports, and feature requests welcome! See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

**Development environment:** AetherSDR is developed using [Claude Code](https://claude.com/claude-code) as the primary development tool. We encourage contributors to use Claude Code for consistency. PRs must follow project conventions, pass CI, and include GPG-signed commits.

**Not a developer?** Click the lightbulb button in AetherSDR's title bar to create an AI-assisted bug report or feature request.

---

## Verifying Downloads

Linux and Windows binaries are GPG-signed. macOS artifacts are Apple notarized. Each release includes `.asc` signatures and `SHA256SUMS.txt`.

```bash
curl -sSL https://raw.githubusercontent.com/ten9876/AetherSDR/main/docs/RELEASE-SIGNING-KEY.pub.asc | gpg --import
gpg --verify AetherSDR-vX.Y.Z-x86_64.AppImage.asc AetherSDR-vX.Y.Z-x86_64.AppImage
```

See [docs/VERIFYING-RELEASES.md](docs/VERIFYING-RELEASES.md) for full instructions.

---

## License

AetherSDR is free and open-source software licensed under the [GNU General Public License v3](LICENSE).

*AetherSDR is an independent project and is not affiliated with or endorsed by FlexRadio Systems.*
