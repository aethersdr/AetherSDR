---
title: "D-STAR (ThumbDV)"
slug: "/d-star-thumbdv"
description: "AetherSDR can operate D-STAR digital voice on a FlexRadio using a ThumbDV or DV3000U AMBE vocoder plugged into your computer."
status: "Supported"
applies_to: ["FlexRadio"]
---

:::info[Status]

**Status:** Supported · **Applies to:** FlexRadio

:::

AetherSDR can operate D-STAR digital voice on a FlexRadio using a **ThumbDV** or **DV3000U** AMBE vocoder plugged into **your computer**. A small bundled helper, `aether-dv-waveform`, registers a **DSTR** mode with the radio, does the D-STAR framing on the PC, and uses the ThumbDV for voice encode and decode. The ThumbDV stays on the computer running AetherSDR; it does not go into the radio.

*D-STAR is a registered trademark of Icom Inc. AetherSDR is not affiliated with or endorsed by Icom Inc.*

## Requirements

- **A FlexRadio with waveform support**, reached directly on your network. The helper uses the Flex waveform interface, so it does not run over SmartLink (see [SmartLink Setup](./smartlink-setup.md)), and it is not available on other radio families. On a radio without waveform support the D-STAR controls are hidden.
- **A ThumbDV or DV3000U** on a USB port of the computer. AetherSDR includes no software AMBE vocoder.
- **A build that includes the `aether-dv-waveform` helper.** In a build without it, the DSTR mode and the D-STAR controls are not shown.
- **Serial-port access to the ThumbDV:**
  - **Linux:** official packages install the udev rule `70-aethersdr-thumbdv.rules`, which gives the desktop user access to the ThumbDV (FTDI `0403:6015`). Unplug and replug the ThumbDV after installing. On systems without systemd-logind, add yourself to the serial-device group (usually `dialout` or `uucp`) and log out and back in.
  - **Windows:** the ThumbDV appears as a `COMn` port. The helper opens it exclusively, so close any other program using it. The installer adds the helper's firewall rule.
  - **macOS:** the ThumbDV appears as `/dev/cu.usbserial-*`.

## Setup

Open **Tools → Waveforms...** and find the **D-STAR** card under **Local Digital Voice**.

| Control | What it does |
|---|---|
| **ThumbDV device** | Pick the ThumbDV's serial port, or type it (for example `COM3`, `/dev/ttyUSB0`, `/dev/cu.usbserial-…`). **Refresh** rescans. On Linux the stable `/dev/serial/by-id/…` name is preferred. |
| **Auto-start** | Start the D-STAR service automatically. |
| **Station and routing** | **MYCALL**, **Suffix**, **URCALL**, **RPT1**, **RPT2** and a 20-character **Message**. |
| **Advanced** | The helper's executable path. Leave it blank to use the bundled helper. |

- A ThumbDV is only saved once it has answered with a valid DV3000 reply, so a wrong serial port is never remembered by mistake.
- **MYCALL** defaults to the radio's callsign. It must be 3–8 letters and digits, containing at least one of each. Suffix, routing and message fields are checked before the helper starts, so a malformed header is never transmitted.
- The default route is **CQCQCQ / DIRECT / DIRECT** (URCALL / RPT1 / RPT2).

## Using D-STAR

### Choose DSTR on a slice

Set the slice mode to **DSTR**. DSTR runs over the radio's DFM path, so the VFO flag's FM options apply: repeater offset, direction, simplex and reverse are shown, and CTCSS is hidden.

Only one digital-voice slice can be active at a time.

### The D-STAR tab in AetherModem

Open **Tools → AetherModem...** and select the **D-STAR** tab. The header shows the service state, which slice is in DSTR (or **No DSTR slice**), and a **Start / Stop** button for the service.

**Station & Route**

| Field | What it does |
|---|---|
| **MYCALL** / **Suffix** | Your callsign and optional 4-character suffix. |
| **From** | **Direct** or **Repeater**. |
| **To** | **Local CQ**, **Specific station**, **Repeater area** or **Custom**. |
| **Access repeater** | Repeater callsign and module, when going through a repeater. |
| **Destination station** | Destination callsign and module, when calling a specific station or repeater area. |
| **Advanced route details** | The raw **URCALL**, **RPT1** and **RPT2** fields, plus the helper executable. |

**Voice Service** shows the vocoder (**ThumbDV / DV3000**), the **ThumbDV device** with **Refresh**, and **Autostart at launch**.

**D-STAR Traffic** logs D-STAR activity (station, destination, URCALL / RPT1 / RPT2 and slice). Use the search box to filter it, or clear it. Below the log, type a **TX MESSAGE** of up to 20 characters and click **Set TX Message**; the **ACTIVE** line shows the message currently being sent.

The footer shows the **MODEM** state, the **ROUTE** and recent **ACTIVITY**.

### Transmitting

- D-STAR is sent **only when you key the DSTR slice** (MOX, PTT or VOX). AetherSDR never keys the radio to start the service.
- A **TUNE** carrier stays a plain tune carrier; the helper does not send D-STAR data during TUNE.

### Controllers and shortcuts

DSTR is available as a mode shortcut and as a MIDI mode action. While the D-STAR helper is not running, **Mode Up / Mode Down** on MIDI controllers skip over DSTR instead of stopping on it.

## Known issues

- Received D-STAR audio is silent or stops after a short time, and transmissions are not recognised as D-STAR by the far station ([#4277](https://github.com/aethersdr/AetherSDR/issues/4277)).
- A DV3000U ThumbDV is not detected on macOS ([#5780](https://github.com/aethersdr/AetherSDR/issues/5780)).

## Troubleshooting

### No D-STAR card or tab, and no DSTR mode

The build has no `aether-dv-waveform` helper, or the connected radio has no waveform support.

1. Use a build that includes the helper.
2. Connect to a FlexRadio with waveform support, directly on your network.

### The D-STAR service will not start

You are connected over SmartLink, which the helper does not support, or MYCALL is missing or invalid.

1. Connect to the radio directly on your network instead of through SmartLink.
2. Check **MYCALL**: 3–8 letters and digits, with at least one of each.

### The ThumbDV is "in use"

Another program has the serial port open. The helper opens it exclusively.

1. Close the other program using the port.
2. Start the D-STAR service again.

### Access denied to the ThumbDV on Linux

Your user has no permission on the serial device.

1. Install the official package, which adds the udev rule `70-aethersdr-thumbdv.rules`.
2. Unplug and replug the ThumbDV.
3. On systems without systemd-logind, add yourself to `dialout` or `uucp`, then log out and back in.

## See also

- [AetherModem Packet Radio](./aethermodem-packet-radio.md)
- [MIDI Controller Mapping](./midi-controller-mapping.md)
- [SmartLink Setup](./smartlink-setup.md)
- [FlexRadio](./flexradio.md)
- Design and protocol notes: [docs/architecture/digital-voice-thumbdv-waveform.md](https://github.com/aethersdr/AetherSDR/blob/main/docs/architecture/digital-voice-thumbdv-waveform.md)
