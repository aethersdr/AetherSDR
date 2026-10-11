---
title: "Linux"
slug: "/linux"
description: "AetherSDR runs natively on Linux as a single-file AppImage, for x86_64 PCs and for aarch64 machines such as the Raspberry Pi."
---

AetherSDR runs natively on Linux as a single-file AppImage, for x86_64 PCs and for aarch64 machines such as the Raspberry Pi. This page collects everything that is specific to Linux: installing, the features that differ from macOS and Windows, where your files live, and fixes for Linux-only problems. Everything else in these docs applies to Linux as written.

## Requirements

| | x86_64 | aarch64 (Raspberry Pi, ARM laptops) |
|---|---|---|
| **Download** | `AetherSDR-*-x86_64.AppImage` | `AetherSDR-*-aarch64.AppImage` |
| **Minimum system** | Built on an Ubuntu 22.04 base, so it runs on that release and newer distributions | **glibc 2.38 or newer**. **Raspberry Pi OS Trixie** is the supported Pi baseline; Raspberry Pi OS **Bookworm** (glibc 2.36) cannot run it. |
| **Graphics** | OpenGL (Mesa or vendor drivers) | OpenGL; the 3D view works on the Raspberry Pi 5 |

The AppImage bundles Qt and AetherSDR's other libraries, so nothing else needs installing. Some features need a little help from the system:

- **PipeWire** for [DAX Virtual Audio](./dax-virtual-audio.md).
- **A desktop keyring** (for example GNOME Keyring or KWallet) to remember passwords and tokens between sessions. See [Settings and Backups](./settings-and-backups.md).
- **udev rules** for USB devices such as HID encoders and the Stream Deck+ (see [Setup](#setup)).

## Setup

### Installing the AppImage

1. Download the AppImage for your machine from [GitHub Releases](https://github.com/aethersdr/AetherSDR/releases/latest).
2. Make it executable and run it:

   ```bash
   chmod +x AetherSDR-*-x86_64.AppImage
   ./AetherSDR-*-x86_64.AppImage
   ```

3. The **Connect to Radio** window opens. Continue with [First Connection](./first-connection.md), or try [Your First Session](./your-first-session.md) with no radio.

To check the download is genuine, verify its GPG signature as described in [Installation](./installation.md).

The AppImage is self-contained. If you build from source instead and want an application-menu entry and icon, `sudo cmake --install build` installs the binary, `.desktop` file and icon. See [Building from Source](./building-from-source.md).

### Wayland and XWayland

The AppImage runs **natively on Wayland**, which renders sharply under fractional scaling. If native Wayland is not available it falls back to XWayland automatically. On a headless Wayland session with no monitor attached (for example a Raspberry Pi reached over VNC), it starts on XWayland (xcb), because native Wayland cannot render the panadapter there.

Set `QT_QPA_PLATFORM` yourself to override the choice. Your setting always wins:

```bash
QT_QPA_PLATFORM=xcb ./AetherSDR-*.AppImage            # force XWayland
QT_QPA_PLATFORM='wayland;xcb' ./AetherSDR-*.AppImage  # force native Wayland
```

### USB device permissions (udev)

Most USB accessories are root-only on Linux until a udev rule or group membership gives your user access.

| Device | What to do | Page |
|---|---|---|
| RC-28, PowerMate, ShuttleXpress, ShuttlePro v2, Stream Deck+ | Copy `packaging/linux/60-hid-encoders.rules` from the source tree to `/etc/udev/rules.d/`, then run `sudo udevadm control --reload-rules && sudo udevadm trigger` | [USB Control Surfaces](./usb-control-surfaces.md), [StreamDeck](./streamdeck.md) |
| Ulanzi Dial | Click **Grant access** in the mapper when it shows "… detected — needs permission". AetherSDR installs `70-ulanzi-dial.rules` through `pkexec`. | [Ulanzi Dial](./ulanzi-dial.md) |
| CTR2-Max (USB mode) | **Start** offers to install the CTR2 udev rule if the device cannot be opened | [CTR2 Proxy](./ctr2-proxy.md) |
| ThumbDV / DV3000U | Official packages install `70-aethersdr-thumbdv.rules`; otherwise join the serial group (below) | [D-STAR (ThumbDV)](./d-star-thumbdv.md) |
| FlexControl knob, USB-serial PTT/CW interfaces | Add yourself to the serial-port group: `uucp` on Arch, `dialout` on Debian/Ubuntu/Fedora | [FlexControl Tuning Knob](./flexcontrol-tuning-knob.md) |

After changing your groups, log out and back in.

## Using AetherSDR on Linux

### What is different on Linux

- **Menus** are behind the **☰** button at the left of the title bar. Alt+letter mnemonics (Alt+F, Alt+S, …) still open each menu. See [Menu Reference](./menu-reference.md).
- **Radio Setup** is **Settings → Radio Setup...**.
- **Window frame:** AetherSDR draws a frameless window with a 6 px resize band on the sides and bottom. **View → Frameless Window** (Ctrl+Shift+F) turns it off if your compositor mishandles it.
- **DAX virtual audio** is built in through PipeWire. Receive channels are native PipeWire streams, and they also appear through PipeWire's PulseAudio and JACK layers. DAX IQ channels appear as capture devices named **AetherSDR DAX IQ n**. See [DAX Virtual Audio](./dax-virtual-audio.md) and [DAX IQ Streaming](./dax-iq-streaming.md).
- **CAT virtual serial ports** (PTY) are available as well as TCP ports, at `$XDG_RUNTIME_DIR/aethersdr/cat-A` … `cat-H`. See [CAT Control](./cat-control.md).
- **Noise reduction:** **BNR** (NVIDIA Maxine, on an NVIDIA RTX/GeForce GPU) is available on Linux. **MNR** is macOS only and is greyed out. See [DSP Noise Mitigation](./dsp-noise-mitigation.md) and [BNR GPU Noise Removal](./bnr-gpu-noise-removal.md).
- **Copy Assist** can use a **Vulkan** GPU (NVIDIA, AMD or Intel). On a Raspberry Pi the **base** model is the default, and builds that include sherpa-onnx can run faster non-whisper models. See [Copy Assist](./copy-assist.md).
- **Credentials** are kept in the desktop keyring (libsecret or KWallet), never in the settings database.
- **Screen reader:** Orca. See [Accessibility](./accessibility.md).

### Software rendering

If the panadapter draws incorrectly on your GPU or driver, force software OpenGL without rebuilding:

```bash
AETHER_NO_GPU=1 ./AetherSDR-*.AppImage
```

This is worth trying first on a Raspberry Pi whose Mesa driver is newer than its hardware. See [GPU Rendering](./gpu-rendering.md).

## Reference

### Where files live

| What | Location |
|---|---|
| Settings database (`AetherSDR.db`), backups, quarantine | `~/.config/AetherSDR/` |
| Application logs and support bundles | `~/.config/AetherSDR/logs/` (newest log also linked as `aethersdr.log`) |
| SpotHub source logs | `~/.config/AetherSDR/spothub/` |
| User themes | `~/.config/AetherSDR/themes/` |
| NR2 FFTW wisdom | `~/.config/AetherSDR/aethersdr_fftw_wisdom` |
| Copy Assist models | `~/.local/share/AetherSDR/models/` |
| BNR NVIDIA runtime | `~/.local/share/AetherSDR/AetherSDR/nvidia-afx/current/` |
| CAT virtual serial ports | `$XDG_RUNTIME_DIR/aethersdr/cat-A` … `cat-H` |

`AetherSDR --config path` prints the exact database path. The `AETHER_SETTINGS_DIR` environment variable points AetherSDR at a different configuration folder. See [Settings and Backups](./settings-and-backups.md) and [Support and Logging](./support-and-logging.md).

### Environment variables

| Variable | Effect |
|---|---|
| `QT_QPA_PLATFORM=xcb` | Force XWayland |
| `QT_QPA_PLATFORM='wayland;xcb'` | Force native Wayland |
| `AETHER_NO_GPU=1` | Software OpenGL for the panadapter |
| `AETHER_SETTINGS_DIR=<dir>` | Use a different configuration folder |
| `QT_LOGGING_RULES="aether.cat.info=true"` | Raise a log category before the GUI opens |

## Known issues

- On Wayland, the GPU spectrum can intermittently freeze the whole window at startup, seen most on a Raspberry Pi 5 ([#4704](https://github.com/aethersdr/AetherSDR/issues/4704)).
- On Ubuntu 26.04 under native Wayland, the panadapter and waterfall can update very slowly ([#4725](https://github.com/aethersdr/AetherSDR/issues/4725)).
- On some X11 desktops with recent NVIDIA cards the AppImage shows no spectrum or waterfall and the Connect to Radio window never appears ([#4998](https://github.com/aethersdr/AetherSDR/issues/4998)).
- On a Raspberry Pi 5 with software rendering, the spectrum heat-map fill can use almost all of the main thread, so the window stops responding to clicks under extra load ([#5192](https://github.com/aethersdr/AetherSDR/issues/5192)).

## Troubleshooting

### AetherSDR will not start: `error while loading shared libraries: libOpenGL.so.0`

Ubuntu 26.04 no longer installs `libopengl0` on the desktop image, and the GPU spectrum links against it.

1. Install it:

   ```bash
   sudo apt install libopengl0
   ```

2. Start AetherSDR again. This is the GLVND desktop OpenGL runtime, not the legacy `libgl1`; both can be installed side by side.

### The ARM AppImage does not run on a Raspberry Pi

The aarch64 AppImage needs glibc 2.38 or newer, and Raspberry Pi OS Bookworm has glibc 2.36.

1. Upgrade the Pi to **Raspberry Pi OS Trixie**.
2. Run the AppImage again.

### A dialog crashes AetherSDR under XWayland (GLX `BadAccess`)

Some compositors produce this XWayland crash when a dialog opens.

1. Run AetherSDR natively on Wayland:

   ```bash
   QT_QPA_PLATFORM='wayland;xcb' ./AetherSDR-*.AppImage
   ```

### The panadapter is black over VNC or remote desktop

Native Wayland cannot render the panadapter on a headless session. AetherSDR detects a Wayland session with no display and switches to XWayland, unless you have set `QT_QPA_PLATFORM` yourself.

1. Remove your own `QT_QPA_PLATFORM` setting, or set `QT_QPA_PLATFORM=xcb`.
2. If the panadapter still draws wrongly, start with `AETHER_NO_GPU=1`.

### PC audio misbehaves on a PipeWire system

The PipeWire library is installed but PipeWire's client configuration is not, so AetherSDR keeps Qt's audio off PipeWire at startup instead of crashing.

1. Install your distribution's PipeWire package: `pipewire` on Arch, `pipewire-bin` on Debian/Ubuntu.
2. Restart AetherSDR.

On source builds for Ubuntu or Linux Mint whose device lists show only "Dummy Output", see the distro notes in [docs/BUILDING.md](https://github.com/aethersdr/AetherSDR/blob/main/docs/BUILDING.md#distro-notes).

### A USB knob, dial or Stream Deck is not found

Your user cannot open the device until a udev rule or group grants access.

1. Install the rule or join the group listed in [USB device permissions](#usb-device-permissions-udev).
2. Unplug the device and plug it back in.
3. If you changed groups, log out and back in (or reboot) before trying again.

### Discovery finds no radio

A firewall is blocking discovery on UDP port 4992, or the radio is on another subnet.

1. Allow discovery through the firewall, for example `sudo ufw allow 4992/udp`.
2. If the radio is on another subnet or behind a VPN, use **Connect by IP**. See [Manual Connection](./manual-connection.md).

### Passwords and tokens are forgotten after every restart

The build has no keychain support, or no desktop keyring is running, so credentials are kept for the current session only.

1. Use the AppImage, which includes keychain support.
2. Make sure a Secret Service keyring (for example GNOME Keyring) or KWallet is running in your session.

### Reporting a crash

1. Run `coredumpctl list AetherSDR`.
2. Run `coredumpctl info <PID> > aethersdr-crash.txt` and attach the text file to your report from **Help → File an Issue...**.
3. Do **not** attach the core file itself: it can contain passwords and tokens.

See [Troubleshooting](./troubleshooting.md) and [docs/debugging-crashes.md](https://github.com/aethersdr/AetherSDR/blob/main/docs/debugging-crashes.md).

## See also

- [Installation](./installation.md)
- [GPU Rendering](./gpu-rendering.md)
- [DAX Virtual Audio](./dax-virtual-audio.md)
- [Settings and Backups](./settings-and-backups.md)
- [Support and Logging](./support-and-logging.md)
- [Troubleshooting](./troubleshooting.md)
- [docs/BUILDING.md](https://github.com/aethersdr/AetherSDR/blob/main/docs/BUILDING.md)
