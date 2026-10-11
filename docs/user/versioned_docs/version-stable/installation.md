---
title: "Installation"
slug: "/installation"
description: "AetherSDR ships native builds for Linux, macOS and Windows."
---

AetherSDR ships native builds for Linux, macOS and Windows. Every platform is built, tested in CI and released together, from the same source.

Download the latest release from [GitHub Releases](https://github.com/aethersdr/AetherSDR/releases/latest). The pre-built binaries are the recommended way to install. To build it yourself, see [Building from Source](./building-from-source.md).

For everything about running AetherSDR on one operating system, see the platform pages: [Linux](./linux.md), [macOS](./macos.md) and [Windows](./windows.md).

## Requirements

| Platform | File | Notes |
|----------|------|-------|
| **Linux x86_64** | `AetherSDR-*-x86_64.AppImage` | Single file, no install. `chmod +x` and run. |
| **Linux ARM** | `AetherSDR-*-aarch64.AppImage` | Raspberry Pi and ARM laptops. `chmod +x` and run. |
| **macOS Apple Silicon** | `AetherSDR-*-macOS-apple-silicon.dmg` | M1 and later, macOS 14.4 or newer. Signed and notarized. |
| **macOS Intel** | `AetherSDR-*-macOS-intel.dmg` | Intel Macs, macOS 14.4 (Sonoma) or newer. Signed and notarized. |
| **Windows Installer** | `AetherSDR-*-Windows-x64-setup.exe` | Setup wizard with Start Menu shortcut and uninstaller. |
| **Windows Portable** | `AetherSDR-*-Windows-x64-portable.zip` | No install. Extract and run `AetherSDR.exe`. |

All release binaries are built with Qt 6.12.

- **Linux x86_64:** the AppImage is built on an Ubuntu 22.04 base, so it runs on that release and on newer distributions.
- **Linux aarch64 (Raspberry Pi):** requires **glibc 2.38 or newer**. **Raspberry Pi OS Trixie** is the supported Pi baseline. Raspberry Pi OS **Bookworm** (glibc 2.36) cannot run it. The ARM build has GPU spectrum rendering like the other platforms.
- **macOS:** Apple Silicon and Intel have separate DMGs. Both need **macOS 14.4 (Sonoma) or newer**; Intel Macs that cannot run Sonoma cannot run current releases.

## Setup

### Linux

More on running AetherSDR on Linux: [Linux](./linux.md).

```bash
chmod +x AetherSDR-*-x86_64.AppImage
./AetherSDR-*-x86_64.AppImage
```

The AppImage bundles Qt and AetherSDR's other libraries; nothing else needs installing.

The AppImage is self-contained. If you built from source and want an application-menu entry and icon, `sudo cmake --install build` installs the binary, `.desktop` file and icon. See [Building from Source](./building-from-source.md).

#### Wayland and XWayland

The AppImage runs **natively on Wayland**, which renders sharply under fractional scaling and avoids an XWayland crash some compositors produce when opening dialogs. If native Wayland is not available it falls back to XWayland automatically. On a headless Wayland session with no monitor attached (for example a Raspberry Pi reached over VNC), AetherSDR starts on XWayland (xcb) automatically, because native Wayland cannot render the panadapter there.

To override the choice, set `QT_QPA_PLATFORM` yourself. Your setting always wins:

```bash
QT_QPA_PLATFORM=xcb ./AetherSDR-*.AppImage            # force XWayland
QT_QPA_PLATFORM='wayland;xcb' ./AetherSDR-*.AppImage  # force native Wayland
```

### macOS

More on running AetherSDR on a Mac: [macOS](./macos.md).

Open the DMG for your Mac and drag AetherSDR to **Applications**. Both DMGs are signed and notarized by Apple, so Gatekeeper verifies them with no extra steps.

The **Intel DMG has no Copy Assist** (speech-to-text), and it draws the panadapter with the CPU renderer rather than the GPU. The Apple Silicon DMG has both. See [Copy Assist](./copy-assist.md) and [GPU Rendering](./gpu-rendering.md).

### Windows

More on running AetherSDR on Windows: [Windows](./windows.md).

Choose either:

- **Installer** (`…-setup.exe`): adds a Start Menu shortcut and an uninstaller.
- **Portable** (`…-portable.zip`): extract anywhere and run `AetherSDR.exe`.

Both carry the Microsoft Visual C++ runtime with the application. **No Visual C++ Redistributable install is needed**, and repairing or removing the system redistributable does not affect AetherSDR.

The Windows builds are Authenticode-signed: the setup, `AetherSDR.exe` and the other AetherSDR files by Jeremy Fielder, and bundled components such as Qt and the Microsoft Visual C++ runtime by their own makers. The downloads are also GPG-signed. SmartScreen may still warn about a new release until it has built download reputation; verifying the signature (below) confirms the download is genuine.

Your settings live in `%LOCALAPPDATA%\AetherSDR\` whichever package you use. See [Settings and Backups](./settings-and-backups.md).

## After installing

- [Your First Session](./your-first-session.md): a guided first run
- [First Connection](./first-connection.md): connect to your radio
- [Demo Mode](./demo-mode.md): try AetherSDR with no radio
- [Settings and Backups](./settings-and-backups.md): where your settings are kept

## Reference

### Verifying downloads

Linux and Windows downloads are GPG-signed, and each release includes `.asc` signatures and a signed `SHA256SUMS.txt`. macOS DMGs are Apple-notarized.

```bash
curl -sSL https://raw.githubusercontent.com/aethersdr/AetherSDR/main/docs/RELEASE-SIGNING-KEY.pub.asc | gpg --import
gpg --verify AetherSDR-vX.Y.Z-x86_64.AppImage.asc AetherSDR-vX.Y.Z-x86_64.AppImage
```

The expected result is **"Good signature from AetherSDR Release Signing"**. Full instructions, including the key fingerprint and Windows steps, are in [docs/VERIFYING-RELEASES.md](https://github.com/aethersdr/AetherSDR/blob/main/docs/VERIFYING-RELEASES.md).

### Debug symbols

Each release also carries per-platform debug-symbol archives (`…-symbols.tar.xz`). You do not need them to run AetherSDR; they let developers read crash reports. See [Support and Logging](./support-and-logging.md).

## Known issues

- On Ubuntu 26.04 under native Wayland, the AppImage's panadapter and waterfall can update very slowly ([#4725](https://github.com/aethersdr/AetherSDR/issues/4725)).
- On Linux under native Wayland, the GPU spectrum can occasionally freeze the whole window at startup, seen on a Raspberry Pi 5 ([#4704](https://github.com/aethersdr/AetherSDR/issues/4704)).
- With the software (CPU) spectrum renderer on a Raspberry Pi 5, painting the spectrum takes nearly all of the interface thread, so the window can stop responding to clicks under extra load ([#5192](https://github.com/aethersdr/AetherSDR/issues/5192)).

## Troubleshooting

### AetherSDR won't start: `error while loading shared libraries: libOpenGL.so.0`

Ubuntu 26.04 no longer installs `libopengl0` by default, and AetherSDR's GPU spectrum links against it.

1. Run `sudo apt install libopengl0`.
2. Start AetherSDR again.

### The ARM AppImage won't run on a Raspberry Pi

Raspberry Pi OS Bookworm has glibc 2.36, and the aarch64 AppImage needs glibc 2.38 or newer.

1. Check your glibc version with `ldd --version`.
2. Move to Raspberry Pi OS Trixie, the supported Pi baseline.

### The panadapter draws incorrectly

Your GPU driver does not render AetherSDR's GPU spectrum correctly. This is worth checking first on a Raspberry Pi whose Mesa driver is newer than its hardware.

1. Start AetherSDR with software OpenGL; no rebuild is needed: `AETHER_NO_GPU=1 ./AetherSDR-*.AppImage`.
2. See [GPU Rendering](./gpu-rendering.md) for details.

### PC audio misbehaves on a PipeWire system

The PipeWire library is installed but PipeWire's client configuration is not. AetherSDR then keeps Qt's audio backend off PipeWire at startup instead of crashing.

1. Install your distribution's PipeWire package: `pipewire` on Arch, `pipewire-bin` on Debian/Ubuntu.
2. Restart AetherSDR.

### Windows SmartScreen warns the first time you run AetherSDR

The Windows builds are Authenticode-signed (publisher: Jeremy Fielder), but SmartScreen can still warn about a new release until it has built download reputation.

1. Check the publisher: right-click the setup `.exe` or `AetherSDR.exe`, choose **Properties → Digital Signatures**, and confirm the signer is Jeremy Fielder. You can also verify the download's signature as described in [Verifying downloads](#verifying-downloads) and [docs/VERIFYING-RELEASES.md](https://github.com/aethersdr/AetherSDR/blob/main/docs/VERIFYING-RELEASES.md).
2. If the signature is good, the download is genuine and you can let it run.

## See also

- [Linux](./linux.md)
- [macOS](./macos.md)
- [Windows](./windows.md)
- [Building from Source](./building-from-source.md)
- [Troubleshooting](./troubleshooting.md)
- [GitHub Releases](https://github.com/aethersdr/AetherSDR/releases/latest)
