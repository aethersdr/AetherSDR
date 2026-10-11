---
title: "AetherSDR Documentation"
slug: "/"
description: "AetherSDR is an open-source client for FlexRadio transceivers with native builds for Linux, macOS and Windows, including an aarch64 build for the Raspberry Pi."
---

*Native. Open. Yours.*

AetherSDR is an open-source client for **FlexRadio** transceivers with native builds for Linux, macOS and Windows, including an aarch64 build for the Raspberry Pi. FlexRadio is the supported target; the Hermes-Lite 2, networked Icom radios, the ANAN-G2 and RTL-SDR dongles are also supported at an earlier stage, each labelled with its status. No radio? [Demo Mode](./demo-mode.md) runs the full interface against a built-in simulator.

These docs come in two versions, chosen from the version menu at the top of every page. **Stable**, the default, describes the latest release. **Next (main)** tracks `main`: it describes what has landed since that release and will ship in the next one. Release notes for every version are in the [CHANGELOG](https://github.com/aethersdr/AetherSDR/blob/main/CHANGELOG.md).

Prefer paper? Download the [printable manual](pathname:///AetherSDR-Manual.pdf) (PDF).

## Where are you starting from?

| If you… | Start with |
|---|---|
| Are brand new, or have no radio connected yet | [Your First Session](./your-first-session.md), then [Installation](./installation.md) |
| Own a FlexRadio | [FlexRadio](./flexradio.md) → [First Connection](./first-connection.md) → [Before You Transmit](./before-you-transmit.md) |
| Own a Hermes-Lite 2, Icom, ANAN-G2 or RTL-SDR dongle | [Supported Radios](./supported-radios.md), then your radio's page |
| Run Linux, macOS or Windows and want the platform specifics | [Linux](./linux.md) · [macOS](./macos.md) · [Windows](./windows.md) |
| Want FT8, RTTY or other digital modes | [WSJT-X Integration](./wsjt-x-integration.md), [DAX Virtual Audio](./dax-virtual-audio.md), [CAT Control](./cat-control.md), [TCI Server](./tci-server.md) |
| Operate remotely | [SmartLink Setup](./smartlink-setup.md), [Tailscale Remote Access](./tailscale-remote-access.md), [Manual Connection](./manual-connection.md) |
| Have a problem | [Troubleshooting](./troubleshooting.md), then [Support and Logging](./support-and-logging.md) to report it |
| Want to help with the code or docs | [Contributing Guide](./contributing-guide.md), [Docs Style Guide](./docs-style-guide.md) |

## Start Here

- [Your First Session](./your-first-session.md) — Hands-on tutorial with no radio needed: tune, filter, reduce noise and save a memory in Demo Mode
- [Installation](./installation.md) — Download and install on Linux, macOS (14.4+) or Windows
- [First Connection](./first-connection.md) — Connect to your radio for the first time
- [Before You Transmit](./before-you-transmit.md) — Checklist before your first transmission: TX slice, callsign, mic, power, amplifier, keying paths

## Your Radio

- [Supported Radios](./supported-radios.md) — FlexRadio models and firmware, other radio families' status, and what works where
- [FlexRadio](./flexradio.md) — Start here with a FlexRadio: models, firmware, setup order and every Flex feature page
- [Hermes-Lite 2](./hermes-lite-2.md) — Experimental: setup, hardware, calibration, receive, transmit and limits
- [Networked Icom](./networked-icom.md) — IC-7300MK2 (supported), IC-705, IC-9700 over RS-BA1: connecting, PC Audio, CWK, XFC
- [ANAN-G2](./anan-g2.md) — Experimental, receive-only: connect options, droop correction, what works
- [RTL-SDR](./rtl-sdr.md) — Experimental, receive-only USB dongles: availability, modes, receiver corrections
- [KiwiSDR and Web-888](./kiwisdr-and-web-888.md) — Public KiwiSDR/Web-888 receivers as receive-only slice antennas, with Receive Sync
- [Demo Mode](./demo-mode.md) — Explore the full UI with no radio: built-in simulator, noise and fault injection

## Your Computer

- [Linux](./linux.md) — AppImage install, Wayland/xcb, PipeWire, udev rules, file locations and Linux-only fixes
- [macOS](./macos.md) — DMG install, DAX driver, Input Monitoring, MNR, file locations and macOS-only fixes
- [Windows](./windows.md) — Installer or portable, TCI instead of DAX, Direct3D, file locations and Windows-only fixes

## Operating

- [Panadapter Controls](./panadapter-controls.md) — Spectrum, waterfall, 3D view, Display panel, Mini-Pan, Center Lock
- [VFO Widget](./vfo-widget.md) — Frequency display, flag tabs, adaptive RX filter, slice management
- [RX Controls](./rx-controls.md) — Filters, AGC, squelch, mute, RIT/XIT
- [TX Controls](./tx-controls.md) — Power, ATU pre-tune, MOX, TUNE, TX filter warnings, Quindar tones
- [Meters](./meters.md) — S-Meter, SmartMTR, cross-needle PWR, Radio Vitals, Antenna Health
- [Aetherial Audio](./aetherial-audio.md) — The AetherTX and AetherRX audio chains: gate, EQ, compressor, de-esser, tube, exciter, reverb, limiter, presets
- [Multi-Slice Operation](./multi-slice-operation.md) — Working with multiple slices, Slice Link
- [Split Operation](./split-operation.md) — SPLIT/SWAP, Split Up offsets, Monitor TX, remembered audio, Split QSY closing
- [Diversity and ESC](./diversity-and-esc.md) — Diversity mode and Enhanced Signal Clarity
- [TNF (Tracking Notch Filters)](./tnf-tracking-notch-filters.md) — Creating and managing notch filters
- [Memory Channels](./memory-channels.md) — Saving, recalling, importing and editing memories
- [Profile Management](./profile-management.md) — Global, TX and Mic profiles; import and export
- [XVTR (Transverters)](./xvtr-transverters.md) — Transverter setup and operation
- [Workspace Canvas](./workspace-canvas.md) — Experimental free layout: placed pans and applets, named workspaces, canvas windows

## CW & Digital Modes

- [CWX Panel](./cwx-panel.md) — Keyboard-to-CW keying (CWK on Icom)
- [CW Decoder](./cw-decoder.md) — Real-time Morse decode of received and transmitted CW
- [DVK Panel](./dvk-panel.md) — Digital Voice Keyer: 12-slot recording and playback for contesting
- [RTTY Operation](./rtty-operation.md) — Built-in RTTY decoder, Mark/Space lines, external software
- [RADE Digital Voice](./rade-digital-voice.md) — FreeDV RADE AI digital voice, sync/SNR, FreeDV Reporter
- [D-STAR (ThumbDV)](./d-star-thumbdv.md) — Local D-STAR digital voice using a PC-attached ThumbDV/DV3000U
- [AetherModem Packet Radio](./aethermodem-packet-radio.md) — Built-in AX.25: APRS, WIDE1-1 digipeater, KISS TNC, BBS terminal and mailbox
- [Copy Assist](./copy-assist.md) — On-device speech-to-text for received voice (whisper.cpp)
- [WSJT-X Integration](./wsjt-x-integration.md) — FT8/FT4 over TCI, or CAT + DAX

## Connecting Other Software

- [DAX Virtual Audio](./dax-virtual-audio.md) — Virtual audio channels for digital mode apps
- [DAX IQ Streaming](./dax-iq-streaming.md) — Raw I/Q streaming at 24–192 kHz
- [CAT Control](./cat-control.md) — The CAT applet: Rigctld, TS-2000 and Flex dialects on up to 8 ports
- [TCI Server](./tci-server.md) — TCI over WebSocket: CAT, audio, IQ, CW and spots
- [MQTT Station Automation](./mqtt-station-automation.md) — Control rotators, switches and Node-RED flows over MQTT
- [Automation Bridge and MCP](./automation-bridge-and-mcp.md) — Local automation bridge and MCP server: token, observe-only mode, gated transmit

## Spotting & Tools

- [SpotHub](./spothub.md) — DX Cluster, RBN, WSJT-X, POTA, FreeDV Reporter, N1MM, EiBi, SpotCollector, Smart Spot Filtering
- [PSK Reporter Map](./psk-reporter-map.md) — Who hears you: 2D map or 3D globe, weather radar, night lights
- [AetherSweep](./aethersweep.md) — In-panadapter SWR analyzer with CSV export
- [Net Scheduler](./net-scheduler.md) — Recurring net reminders with one-click Tune Now
- [AetherClock and GPS](./aetherclock-and-gps.md) — WWV/WWVB time-signal decoder and the GPS & Station Location dashboard
- [Callsign Lookup](./callsign-lookup.md) — QRZ.com lookups, cache and CW-decoder contact cards

## Station Accessories

- [Peripherals](./peripherals.md) — Add, remove and auto-connect station accessories; 4O3A access codes; PGXL and Antenna Genius
- [Amplifiers](./amplifiers.md) — ACOM, SPE Expert, VK3AMP and KPA1500 amplifiers; LP-100A wattmeter
- [TGXL Tuner Control](./tgxl-tuner-control.md) — 4O3A Tuner Genius XL: direct or relayed, autotune, antenna switch
- [ShackSwitch](./shackswitch.md) — Network antenna switch with dummy-load protection
- [Green Heron Everyware](./green-heron-everyware.md) — Green Heron Everyware antenna switch and rotator control
- [USB Cable Management](./usb-cable-management.md) — CAT, BCD, Bit, LDPA and Passthrough cables on the radio's USB ports

## Controllers

- [StreamDeck](./streamdeck.md) — Native Stream Deck+ over USB; other models via TCI or the automation bridge
- [FlexControl Tuning Knob](./flexcontrol-tuning-knob.md) — FlexControl knob and the on-screen AetherControl
- [Ulanzi Dial](./ulanzi-dial.md) — Ulanzi Dial knob and keys: wheel actions, key mapping, PTT and CW keying
- [USB Control Surfaces](./usb-control-surfaces.md) — RC-28, PowerMate, Contour Shuttle, Stream Deck+ and serial PTT/CW interfaces
- [MIDI Controller Mapping](./midi-controller-mapping.md) — Map any MIDI controller, with Learn mode and profile import/export
- [CTR2 Proxy](./ctr2-proxy.md) — Relay a CTR2-Max controller to the radio over Wi-Fi (USB pending CTR2 firmware)

## Remote Operation

- [SmartLink Setup](./smartlink-setup.md) — Operate your radio over the internet
- [Manual Connection](./manual-connection.md) — Connect by IP across routed networks, VPN source path, diagnostics
- [Tailscale Remote Access](./tailscale-remote-access.md) — Remote operation behind CGNAT through a container in the radio (FLEX-8000/Aurora), on Tailscale's free plan
- [Low Bandwidth Connections](./low-bandwidth-connections.md) — Reduce traffic for VPN, LTE and metered links
- [Multi-Flex](./multi-flex.md) — Operating alongside SmartSDR or Maestro

## Settings

- [Radio Setup](./radio-setup.md) — The searchable settings tree, page by page
- [Audio Settings](./audio-settings.md) — Line out, headphones, PC audio devices
- [Settings and Backups](./settings-and-backups.md) — Where settings live, keychain credentials, backups, `--config`, resetting properly
- [Keyboard Shortcuts](./keyboard-shortcuts.md) — Operating shortcuts (off by default), PTT hold, editor, Minimal Mode
- [Themes and Theme Editor](./themes-and-theme-editor.md) — Switch themes live, edit tokens, import and export `.aethertheme` files
- [Slice Colors](./slice-colors.md) — Per-slice colours for VFOs, panadapter overlays and meters
- [Accessibility](./accessibility.md) — Screen readers, keyboard navigation, announced reasons for dimmed controls
- [Firmware Update](./firmware-update.md) — Upload FlexRadio firmware from AetherSDR

## Reference

- [Menu Reference](./menu-reference.md) — Every menu item, the title bar, radio tabs and the About window
- [Keyboard Shortcuts](./keyboard-shortcuts.md) — Operating shortcuts (off by default), PTT hold, editor, Minimal Mode

## Understanding

- [DSP Noise Mitigation](./dsp-noise-mitigation.md) — All seven client-side noise-reduction engines plus the radio's own
- [BNR GPU Noise Removal](./bnr-gpu-noise-removal.md) — In-process NVIDIA Maxine denoising on a local RTX/GeForce GPU
- [NR2 Noise Reduction](./nr2-noise-reduction.md) — Client-side spectral noise reduction parameter reference
- [GPU Rendering](./gpu-rendering.md) — QRhi spectrum and waterfall on GPU (OpenGL/Metal/D3D11), GPU selection

## Help

- [Troubleshooting](./troubleshooting.md) — Common issues and solutions
- [Support and Logging](./support-and-logging.md) — Help → Support & Diagnostics…, log categories, support bundles, crash reports
- [Runtime Monitor](./runtime-monitor.md) — CPU, threads and memory; Radio Health; Network Diagnostics

## Contributing

- [Contributing Guide](./contributing-guide.md) — How to contribute code and features
- [Docs Style Guide](./docs-style-guide.md) — How pages are laid out and written: page types, skeleton, status, Known issues
- [Translating the Docs](./translating-the-docs.md) — Adding and maintaining a translation of these docs
- [AI-Assisted Development](./ai-assisted-development.md) — Contributing with AI tools; AGENTS.md
- [Building from Source](./building-from-source.md) — Compile AetherSDR yourself (Qt 6.12)
