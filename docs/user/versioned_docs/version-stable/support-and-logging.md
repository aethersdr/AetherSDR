---
title: "Support and Logging"
slug: "/support-and-logging"
description: "When something isn't working, AetherSDR has several built-in tools for capturing what happened and turning it into a useful report."
---

When something isn't working, AetherSDR has several built-in tools for capturing what happened and turning it into a useful report:

- **Help → Support & Diagnostics...**: log categories and a live log viewer
- **Help → File an Issue...**: builds a support bundle and opens a pre-filled GitHub bug report
- **Help → Submit your Idea... 💡**: an AI-assisted reporter for feature requests and conceptual bugs
- **Help → Slice Troubleshooting...**: a guided check for slice problems
- **Tools → Runtime Monitor...**, **Tools → Network Diagnostics...** and **Tools → Radio Health...**: see [Runtime Monitor](./runtime-monitor.md)

Before you file a report, drop your log or support bundle on the [Log Analyzer](/log-analyzer). It checks for known problems and links the fix for each one it finds. It runs in your browser; the file is never uploaded.

The Help menu also has **Getting Started...**, **AetherSDR Help...**, **What's New...**, three how-to guides (**Understanding Noise Cancellation...**, **Configuring AetherSDR Controls...**, **Configuring Data Modes...**), **AetherSDR Website**, **Donate to AetherSDR**, **Contributing to AetherSDR...**, **Check for Updates...** and **About AetherSDR**. On Windows and Linux the menus are behind the **☰** button at the left of the title bar; on macOS they are in the system menu bar.

## Using Support & Diagnostics

**Help → Support & Diagnostics...** has two parts.

### Diagnostic Logging

A grid of checkboxes, one per log category. Hover a box for what it covers. Turn on the categories relevant to your problem, then reproduce it. The dialog advises restarting AetherSDR after changing categories so the new settings are in effect from startup.

**Enable All** / **Disable All** toggle every category at once. The categories, and which are on by default, are listed under [Log categories](#log-categories).

### Log viewer

Below the grid, the dialog shows the path of the current log file, its size, and the last part of the log. **Refresh** re-reads it, **Clear Log** empties it (handy just before reproducing a bug), and **Open Log Folder** opens the log directory in your file manager.

## Filing a bug report

**Help → File an Issue...**:

1. **Builds a support bundle**: a timestamped zip, `support-bundle-<date>-<time>.zip`, in the `logs/support/` folder. Its contents are listed under [Support bundle contents](#support-bundle-contents).
2. **Copies an AI prompt** to your clipboard, pre-filled with your AetherSDR version, Qt version, OS and radio, and opens a dialog with buttons for **Claude**, **ChatGPT**, **Gemini**, **Grok** and **Perplexity**. Paste the prompt into the AI, describe what went wrong, and it writes a structured bug report.
3. **Submit Bug Report** opens GitHub's bug-report form, pre-filled with your system and radio details and a redacted tail of the recent log. If the log tail is too long for the link, it is put on your clipboard instead.
4. Drag the support bundle into the GitHub form.

### Reporting a crash

For a crash, also attach the crash report your operating system kept: `coredumpctl info` on Linux, the `.ips` file on macOS, or a minidump on Windows. Every release has matching debug-symbol archives, so this is enough for a developer to see exactly where it crashed. Step-by-step instructions are in [Troubleshooting](./troubleshooting.md) and [docs/debugging-crashes.md](https://github.com/aethersdr/AetherSDR/blob/main/docs/debugging-crashes.md).

## Submitting an idea

**Help → Submit your Idea... 💡** opens the **AI-Assisted Issue Reporter**. It is for **feature requests and conceptual bug reports** rather than runtime diagnostics. No support bundle is built.

1. If a newer release exists, AetherSDR first says so and suggests updating, since your issue may already be fixed.
2. The **AI-Assisted Issue Reporter** offers Claude, ChatGPT, Gemini, Grok and Perplexity. Click one: a structured prompt is copied and that AI opens in your browser.
3. Paste the prompt and replace the bracketed section with your idea or bug. The prompt tells the AI to check the open issue list for duplicates first, then write a complete issue in the project's format.
4. Click **Submit Your Idea** (feature request) or **Report a Bug** to open the matching GitHub issue template, and paste the AI's output.

## Finding your version and build details

**Help → About AetherSDR** shows the version and commit, and a build-details card (Qt version, build date and the active renderer) whose text you can select and copy into a bug report. **Contributor Logbook ↗** opens [contributors.aethersdr.com](https://contributors.aethersdr.com/#all-time). Esc or Ctrl/Cmd+W closes it.

## Before you share a log

- Logs, the GitHub log tail and the support bundle are redacted as described under [What is redacted](#what-is-redacted), but **skim before you post publicly**.
- `radio-info.json` carries your callsign in clear and the radio's serial and IP only in redacted form.
- Passwords and tokens (SmartLink, MQTT, automation token, Copy Assist API key and others) are kept in the OS keychain, never in the settings, so they cannot appear in `settings.txt`.
- On Linux, never attach a core file. Attach the `coredumpctl info` text instead.

## Reference

### When to use which

| Situation | Use |
|---|---|
| Crash or wrong behaviour while operating | Enable the relevant categories → reproduce → **Help → File an Issue...** |
| Audio glitch, spectrum freeze, DAX problem | **Support & Diagnostics...** with *Audio*, *VITA-49* or *DAX*; **Tools → Network Diagnostics...** |
| App feels slow or freezes | **Tools → Runtime Monitor...**; *Performance* and *Render* categories |
| Connection or discovery problem | *Discovery* + *Connection / Commands* |
| SmartLink problem | *SmartLink* + *Audio* + *VITA-49* |
| WSJT-X / TCI / CAT keying the wrong slice | *TCI / CAT / rigctld* |
| Feature request or design idea | **Help → Submit your Idea... 💡** |

### Log categories

| Label | Category | Covers |
|---|---|---|
| **Discovery** | `aether.discovery` | UDP radio discovery broadcasts |
| **Connection / Commands** | `aether.connection` | Raw TCP command channel: commands sent, responses, socket state |
| **Protocol / Status** | `aether.protocol` | Parsed SmartSDR protocol handling and status updates |
| **Audio** | `aether.audio` | RX/TX audio, device negotiation, volume |
| **Audio Summary** | `aether.audio.summary` | Audio routing and device summaries for support logs |
| **VITA-49** | `aether.vita49` | UDP packet routing: FFT, waterfall, meters, DAX |
| **DSP** | `aether.dsp` | Client noise reduction and CW decoder processing |
| **RADE** | `aether.rade` | FreeDV RADE digital voice |
| **SmartLink** | `aether.smartlink` | SmartLink sign-in, TLS tunnel, WAN streaming |
| **TCI / CAT / rigctld** | `aether.cat` | TCI server (slice and DAX arming, TX audio summary), rigctld TCP servers, PTY virtual serial ports |
| **DAX** | `aether.dax` | Virtual audio bridge (PipeWire / CoreAudio) |
| **Meters** | `aether.meters` | Meter definitions and value conversion |
| **Transmit** | `aether.transmit` | TX state, ATU, profiles, power control |
| **Firmware** | `aether.firmware` | Firmware download, staging, upload |
| **Tuner/AGM** | `aether.tuner` | TGXL tuner, Antenna Genius state |
| **Peripheral Authorization** | `aether.peripheral.auth` | Peripheral access-code storage (code values are never logged) |
| **GUI** | `aether.gui` | Windows, applets, dialogs |
| **DX Cluster** | `aether.dxcluster` | Cluster telnet connection and spot parsing |
| **MQTT** | `aether.mqtt` | MQTT client connection and messages |
| **RBN** | `aether.rbn` | Reverse Beacon Network connection and spots |
| **Ext Devices** | `aether.devices` | Serial ports, FlexControl, MIDI, HID encoders |
| **Performance** | `aether.perf` | Render timing and CPU profiling |
| **Render** | `aether.render` | GPU/QRhi path selection and fallback, paint stalls |
| **Propagation** | `aether.propforecast` | Solar and propagation forecast updates |
| **CW / netCW** | `aether.cw` | CW keying, MIDI paddle, iambic and netCW timing |
| **S History** | `aether.shistory` | Signal History voice detection |
| **AetherModem** / **AX.25 Link** | `aether.ax25`, `aether.ax25.link` | Packet modem, and connected-mode link timing and retransmits |
| **Waveform** | `aether.waveform` | Waveform install upload and the local waveform helper (D-STAR) |
| **KiwiSDR** / **KiwiSDR Audio/DSP** | `aether.kiwisdr`, `aether.kiwisdr.audio` | KiwiSDR receivers; the audio category is high-rate |
| **Automation Bridge** | `aether.automation` | The agent automation bridge |
| **QRZ Lookup** | `aether.qrz` | QRZ.com lookups and cache |
| **AetherClock** | `aether.clock` | WWV/WWVB time-signal decoder |
| **Hermes-Lite 2** / **Hermes-Lite 2 TX** | `aether.hl2`, `aether.hl2.tx` | HL2 backend; the TX category is high-rate transmit telemetry |
| **ANAN Protocol 2** | `aether.anan.p2` | ANAN-G2 Protocol 2 session |
| **Icom Session / Streams / CI-V / Scope / Link / Credentials** | `aether.icom.*` | Networked Icom backend; **Icom CI-V** logs every frame and is high-rate |
| **System Info** | `aether.sysinfo` | Startup hardware inventory: OS, CPU and SIMD features, RAM, GPU |

**On by default:** Discovery, Connection / Commands, Protocol / Status, Audio Summary, KiwiSDR and System Info. Everything else is off until you turn it on. A category that is off still records its warnings and errors.

### Log files

| Platform | Log folder |
|---|---|
| **Linux** | `~/.config/AetherSDR/logs/` |
| **macOS** | `~/Library/Preferences/AetherSDR/logs/` |
| **Windows** | `%LOCALAPPDATA%\AetherSDR\logs\` |

- Every launch starts a new file named `aethersdr-<date>-<time>.log`. Attach the newest one. On Linux and macOS, `aethersdr.log` in the same folder is a link to it; on Windows it is a `.lnk` shortcut.
- A log that grows past 100 MB is rotated to a new file.
- Old logs are pruned at startup after 7 days or once they total more than 500 MB; the two most recent are always kept.
- Logging is buffered and written in the background, so it does not slow the app down.

### What is redacted

Every line is scrubbed before it is written: IP addresses (all but the last part), MAC addresses, radio serial numbers (all but the last group), your home-directory path, email addresses, tokens and passwords, personal names, GPS coordinates and grid squares. Your **callsign**, radio model, firmware version, frequencies and modes are deliberately kept, because they are what makes a log useful. Skim a log before sharing it anyway.

### Driving categories from the command line

The categories are Qt logging categories, so `QT_LOGGING_RULES` can raise one before the GUI opens. For example, to log one line for every TCI transmit-routing decision (useful when TCI keys the wrong slice):

```bash
QT_LOGGING_RULES="aether.cat.info=true" ./AetherSDR-*.AppImage
```

For anything else, prefer the checkboxes in **Help → Support & Diagnostics...**.

### Support bundle contents

| File | Contents |
|---|---|
| `aethersdr.log` (plus up to two older logs) | Recent logs, re-scrubbed on the way out |
| `system-info.json` | AetherSDR version, Qt version, OS, kernel, CPU model with SIMD features, architecture, RAM, GPU, build date |
| `radio-info.json` | Radio model, firmware, protocol version and callsign, with serial and IP redacted |
| `settings.txt` | A sanitized dump of your AetherSDR settings with every password and token removed, plus any settings-store notice. See [Settings and Backups](./settings-and-backups.md). |
| `enabled-categories.txt` | Which log categories were on |

## Known issues

- With WNB on, a FLEX-8600 fills the default-on Connection / Commands log category with a `wnb_updating` status line every few seconds ([#6069](https://github.com/aethersdr/AetherSDR/issues/6069)).

## Troubleshooting

### The log has no detail about your problem

The relevant log category is off, so only its warnings and errors were recorded.

1. Open **Help → Support & Diagnostics...** and tick the categories for your problem. [When to use which](#when-to-use-which) suggests a set.
2. Restart AetherSDR so the categories are on from startup.
3. Click **Clear Log**, reproduce the problem, then use **Help → File an Issue...**.

### The GitHub bug report form has no log tail

The log tail was too long to fit in the link, so AetherSDR put it on your clipboard instead.

1. Paste the clipboard into the GitHub form.
2. Drag the support bundle from `logs/support/` into the form as well.

### You can't find the support bundle

The bundle is written to the `support/` folder inside the log folder listed under [Log files](#log-files).

1. Open **Help → Support & Diagnostics...** and click **Open Log Folder**.
2. Open the `support` folder and take the newest `support-bundle-<date>-<time>.zip`.

## See also

- [Troubleshooting](./troubleshooting.md): symptom index and crash-report instructions
- [Runtime Monitor](./runtime-monitor.md)
- [Settings and Backups](./settings-and-backups.md): **Settings → Reset Settings...** and **Settings → Settings Browser...**
- [Contributing Guide](./contributing-guide.md)
- [docs/debugging-crashes.md](https://github.com/aethersdr/AetherSDR/blob/main/docs/debugging-crashes.md)
