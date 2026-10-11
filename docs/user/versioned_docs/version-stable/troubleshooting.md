---
title: "Troubleshooting"
slug: "/troubleshooting"
description: "This page is the symptom index for AetherSDR."
---

This page is the symptom index for AetherSDR. Find the symptom you see below; if it belongs to one feature, the [By feature](#by-feature) list links that feature's own Troubleshooting section. To report a problem that isn't solved here, see [Filing a bug](#filing-a-bug).

Start with the built-in tools:

- **Help → Support & Diagnostics...** for logs. See [Support and Logging](./support-and-logging.md).
- **Tools → Network Diagnostics...** for link problems, and **Tools → Runtime Monitor...** for performance. See [Runtime Monitor](./runtime-monitor.md).
- **Help → Slice Troubleshooting...** walks through common slice problems.
- The [Log Analyzer](/log-analyzer) checks a log or support bundle for known problems and links the fix. It runs in your browser; the file is never uploaded.

## Connecting

### No radio appears under On This Network

Discovery broadcasts are not reaching AetherSDR. The radio may be on a different subnet (broadcasts do not cross routers or most VPNs), a firewall may be blocking UDP port 4992, or the radio may enforce private-IP connections while you are on a different network.

1. Check the radio is reachable: `ping <radio-ip>`.
2. Allow discovery through the firewall, for example `sudo ufw allow 4992/udp`.
3. Use **Connect by IP**: choose the **Radio type**, enter the radio's address and click **Connect by IP**. See [Manual Connection](./manual-connection.md).

### The radio refuses the connection

The radio rejected AetherSDR's GUI-client registration (for example with error `F3000001`). AetherSDR stops, closes the connection, does not retry on its own, and returns to the connect panel showing the radio's error and what to do about it.

1. Read the message in the connect panel and follow it.
2. Typically another client must disconnect first, or the radio needs a restart.

### A Connected Stations dialog appears when you connect

multiFLEX is off on the radio and other clients are already connected.

1. Disconnect one of the stations listed in the **Connected Stations** dialog.
2. See [Multi-Flex](./multi-flex.md).

### SmartLink: signed in, but no radio is listed

The radio is not registered with SmartLink, or port forwarding is not set up.

1. Register the radio with SmartLink.
2. Forward TCP 4994 and UDP 4993 to the radio.
3. See [SmartLink Setup](./smartlink-setup.md).

### SmartLink stops with a certificate mismatch warning

The radio's certificate does not match the one AetherSDR pinned.

1. Read the [certificate pinning](./smartlink-setup.md#certificate-pinning) section of [SmartLink Setup](./smartlink-setup.md) before accepting the new certificate.

## Audio

### No audio from the speakers

The panadapter and waterfall run but nothing comes out of the speakers. PC Audio is off, the wrong device is selected, something is muted, or the audio device failed to open.

1. Check that **PC Audio** is on in the title bar and that the right **Output** device is selected under **Radio Setup → Audio**. See [Audio Settings](./audio-settings.md).
2. Check the slice is not muted, and that the title-bar speaker and master slider are not muted or at zero.
3. Turn on the **Audio** log category in **Help → Support & Diagnostics...** and look for device-open errors.
4. **Linux with PipeWire:** if the PipeWire library is installed but PipeWire's client configuration is not, AetherSDR keeps Qt's audio off PipeWire at startup rather than crashing. Install your distribution's `pipewire` (Arch) or `pipewire-bin` (Debian/Ubuntu) package.

### The audio device lists show only "Dummy Output"

This affects source builds on Ubuntu and Linux Mint. The AppImage does not need this.

1. Follow the distro notes in [docs/BUILDING.md](https://github.com/aethersdr/AetherSDR/blob/main/docs/BUILDING.md#distro-notes).

### Audio stutters or clicks on a remote link

The link has jitter or packet loss.

1. Raise **Radio Setup → Audio → Audio Buffer** (default 100 ms) a little at a time.
2. Leave **Smooth packet loss** on.
3. Use Opus compression and low bandwidth mode. See [Low Bandwidth Connections](./low-bandwidth-connections.md).
4. Watch latency, jitter, packet loss and the audio buffer over time in **Tools → Network Diagnostics...**.

## Keyboard

### Space bar or arrow keys do nothing

Operating keyboard shortcuts (Space for PTT, arrows to tune, and so on) are **off by default**. While they are off, the first key press that nothing accepts shows "Keyboard shortcuts are off" in the status bar.

1. Turn them on with **Settings → Keyboard Shortcuts**.
2. See [Keyboard Shortcuts](./keyboard-shortcuts.md).

## Starting and crashing

### `error while loading shared libraries: libOpenGL.so.0`

Ubuntu 26.04 no longer installs `libopengl0` by default on the desktop image. AetherSDR's GPU spectrum and waterfall link against `libOpenGL.so.0`, so it refuses to start without it.

1. Run `sudo apt install libopengl0`.

This is the modern GLVND desktop OpenGL runtime, not the legacy `libgl1`; both can be installed side by side.

### The AppImage does not run on Raspberry Pi OS Bookworm

The ARM AppImage needs glibc 2.38 or newer, and Bookworm has glibc 2.36.

1. Use Raspberry Pi OS Trixie. See [Installation](./installation.md).

### AetherSDR crashes opening a dialog under XWayland (GLX `BadAccess`)

Some compositors produce this crash under XWayland.

1. Run natively on Wayland: `QT_QPA_PLATFORM='wayland;xcb' ./AetherSDR-*.AppImage`.

### Problems after a crash while popping out a panadapter

AetherSDR saves a floating panadapter only after the pop-out succeeds, so a crash during a pop-out does not repeat on the next launch. A stale floating layout can still be left behind.

1. Use **View → Workspace Canvas → Workspaces → Import pop-outs onto canvas** to recover it. See [Workspace Canvas](./workspace-canvas.md).

### A stored setting stops AetherSDR from starting

A bad stored value, such as a window placed off-screen, is applied at startup.

1. Use `AetherSDR --config` to inspect or remove the value. See [Settings and Backups](./settings-and-backups.md).

### AetherSDR crashes on exit with MangoHud

This is not an AetherSDR bug.

1. Disable MangoHud: `MANGOHUD=0 ./AetherSDR`.

## Display

### Panadapter is black, blank or drawn wrongly

The renderer cannot draw on this display or driver.

1. **Headless Linux (VNC, no monitor):** AetherSDR detects a Wayland session with no connected display and starts on XWayland automatically. If you have set `QT_QPA_PLATFORM` yourself, your setting wins: remove it or set `QT_QPA_PLATFORM=xcb`.
2. **Driver problems:** force software OpenGL with `AETHER_NO_GPU=1` (no rebuild needed). This is worth trying first on a Raspberry Pi.
3. **Help → About AetherSDR** shows which renderer is in use. See [GPU Rendering](./gpu-rendering.md).

## Settings

### Your settings came back after you deleted `AetherSDR.db`

Deleting `AetherSDR.db` by hand is **not** a reset: the next launch re-imports the frozen settings snapshot from your upgrade.

1. Use **Settings → Reset Settings...**. It writes a backup first, removes AetherSDR's local settings, then quits. Settings stored on the radio are not affected.
2. See [Settings and Backups](./settings-and-backups.md).

## Reporting a crash

Every release has matching debug-symbol archives, so a crash report from your computer is enough for developers to find the cause. When you file the issue (**Help → File an Issue...**), say which download you ran (AppImage, DMG, Windows installer or portable) and attach:

- **Linux:** run `coredumpctl list AetherSDR`, then `coredumpctl info <PID> > aethersdr-crash.txt` and attach the text file. Do **not** attach the core file itself. It is a copy of the program's memory and can contain passwords and tokens.
- **macOS:** the newest `AetherSDR-*.ips` from **Console → Crash Reports** or `~/Library/Logs/DiagnosticReports/`. It names your user folder in file paths; edit that out if you prefer.
- **Windows:** Windows keeps a crash dump only if local dumps are switched on. In an administrator PowerShell:
  ```powershell
  $k = 'HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\AetherSDR.exe'
  New-Item -Force $k | Out-Null
  New-ItemProperty -Force $k -Name DumpType -PropertyType DWord -Value 1 | Out-Null
  ```
  After the next crash, attach the dump from `%LOCALAPPDATA%\CrashDumps\` (`AetherSDR.exe.<pid>.dmp`).

The full guide is [docs/debugging-crashes.md](https://github.com/aethersdr/AetherSDR/blob/main/docs/debugging-crashes.md).

## Filing a bug

If nothing here or on the feature's own page fixes the problem, use **Help → File an Issue...**. It builds a support bundle and opens a pre-filled GitHub bug report. See [Support and Logging](./support-and-logging.md) for which log categories to turn on first.

## Reference

### File locations

| | Linux | macOS | Windows |
|---|---|---|---|
| **Settings** (`AetherSDR.db`) | `~/.config/AetherSDR/` | `~/Library/Preferences/AetherSDR/` | `%LOCALAPPDATA%\AetherSDR\` |
| **Logs** | `~/.config/AetherSDR/logs/` | `~/Library/Preferences/AetherSDR/logs/` | `%LOCALAPPDATA%\AetherSDR\logs\` |

Each launch writes a new timestamped log; attach the newest one. See [Support and Logging](./support-and-logging.md) and [Settings and Backups](./settings-and-backups.md).

## By feature

These pages have their own Troubleshooting section:

- [Accessibility](./accessibility.md)
- [AetherClock and GPS](./aetherclock-and-gps.md)
- [Aetherial Audio](./aetherial-audio.md)
- [AetherModem Packet Radio](./aethermodem-packet-radio.md)
- [AetherSweep](./aethersweep.md)
- [Amplifiers](./amplifiers.md)
- [ANAN-G2](./anan-g2.md)
- [Audio Settings](./audio-settings.md)
- [Automation Bridge and MCP](./automation-bridge-and-mcp.md)
- [Before You Transmit](./before-you-transmit.md)
- [BNR GPU Noise Removal](./bnr-gpu-noise-removal.md)
- [Building from Source](./building-from-source.md)
- [Callsign Lookup](./callsign-lookup.md)
- [CAT Control](./cat-control.md)
- [Copy Assist](./copy-assist.md)
- [CTR2 Proxy](./ctr2-proxy.md)
- [CW Decoder](./cw-decoder.md)
- [CWX Panel](./cwx-panel.md)
- [D-STAR (ThumbDV)](./d-star-thumbdv.md)
- [DAX IQ Streaming](./dax-iq-streaming.md)
- [DAX Virtual Audio](./dax-virtual-audio.md)
- [Demo Mode](./demo-mode.md)
- [Diversity and ESC](./diversity-and-esc.md)
- [DSP Noise Mitigation](./dsp-noise-mitigation.md)
- [DVK Panel](./dvk-panel.md)
- [Firmware Update](./firmware-update.md)
- [First Connection](./first-connection.md)
- [FlexControl Tuning Knob](./flexcontrol-tuning-knob.md)
- [FlexRadio](./flexradio.md)
- [GPU Rendering](./gpu-rendering.md)
- [Green Heron Everyware](./green-heron-everyware.md)
- [Hermes-Lite 2](./hermes-lite-2.md)
- [Installation](./installation.md)
- [Keyboard Shortcuts](./keyboard-shortcuts.md)
- [KiwiSDR and Web-888](./kiwisdr-and-web-888.md)
- [Linux](./linux.md)
- [Low Bandwidth Connections](./low-bandwidth-connections.md)
- [macOS](./macos.md)
- [Manual Connection](./manual-connection.md)
- [Memory Channels](./memory-channels.md)
- [Menu Reference](./menu-reference.md)
- [Meters](./meters.md)
- [MIDI Controller Mapping](./midi-controller-mapping.md)
- [MQTT Station Automation](./mqtt-station-automation.md)
- [Multi-Flex](./multi-flex.md)
- [Multi-Slice Operation](./multi-slice-operation.md)
- [Net Scheduler](./net-scheduler.md)
- [Networked Icom](./networked-icom.md)
- [NR2 Noise Reduction](./nr2-noise-reduction.md)
- [Panadapter Controls](./panadapter-controls.md)
- [Peripherals](./peripherals.md)
- [Profile Management](./profile-management.md)
- [PSK Reporter Map](./psk-reporter-map.md)
- [RADE Digital Voice](./rade-digital-voice.md)
- [Radio Setup](./radio-setup.md)
- [RTL-SDR](./rtl-sdr.md)
- [RTTY Operation](./rtty-operation.md)
- [Runtime Monitor](./runtime-monitor.md)
- [RX Controls](./rx-controls.md)
- [Settings and Backups](./settings-and-backups.md)
- [ShackSwitch](./shackswitch.md)
- [Slice Colors](./slice-colors.md)
- [SmartLink Setup](./smartlink-setup.md)
- [Split Operation](./split-operation.md)
- [SpotHub](./spothub.md)
- [StreamDeck](./streamdeck.md)
- [Support and Logging](./support-and-logging.md)
- [Tailscale Remote Access](./tailscale-remote-access.md)
- [TCI Server](./tci-server.md)
- [TGXL Tuner Control](./tgxl-tuner-control.md)
- [Themes and Theme Editor](./themes-and-theme-editor.md)
- [TNF (Tracking Notch Filters)](./tnf-tracking-notch-filters.md)
- [TX Controls](./tx-controls.md)
- [Ulanzi Dial](./ulanzi-dial.md)
- [USB Cable Management](./usb-cable-management.md)
- [USB Control Surfaces](./usb-control-surfaces.md)
- [VFO Widget](./vfo-widget.md)
- [Windows](./windows.md)
- [Workspace Canvas](./workspace-canvas.md)
- [WSJT-X Integration](./wsjt-x-integration.md)

## See also

- [Support and Logging](./support-and-logging.md)
- [Runtime Monitor](./runtime-monitor.md)
- [Settings and Backups](./settings-and-backups.md)
- [Installation](./installation.md)
- [docs/debugging-crashes.md](https://github.com/aethersdr/AetherSDR/blob/main/docs/debugging-crashes.md)
