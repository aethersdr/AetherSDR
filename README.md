# AetherSDR

**A Linux-native client for FlexRadio Systems transceivers**

[![CI](https://github.com/aethersdr/AetherSDR/actions/workflows/ci.yml/badge.svg)](https://github.com/aethersdr/AetherSDR/actions/workflows/ci.yml)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![Qt6](https://img.shields.io/badge/Qt-6-green.svg)](https://www.qt.io/)
[![Signed Commits](https://img.shields.io/badge/commits-GPG%20signed-brightgreen?logo=gnuprivacyguard)](https://github.com/aethersdr/AetherSDR/commits/main)

AetherSDR brings FlexRadio operation to Linux without Wine or virtual machines. Built from the ground up with Qt6 and C++20, it speaks the SmartSDR protocol natively and aims to replicate the full SmartSDR experience.

**Current version: 26.5.2.1** — CalVer (`YY.M.patch[.hotfix]`). | [Download](https://github.com/aethersdr/AetherSDR/releases/latest) | [Discussions](https://github.com/aethersdr/AetherSDR/discussions) | [What's New](https://github.com/aethersdr/AetherSDR/releases)

> **Cross-platform downloads available:** Linux AppImage, macOS universal DMG, Windows installer and portable ZIP.
> Linux is the primary supported platform. macOS and Windows builds are provided as a courtesy.

![AetherSDR Screenshot](docs/assets/screenshot-current.png)

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

## How AetherSDR Is Built

AetherSDR is developed using an AI-augmented open-source workflow:

- **Project lead (Jeremy KK7GWY) + a core contributor team** working primarily through Claude Code and a mix of AI development tools — every commit goes through the merge gate; nothing reaches `main` without human review
- **[AetherClaude](https://github.com/aethersdr/aetherclaude) orchestrator bot** auto-triages incoming issues, drafts implementation plans, and produces PRs for issues labelled `aetherclaude-eligible`
- **Contributors use a mix of AI tools** (Codex, Copilot, Cursor, Gemini, Aider) — the project's [Constitution](CONSTITUTION.md) (14 principles, structured per [Cisco's Foundry Constitution](https://github.com/CiscoDevNet/foundry-security-spec) spec) codifies the conventions every contributor and every AI tool follows
- **Branch protection enforces signed commits, CI green, and CODEOWNERS review** — every change goes through the same gate regardless of which AI tool (or human) produced it
- **At active pace: ~50 PRs per week, ~15,000–30,000 lifetime downloads, ≥6 distinct AI tools touching the codebase**

See [`AGENTS.md`](AGENTS.md) for the canonical project guide that every AI assistant reads first, and [`CONSTITUTION.md`](CONSTITUTION.md) for the principles that gate the contribution model.

The full list of code contributors is auto-generated from GitHub commit attribution — see the [Contributors graph](https://github.com/aethersdr/AetherSDR/graphs/contributors).

---

## Supported Hardware

Works with any FlexRadio transceiver, including:

- FLEX-6000 series: FLEX-6300, FLEX-6400, FLEX-6400M, FLEX-6500, FLEX-6600, FLEX-6600M, FLEX-6700
- FLEX-8000 series: FLEX-8400, FLEX-8400M, FLEX-8600, FLEX-8600M
- Aurora series: AU-510, AU-510M, AU-520, AU-520M
- ML-, CL-, and RT-series devices

Supported external devices include the 4O3A/FlexRadio PGXL (Power Genius XL)
power amplifier and TGXL (Tuner Genius XL) antenna tuner.

Active test target is FLEX-8600 firmware 4.2.18 (SmartSDR protocol v1.4.0.0);
earlier 4.x firmware works; v3.x is unsupported.

## Tested Controller Devices

AetherSDR supports external station-control hardware through USB serial, USB HID,
MIDI, Stream Deck/StreamController plugins, and generic USB-serial adapters:

- FlexRadio FlexControl USB tuning knob
- Icom RC-28 USB remote encoder
- Griffin PowerMate USB knob
- Contour ShuttleXpress and ShuttlePro v2 jog controllers
- MIDI controllers with learn mode, profiles, and relative-encoder support
- Elgato Stream Deck devices through the bundled macOS/Windows Stream Deck plugin
- Stream Deck devices on Linux through the bundled StreamController plugin
- USB-serial PTT/CW interfaces for foot switches, straight keys, iambic paddles,
  amplifier keying lines, and external sequencers

---

## Download

Pre-built binaries are available from [Releases](https://github.com/aethersdr/AetherSDR/releases/latest):

| Platform | Download | Notes |
|----------|----------|-------|
| **Linux x86_64** | `AetherSDR-*-x86_64.AppImage` | Single file, no install needed. `chmod +x` and run. |
| **Linux ARM** | `AetherSDR-*-aarch64.AppImage` | Raspberry Pi, ARM laptops. `chmod +x` and run. |
| **macOS** | `AetherSDR-*-macOS-apple-silicon.dmg` | Apple Silicon (M1+). Intel Macs via Rosetta. Signed & notarized. |
| **Windows Installer** | `AetherSDR-*-Windows-x64-setup.exe` | Setup wizard with Start Menu shortcut and uninstaller. |
| **Windows Portable** | `AetherSDR-*-Windows-x64-portable.zip` | No install needed. Extract and run. |

---

## Building from Source

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
git clone https://github.com/aethersdr/AetherSDR.git
cd AetherSDR
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j$(nproc)
./build/AetherSDR
```

RADE-enabled builds use a vendored Opus snapshot, so no additional Opus download
is required during configure or build.

### Qt 6.7+ for GPU Spectrum Rendering

GPU-accelerated spectrum/waterfall rendering requires Qt 6.7 or greater. If your distribution ships with an older version (e.g., Ubuntu 24.04, Debian 12, or Mint 21–22 include Qt 6.4.2), the build system automatically disables GPU rendering and falls back to the CPU-based `QPainter` path. (Release binaries ship Qt 6.8.3 LTS; the 6.7 floor is the source-build minimum for QRhi.)

To use GPU acceleration on these systems, install Qt 6.7+ manually:

1. **Option 1: Using a PPA (Ubuntu/Mint)**
   The `kubuntu-backports` PPA may provide a newer Qt — verify the version it ships before relying on it.

2. **Option 2: Using the Qt Online Installer**
   Install Qt into your home directory (e.g., `~/Qt/6.8.3/gcc_64`). Because CMake otherwise defaults to the system-provided Qt, point it at the newer install with `-DCMAKE_PREFIX_PATH`:

   ```bash
   cmake -B build -G Ninja \
       -DCMAKE_PREFIX_PATH="$HOME/Qt/6.8.3/gcc_64" \
       -DCMAKE_BUILD_TYPE=RelWithDebInfo
   ```

   Make sure the `qtshadertools` and `qt5compat` (or equivalent) modules are selected in the Qt Online Installer along with `qtbase`.

*Note: GPU rendering also needs the private QtGui headers (`qt6-base-private-dev` on Debian-family, included by default in the Qt Online Installer).*

### Install (optional, Linux)

```bash
sudo cmake --install build
```

---

## Roadmap

- [ ] CW ultimatic keyer mode (#416)
- [ ] Native DAX audio channels on Windows

See the full [issue tracker](https://github.com/aethersdr/AetherSDR/issues) for all planned features.

---

## Contributing

PRs, bug reports, and feature requests welcome! See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

**Development environment:** AetherSDR is developed using [Claude Code](https://claude.com/claude-code) as the primary development tool. We encourage contributors to use Claude Code for consistency. PRs must follow project conventions, pass CI, and include GPG-signed commits.

**Not a developer?** Click the lightbulb button in AetherSDR's title bar to create an AI-assisted bug report or feature request.

---

## Verifying Downloads

Linux and Windows binaries are GPG-signed. macOS artifacts are Apple notarized. Each release includes `.asc` signatures and `SHA256SUMS.txt`.

```bash
curl -sSL https://raw.githubusercontent.com/aethersdr/AetherSDR/main/docs/RELEASE-SIGNING-KEY.pub.asc | gpg --import
gpg --verify AetherSDR-vX.Y.Z-x86_64.AppImage.asc AetherSDR-vX.Y.Z-x86_64.AppImage
```

See [docs/VERIFYING-RELEASES.md](docs/VERIFYING-RELEASES.md) for full instructions.

---

## License

AetherSDR is free and open-source software licensed under the [GNU General Public License v3](LICENSE).

*AetherSDR is an independent project and is not affiliated with or endorsed by FlexRadio Systems.*
