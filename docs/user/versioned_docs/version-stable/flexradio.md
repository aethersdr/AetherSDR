---
title: "FlexRadio"
slug: "/flexradio"
description: "AetherSDR is a native client for FlexRadio transceivers on Linux, macOS and Windows."
status: "Supported"
applies_to: ["FlexRadio"]
---

:::info[Status]

**Status:** Supported · **Applies to:** FlexRadio

:::

AetherSDR is a native client for FlexRadio transceivers on Linux, macOS and Windows. It speaks the SmartSDR protocol directly, so it runs alongside SmartSDR and Maestro and shares the radio's own profiles and memories. FlexRadio is AetherSDR's supported target: the rest of this documentation describes FlexRadio operation unless a page says otherwise.

This page is the starting point for Flex owners. It lists the supported models and firmware, the recommended order for getting on the air, and every page with Flex-specific features.

## Requirements

### Supported models

AetherSDR works with any FlexRadio transceiver, including:

- **FLEX-6000 series:** FLEX-6300, FLEX-6400, FLEX-6400M, FLEX-6500, FLEX-6600, FLEX-6600M, FLEX-6700
- **FLEX-8000 series:** FLEX-8400, FLEX-8400M, FLEX-8600, FLEX-8600M
- **Aurora series:** AU-510, AU-510M, AU-520, AU-520M
- ML-, CL- and RT-series devices

The number of slices and panadapters comes from the radio itself, so AetherSDR offers exactly what your model can do.

### Firmware

- The active test target is **FLEX-8600 firmware 4.2.18** (SmartSDR protocol v1.4.0.0).
- Earlier **4.x** firmware works.
- **v3.x is unsupported.**

To update the radio from AetherSDR, see [Firmware Update](./firmware-update.md) (experimental; updating with SmartSDR for Windows is still the recommended route).

### Network

- For local operation, the computer and the radio on the same LAN. Discovery uses UDP port 4992.
- For a routed network or a VPN, the radio's IP address ([Manual Connection](./manual-connection.md)).
- For remote operation, SmartLink with port forwarding ([SmartLink Setup](./smartlink-setup.md)), or Tailscale on a FLEX-8000 series or Aurora radio ([Tailscale Remote Access](./tailscale-remote-access.md)).

## Setup

Follow these pages in order the first time:

1. **[Installation](./installation.md)** — download and install AetherSDR for your computer ([Linux](./linux.md), [macOS](./macos.md) or [Windows](./windows.md)).
2. **[Your First Session](./your-first-session.md)** (optional) — learn the window on the built-in demo, with no radio and no risk of transmitting.
3. **[First Connection](./first-connection.md)** — find the radio under **On This Network** and connect.
4. **[Audio Settings](./audio-settings.md)** — pick your speakers, headphones and microphone, and the PC Audio path.
5. **[Before You Transmit](./before-you-transmit.md)** — the checklist to work through before you key up for the first time.

## Using AetherSDR with a FlexRadio

### Operating

- [Panadapter Controls](./panadapter-controls.md) — spectrum, waterfall, the Display panel and up to 8 panadapters
- [VFO Widget](./vfo-widget.md) and [RX Controls](./rx-controls.md) — frequency, mode, filters, AGC, squelch, RIT/XIT, and the radio's own DSP
- [TX Controls](./tx-controls.md) — RF and tune power, TUNE, MOX, ATU and APD
- [Meters](./meters.md) — S-meter, SmartMTR, cross-needle power and SWR, Radio Vitals
- [Multi-Slice Operation](./multi-slice-operation.md) and [Split Operation](./split-operation.md) — several receivers, TX assignment and split
- [Diversity and ESC](./diversity-and-esc.md) — two-SCU diversity reception and beamforming on dual-SCU models
- [TNF (Tracking Notch Filters)](./tnf-tracking-notch-filters.md) — radio-side tracking notches
- [XVTR (Transverters)](./xvtr-transverters.md) — transverter setup
- [Aetherial Audio](./aetherial-audio.md) — the AetherTX and AetherRX client audio chains

### Radio configuration

- [Radio Setup](./radio-setup.md) — every settings page, including Transmit, Antennas, GPS and Peripherals
- [Profile Management](./profile-management.md) — Global, TX and Mic profiles, stored on the radio and shared with SmartSDR; import and export `.ssdr_cfg` packages
- [Memory Channels](./memory-channels.md) — an editor for the radio's own memory bank
- [Firmware Update](./firmware-update.md) — upload SmartSDR firmware from AetherSDR
- [USB Cable Management](./usb-cable-management.md) — CAT, band decoder, bit, LDPA and passthrough cables on the radio's rear USB ports

### Remote operation and sharing

- [SmartLink Setup](./smartlink-setup.md) — operate over the internet with your SmartLink account
- [Tailscale Remote Access](./tailscale-remote-access.md) — remote operation behind CGNAT on FLEX-8000 series and Aurora radios
- [Manual Connection](./manual-connection.md) — connect by IP across routed networks and VPNs
- [Low Bandwidth Connections](./low-bandwidth-connections.md) — reduce traffic for VPN, LTE and metered links
- [Multi-Flex](./multi-flex.md) — run alongside SmartSDR, Maestro or another AetherSDR

### Digital modes and other programs

- [DAX Virtual Audio](./dax-virtual-audio.md) — virtual audio channels for WSJT-X, fldigi and others (Linux and macOS)
- [DAX IQ Streaming](./dax-iq-streaming.md) — raw I/Q at 24–192 kHz
- [CAT Control](./cat-control.md) — up to 8 CAT ports with Rigctld, TS-2000 and Flex dialects
- [TCI Server](./tci-server.md) — CAT, audio, IQ, CW and spots over one WebSocket
- [WSJT-X Integration](./wsjt-x-integration.md) — FT8/FT4 over TCI, or CAT + DAX
- [RADE Digital Voice](./rade-digital-voice.md), [D-STAR (ThumbDV)](./d-star-thumbdv.md), [RTTY Operation](./rtty-operation.md) and [AetherModem Packet Radio](./aethermodem-packet-radio.md)
- [CWX Panel](./cwx-panel.md), [CW Decoder](./cw-decoder.md) and [DVK Panel](./dvk-panel.md) — CW keying, decoding and the voice keyer

### Amplifiers, tuners and station accessories

- [TGXL Tuner Control](./tgxl-tuner-control.md) — 4O3A Tuner Genius XL, direct or relayed through the radio
- [Peripherals](./peripherals.md) — PGXL amplifier, Antenna Genius and other accessories, with 4O3A access codes
- [Amplifiers](./amplifiers.md) — ACOM, SPE Expert, VK3AMP and KPA1500 amplifiers
- [ShackSwitch](./shackswitch.md) and [Green Heron Everyware](./green-heron-everyware.md) — antenna switches and rotators

### Controllers

- [FlexControl Tuning Knob](./flexcontrol-tuning-knob.md) — the FlexControl knob and the on-screen AetherControl
- [CTR2 Proxy](./ctr2-proxy.md) — relay a CTR2-Max controller to the radio
- [USB Control Surfaces](./usb-control-surfaces.md), [MIDI Controller Mapping](./midi-controller-mapping.md), [StreamDeck](./streamdeck.md) and [Keyboard Shortcuts](./keyboard-shortcuts.md)

## Reference

Features that work only with a FlexRadio, compared with the other radio families (from [Supported Radios](./supported-radios.md)):

| Feature | FlexRadio | Other families |
|---|---|---|
| DAX virtual audio and IQ | Yes | No |
| SmartLink | Yes | No |
| Multi-Flex (several clients) | Yes | No |
| Firmware update from AetherSDR | Yes | No |
| Profiles | Stored on the radio | Not offered |
| Memory channels | Stored on the radio | Stored on this computer |

## Troubleshooting

### The radio does not appear under On This Network

The radio is on another subnet, or a firewall, VPN or guest Wi-Fi isolation is blocking discovery on UDP port 4992.

1. Check the radio answers: `ping <radio-ip>`.
2. Allow UDP port 4992 through the computer's firewall.
3. Use **Connect by IP** with **Radio type:** FlexRadio. See [Manual Connection](./manual-connection.md).
4. **Open Network Diagnostics** in the Connect to Radio window helps narrow it down.

### The radio refuses the connection

The radio rejected AetherSDR's client registration, for example with error `F3000001`. AetherSDR returns to the Connect to Radio window with the radio's error and what to do about it.

1. Follow the message: typically another client must disconnect first, or the radio needs a restart.
2. If multiFLEX is off and other clients are connected, use the **Connected Stations** dialog to disconnect one. See [Multi-Flex](./multi-flex.md).

For anything else, see [Troubleshooting](./troubleshooting.md).

## See also

- [Supported Radios](./supported-radios.md)
- [First Connection](./first-connection.md)
- [Before You Transmit](./before-you-transmit.md)
- [Profile Management](./profile-management.md)
- [Troubleshooting](./troubleshooting.md)
- [FlexRadio](https://www.flexradio.com/)
