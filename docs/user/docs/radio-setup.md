---
title: "Radio Setup"
slug: "/radio-setup"
description: "Radio Setup holds the radio's configuration and most of AetherSDR's own preferences in one window."
---

Radio Setup holds the radio's configuration and most of AetherSDR's own preferences in one window.

Which pages appear depends on the connected radio: pages for hardware the radio does not have are hidden.

<img src="/img/screens/radio-setup.png" width="960" alt="Radio Setup window. A Search settings box spans the top. The left tree lists RADIO (Radio, Network, GPS), RECEIVE &amp; TRANSMIT (Audio, Transmit, Phone &amp; CW, Receive, Filters) and CONTROLLERS &amp; HARDWARE (Antennas, Transverters), with Radio selected. The Radio page shows Radio Information (serial blacked out, HW version v4.2.20.41343, options GPS, a Reboot Radio button, region USA, Remote On disabled, FlexControl and multiFLEX enabled), Radio Identification (model FLEX-8600, callsign KK7GWY, nickname FLX8600, station name blacked out), License Info (SmartSDR+ Early Access subscription, its expiration date, radio ID blacked out, licensed version v4) and Firmware Update (FW version 4.2.20.41343, with Check for Update, Select Installer and a dimmed Upload Firmware button)." />

*Radio Setup, open on the Radio page. The settings tree on the left groups the pages; the search box at the top finds any setting. The serial number, radio ID and station name are blacked out here.*

## Setup

Open Radio Setup from the menu:

- **Windows and Linux:** **Settings → Radio Setup...**
- **macOS:** **AetherSDR → Preferences...**

Several menu items open Radio Setup straight at the right page:

- **Settings → FlexControl Knob & Buttons...** → Serial & Controllers
- **Settings → USB Cables...** → USB Cables
- **Tools → Configure KiwiSDR...** → Antennas

## Finding a setting

Radio Setup is a searchable browser: a categorised tree of pages on the left and the selected page on the right.

<img src="/img/screens/radio-setup-search.png" width="960" alt="Radio Setup with latency typed in the search box. The left tree now shows only RECEIVE &amp; TRANSMIT with the Audio page under it, while the right side still shows the Radio page, with the serial number, radio ID and station name blacked out." />

*Typing in the search box filters the page list: here, latency matches the Audio page.*

- **Search settings** (Ctrl+F, ⌘F on macOS) filters the tree as you type. It matches page names and also common symptom words. Try *latency*, *ptt*, *calibration* or *certificate*. Press Enter to jump to the first match.
- Pages for hardware the connected radio does not have are hidden. Individual controls a radio cannot use stay visible but dimmed, with the reason in the tooltip.

## Reference

### RADIO

| Page | What it covers |
|------|----------------|
| **Radio** | Radio information (serial, hardware version, region, options), identification (model, nickname, callsign), license info and [Firmware Update](./firmware-update.md) |
| **Network** | The radio's IP address, mask, MAC and gateway; DHCP/static IP configuration; **Advanced**: Enforce Private IP Connections, **Agent Automation (MCP)** (see [Automation Bridge and MCP](./automation-bridge-and-mcp.md)), Network MTU and the VITA-49 RX buffer (see [Low Bandwidth Connections](./low-bandwidth-connections.md)) |
| **GPS** | Whether a GPS is installed, position, grid, altitude, satellites and lock status. The full dashboard is **Tools → GPS Dashboard...**. See [AetherClock and GPS](./aetherclock-and-gps.md). Hidden on radios without GPS hardware. |
| **Calibration** | Host-side frequency calibration in ppb, for radios that cannot calibrate themselves ([Hermes-Lite 2](./hermes-lite-2.md)). Hidden on FlexRadio, which calibrates on the Receive page. |
| **RTL Receiver** | ppm correction, IQ DC suppression and serial for an [RTL-SDR](./rtl-sdr.md) dongle. Only when one is connected. |
| **HL2 Hardware** | Board variant, dither, companion filter board, CL1 external 10 MHz reference and tuner for the [Hermes-Lite 2](./hermes-lite-2.md). Only when an HL2 is connected. |
| **Droop Correction** | Panadapter edge-droop correction for the [ANAN-G2](./anan-g2.md). Only when one is connected. |

### RECEIVE & TRANSMIT

| Page | What it covers |
|------|----------------|
| **Audio** | The radio's Line Out, Headphone and Front Speaker outputs; SmartLink audio compression; packet-loss smoothing; PC audio devices, Audio Boost and Audio Buffer; recording. See [Audio Settings](./audio-settings.md). |
| **Transmit** | TX timings (ACC TX, TX delay, timeout), TX REQ interlock polarity (RCA, Accessory), Max Power, Show TX in Waterfall, Slice/TX Follow, and **TX Band Settings** (per-band RF/tune power, PTT inhibit and interlocks). See [TX Controls](./tx-controls.md). |
| **Phone & CW** | Microphone bias and +20 dB boost; CW iambic mode, paddle swap, sideband and CWX sync, and CW decoder RX/TX decode; Digital (RTTY mark default). See [CW Decoder](./cw-decoder.md) and [CWX Panel](./cwx-panel.md). |
| **Receive** | Frequency offset (GPSDO detection, manual calibration) and the 10 MHz reference source |
| **Filters** | Voice, CW and Digital filter sharpness (Low Latency ↔ Sharp Filters) with Auto buttons, and **Use Low Latency Filters for Digital Modes** |

### CONTROLLERS & HARDWARE

| Page | What it covers |
|------|----------------|
| **Antennas** | Local display names for antenna ports, and the **KiwiSDR RX Antennas** list. See [KiwiSDR and Web-888](./kiwisdr-and-web-888.md). |
| **Transverters** | One tab per transverter plus **+** to add one. See [XVTR (Transverters)](./xvtr-transverters.md). |
| **APD** | Adaptive predistortion external sampler per TX antenna. Only on radios that report APD support (FLEX-8000 series). |
| **USB Cables** | The radio's USB cables, each with a Cable Type of CAT, Bit, BCD, LDPA or Passthrough. See [USB Cable Management](./usb-cable-management.md). |
| **Peripherals** | Station devices AetherSDR talks to directly: tuners, amplifiers, antenna switches and wattmeters. See [Peripherals](./peripherals.md). |
| **Serial & Controllers** | USB control surfaces (Ulanzi Dial, Stream Deck+ and HID encoders), serial port and pin assignment for PTT/CW keying, the FlexControl knob, and TMate 2. See [FlexControl Tuning Knob](./flexcontrol-tuning-knob.md), [USB Control Surfaces](./usb-control-surfaces.md) and [Ulanzi Dial](./ulanzi-dial.md). Present only in builds with serial-port support. |

### ONLINE & APPEARANCE

| Page | What it covers |
|------|----------------|
| **Appearance & Behavior** | Slice Letter Display (see [Multi-Flex](./multi-flex.md)), slice colours (see [Slice Colors](./slice-colors.md)), Single-click delay and mouse-wheel behaviour. Themes are under **View → Theme**. See [Themes and Theme Editor](./themes-and-theme-editor.md). |
| **SmartLink** | Pinned SmartLink certificates. See [SmartLink Setup](./smartlink-setup.md). |
| **QRZ & Callsigns** | QRZ.com account and lookup cache. See [Callsign Lookup](./callsign-lookup.md). |

## Known issues

- On macOS, opening the **Audio** page for the first time can send the Preferences window behind the main window ([#6277](https://github.com/aethersdr/AetherSDR/issues/6277)).

## Troubleshooting

### A page you expect is missing

Radio Setup hides pages for hardware the connected radio does not have. For example, **Calibration** is hidden on a FlexRadio, and **HL2 Hardware**, **RTL Receiver** and **Droop Correction** appear only when that radio is connected.

1. Check which radio you are connected to.
2. Use **Search settings** (Ctrl+F, ⌘F on macOS) to find where the setting lives on this radio. For example, a FlexRadio calibrates on the **Receive** page.

### A control is greyed out

The connected radio or this build cannot use that control.

1. Hover the control. Its tooltip gives the reason.

### You can't find where to connect to a radio

Radio connection is not part of Radio Setup.

1. Use **File → Connect to Radio...**, or the **+** button on the title bar's radio tabs.
2. Tick **Connect to last radio on start up** in the connect panel if you want AetherSDR to connect automatically at launch.

See [First Connection](./first-connection.md) and [Manual Connection](./manual-connection.md).

## See also

- [Audio Settings](./audio-settings.md)
- [Settings and Backups](./settings-and-backups.md)
- [First Connection](./first-connection.md)
- [Menu Reference](./menu-reference.md)
