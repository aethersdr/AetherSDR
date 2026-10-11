---
title: "macOS"
slug: "/macos"
description: "AetherSDR runs natively on macOS, with separate signed and notarized DMGs for Apple Silicon and Intel Macs."
---

AetherSDR runs natively on macOS, with separate signed and notarized DMGs for Apple Silicon and Intel Macs. This page collects everything that is specific to macOS: installing, the features that differ from Linux and Windows, where your files live, and fixes for macOS-only problems. Everything else in these docs applies to macOS as written, with **Cmd** in place of **Ctrl** in shortcuts.

## Requirements

| | Apple Silicon | Intel |
|---|---|---|
| **Download** | `AetherSDR-*-macOS-apple-silicon.dmg` | `AetherSDR-*-macOS-intel.dmg` |
| **macOS** | 14.4 (Sonoma) or newer | 14.4 (Sonoma) or newer. Intel Macs that cannot run Sonoma cannot run current releases. |
| **Panadapter rendering** | GPU (Metal) | CPU renderer, deliberately, because the GPU path misbehaves on older Metal/OpenGL hardware |
| **Copy Assist (speech-to-text)** | Included, with Metal GPU acceleration | **Not included** in the DMG. Building from source on an Intel Mac still works. |

## Setup

### Installing the DMG

1. Download the DMG for your Mac from [GitHub Releases](https://github.com/aethersdr/AetherSDR/releases/latest).
2. Open it and drag **AetherSDR** to **Applications**.
3. Start AetherSDR from Applications. Both DMGs are signed and notarized by Apple, so Gatekeeper verifies them with no extra steps.
4. The **Connect to Radio** window opens. Continue with [First Connection](./first-connection.md), or try [Your First Session](./your-first-session.md) with no radio.

### Installing the DAX audio driver

[DAX Virtual Audio](./dax-virtual-audio.md) on macOS uses the **DAX Virtual Audio Driver** that ships in the AetherSDR DMG. Install it from the DMG before you enable DAX. Without it, enabling DAX shows **DAX Audio Driver Missing**.

### Allowing Input Monitoring

USB and Bluetooth HID controllers need macOS **Input Monitoring** permission. The first time AetherSDR opens one, macOS asks for it. Allow AetherSDR in **System Settings → Privacy & Security → Input Monitoring**.

This applies to the [Ulanzi Dial](./ulanzi-dial.md), the Stream Deck+ ([StreamDeck](./streamdeck.md)) and the HID encoders in [USB Control Surfaces](./usb-control-surfaces.md) (RC-28, PowerMate, Contour Shuttle, TMate 2). HID encoders are off by default in **Serial & Controllers** so that the prompt never appears unless you own one, and the Ulanzi Dial asks only once a dial is attached.

## Using AetherSDR on macOS

### What is different on macOS

- **Menus** are in the system menu bar at the top of the screen. See [Menu Reference](./menu-reference.md).
- **Radio Setup** is **AetherSDR → Preferences...** (Settings → Radio Setup... on the other platforms). Search in it with ⌘F.
- **Window:** a native window with the standard traffic-light buttons, the tiling menu, Stage Manager and native full screen (Cmd+Ctrl+F). Cmd+M minimises the window.
- **DAX virtual audio** is built in through the DAX Virtual Audio Driver (above). See [DAX Virtual Audio](./dax-virtual-audio.md).
- **CAT virtual serial ports** (PTY) are available as well as TCP ports, at `~/Library/Caches/AetherSDR/cat-A` … `cat-H`. See [CAT Control](./cat-control.md).
- **Noise reduction:** **MNR**, Apple's MMSE-Wiener spectral noise reduction on the vDSP/Accelerate framework, is available only on macOS. **BNR** (NVIDIA) is not available; use **DFNR** instead, which runs on any CPU. See [DSP Noise Mitigation](./dsp-noise-mitigation.md).
- **Copy Assist** uses the **Metal** GPU on Apple Silicon. It is not in the Intel DMG. See [Copy Assist](./copy-assist.md).
- **Credentials** are kept in the macOS Keychain, never in the settings database.
- **ThumbDV** appears as `/dev/cu.usbserial-*`. See [D-STAR (ThumbDV)](./d-star-thumbdv.md).
- **Screen reader:** VoiceOver. AetherSDR holds animations still when **Reduce motion** is on. See [Accessibility](./accessibility.md).

## Reference

### Where files live

| What | Location |
|---|---|
| Settings database (`AetherSDR.db`), backups, quarantine | `~/Library/Preferences/AetherSDR/` |
| Application logs and support bundles | `~/Library/Preferences/AetherSDR/logs/` (newest log also linked as `aethersdr.log`) |
| NR2 FFTW wisdom | `~/.config/AetherSDR/aethersdr_fftw_wisdom` |
| Copy Assist models | `~/Library/Application Support/AetherSDR/models/` |
| CAT virtual serial ports | `~/Library/Caches/AetherSDR/cat-A` … `cat-H` |
| Crash reports | **Console → Crash Reports**, or `~/Library/Logs/DiagnosticReports/` |

To use the settings command line, run the program inside the app bundle, for example `/Applications/AetherSDR.app/Contents/MacOS/AetherSDR --config path`. See [Settings and Backups](./settings-and-backups.md) and [Support and Logging](./support-and-logging.md).

## Known issues

- Selecting the **Audio** page in **Preferences** can push the Preferences window behind the main window ([#6277](https://github.com/aethersdr/AetherSDR/issues/6277)).
- Without Input Monitoring permission, a Ulanzi Dial shows only "Disconnected", with no hint that a permission is missing ([#5247](https://github.com/aethersdr/AetherSDR/issues/5247)).
- DAX transmit through the macOS DAX driver can send audio in bursts, and can stop sending until AetherSDR is restarted; TCI is not affected ([#5870](https://github.com/aethersdr/AetherSDR/issues/5870)).
- Receive audio can become distorted after several minutes on some external audio interfaces until **PC Audio** is toggled ([#6160](https://github.com/aethersdr/AetherSDR/issues/6160)).
- Copy Assist can freeze the whole application for 15–20 seconds at a time on recent Apple Silicon Macs ([#5107](https://github.com/aethersdr/AetherSDR/issues/5107)).
- An RTL-SDR Blog V4 dongle shows only noise, with no signals ([#6016](https://github.com/aethersdr/AetherSDR/issues/6016)).

## Troubleshooting

### Enabling DAX shows "DAX Audio Driver Missing"

The DAX Virtual Audio Driver is not installed on this Mac.

1. Open the AetherSDR DMG again.
2. Install the **DAX Virtual Audio Driver** from it.
3. Enable DAX again.

### A USB knob, dial or Stream Deck+ does nothing

AetherSDR does not have Input Monitoring permission, or HID encoders are switched off.

1. Open **System Settings → Privacy & Security → Input Monitoring** and allow AetherSDR.
2. For the RC-28, PowerMate, Contour Shuttle, TMate 2 or Stream Deck+, open **AetherSDR → Preferences... → Serial & Controllers** and turn on **Enable HID encoders / StreamDeck+ (RC-28, PowerMate, ShuttleXpress, …)**.
3. Unplug the device and plug it back in.

### Copy Assist is missing

You are running the Intel DMG, which ships without speech-to-text.

1. On an Apple Silicon Mac, install the Apple Silicon DMG.
2. On an Intel Mac, build AetherSDR from source to get Copy Assist. See [Building from Source](./building-from-source.md).

### BNR is not offered

BNR needs an NVIDIA GPU and is not available on macOS.

1. Choose **DFNR** in AetherRX's **AetherNR** tab instead, or **MNR** for a native one-knob option. See [DSP Noise Mitigation](./dsp-noise-mitigation.md).

### A stored setting stops AetherSDR from starting

1. Quit AetherSDR.
2. In Terminal, run `/Applications/AetherSDR.app/Contents/MacOS/AetherSDR --config list` to find the value.
3. Remove it with `--config unset <key>`. See [Settings and Backups](./settings-and-backups.md).

### Reporting a crash

1. Open **Console → Crash Reports**, or `~/Library/Logs/DiagnosticReports/`.
2. Attach the newest `AetherSDR-*.ips` file to your report from **Help → File an Issue...**. It names your user folder in file paths; edit that out if you prefer.

See [Troubleshooting](./troubleshooting.md) and [docs/debugging-crashes.md](https://github.com/aethersdr/AetherSDR/blob/main/docs/debugging-crashes.md).

## See also

- [Installation](./installation.md)
- [DAX Virtual Audio](./dax-virtual-audio.md)
- [DSP Noise Mitigation](./dsp-noise-mitigation.md)
- [USB Control Surfaces](./usb-control-surfaces.md)
- [Settings and Backups](./settings-and-backups.md)
- [Troubleshooting](./troubleshooting.md)
