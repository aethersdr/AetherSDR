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

- **Settings → Controllers → FlexControl Knob & Buttons...** → Serial & Controllers
- **Settings → USB Cables...** → USB Cables
- **Tools → Configure KiwiSDR...** → Antennas

The window has no title bar. Drag any empty part of it to move it, and drag its top or bottom edge to make it taller or shorter; its width is fixed. It reopens at the height and place you left it. Close it with the round **✕** in the top-right corner, **Esc** or **Ctrl+W** (**⌘W** on macOS). It's a window of its own, with its own taskbar entry, so you can minimize it from the taskbar or park it on another monitor. **View → Frameless Window** does not apply to it.

On/off buttons (**Enabled** / **Disabled**) fill green when on. Where you pick one of a set (**DHCP** / **Static**, **Radio Side** / **Client Side**, **CWU** / **CWL**), the chosen one fills cyan. A control the connected radio cannot use is dimmed, never hidden.

## Finding a setting

Radio Setup is a searchable browser: a categorized tree of pages on the left and the selected page on the right.

<img src="/img/screens/radio-setup-search.png" width="960" alt="Radio Setup with latency typed in the search box. The left tree now shows only RECEIVE &amp; TRANSMIT with the Audio page under it, while the right side still shows the Radio page, with the serial number, radio ID and station name blacked out." />

*Typing in the search box filters the page list: here, latency matches the Audio page.*

- **Search settings** (Ctrl+F, ⌘F on macOS) filters the tree as you type. It matches page names and also common symptom words. Try *latency*, *ptt*, *calibration* or *certificate*. Press Enter to jump to the first match.
- Pages for hardware the connected radio does not have are hidden. Individual controls a radio cannot use stay visible but dimmed, with the reason in the tooltip.

## Reference

On Flex radios, **Nickname** and **Callsign** use the radio display's supported
characters: letters, digits, `.`, `,`, `/` and `-`. Spaces and other characters
are removed; callsigns are uppercased. Blank or symbol-only callsign edits keep
the previous callsign rather than replacing it. **Station Name** keeps spaces
but removes characters the radio cannot accept. The locally saved station name
is unchanged. Your callsign and station name are saved locally even if the radio
copy cannot be dispatched.

**Network → Static** requires complete IPv4 addresses and a nonzero contiguous
subnet mask. **Apply** stays disabled while a field is invalid; hover the field
to see the reason. **TX Band Settings** limits RF and tune power to 0–100 percent
and restores the radio's reported value for malformed entries.

### RADIO

On a FlexRadio, edit **Nickname** under **Settings → Radio Setup... → Radio**
and press Enter or leave the field to save an edit on the radio. An untouched
field follows the radio's current nickname and does not rename or clear it.
When the radio accepts the edit, the nickname in the station label and status
bar updates without reconnecting. If the radio rejects it, the field returns
to the confirmed nickname. A confirmed clear also empties the nickname in the
status bar. On **SmartSDR 4.3
or later**, leave the field empty to clear the saved nickname. The radio accepts
letters, digits, `.`, `,`, `/` and `-`; spaces and other characters are removed.
If no supported characters remain, SmartSDR 4.3 or later clears the nickname.
If the firmware is older or its version is not yet known, clearing is unavailable
and the field returns to the radio's current nickname. Other radio families keep
their nicknames on this computer.

| Page | What it covers |
|------|----------------|
| **Radio** | Radio information (serial, hardware version, region, options), identification (model, nickname, callsign), license info and the firmware version (**FW Version**; to update it, see [Firmware Update](./firmware-update.md)) |
| **Network** | The radio's IP address, mask, MAC and gateway; DHCP/static IP configuration; **Advanced**: Enforce Private IP Connections, **Agent Automation (MCP)** (see [Automation Bridge and MCP](./automation-bridge-and-mcp.md)), Network MTU and the VITA-49 RX buffer (see [Low Bandwidth Connections](./low-bandwidth-connections.md)) |
| **GPS** | Whether a GPS is installed, position, grid, altitude, satellites and lock status. The full dashboard is **Tools → AetherGPS...**. See [AetherClock and GPS](./aetherclock-and-gps.md). Hidden on radios without GPS hardware. |
| **Calibration** | Host-side frequency calibration in ppb, for radios that cannot calibrate themselves ([Hermes-Lite 2](./hermes-lite-2.md)). Hidden on FlexRadio, which calibrates on the Receive page. |
| **RTL Receiver** | ppm correction, IQ DC suppression and serial for an [RTL-SDR](./rtl-sdr.md) dongle. Only when one is connected. |
| **HL2 Hardware** | Board variant, dither, companion filter board, CL1 external 10 MHz reference and tuner for the [Hermes-Lite 2](./hermes-lite-2.md). Only when an HL2 is connected. |
| **ANAN Front End** | Starting sample rate, ADC choice, dither, randomization, filter-bank bypass and radio-speaker audio for the [ANAN-G2](./anan-g2.md), which apply on the next connect, plus the band-pass filter bank and the speaker amplifier mute, which also apply at once. Available while disconnected or connected to a G2. |
| **Droop Correction** | Panadapter edge-droop correction for the [ANAN-G2](./anan-g2.md). Only when one is connected. |

### RECEIVE & TRANSMIT

Open **TX Band Settings** on the **Transmit** page to configure each band's
TX outputs. On a FlexRadio with SmartSDR 4.3 or later, the **RCA off/TUNE**
column controls all three RCA keying outputs (TX1/TX2/TX3) for the
named band. The existing columns remain usable on newer firmware: **RCA TX1**,
**RCA TX2** and **RCA TX3** still set which outputs are enabled for that band.
Each new checkbox follows that band's value on the radio; it does not
follow the selected panadapter. Editing it never starts TUNE. A dimmed checkbox
has a tooltip explaining missing support, missing readback or a pending request.
After connecting or reconnecting, the column becomes available once the radio
has reported both supported firmware and the band's value, in either order.
See [TX Controls](./tx-controls.md#tune) for the output scope and older firmware's
**Inhibit during TUNE** settings.

The TX Band Settings window has no title bar. Drag any empty part of it to move it, and drag an edge or corner to resize it; it reopens at the size and place you left it. Close it with the round **✕** in the top-right corner, **Esc** or **Ctrl+W** (**⌘W** on macOS). It's a window of its own, with its own taskbar entry. **View → Frameless Window** does not apply to it.

The table shows the current TX profile's values and follows the radio while it's open. A value changed on the radio appears in the table, except in a field you are typing in. A change you make stays as you set it until the radio answers; one the radio refuses, or one the app can't send right now (for example while reconnecting), shows the radio's value again. Switching to another TX profile, reconnecting, or the radio reporting another band rebuilds the table, and text you were typing is dropped. Reopening the window after closing it also builds a fresh table.

| Page | What it covers |
|------|----------------|
| **Audio** | The radio's Line Out, Headphone and Front Speaker outputs; audio compression for SmartLink and tailnet connections; packet-loss smoothing; **Prevent system sleep while connected**; PC audio devices, Audio Boost and Audio Buffer; recording. See [Audio Settings](./audio-settings.md). |
| **Transmit** | TX timings (ACC TX, TX delay, timeout), TX REQ interlock polarity (RCA, Accessory), **PTT Mic Override** (firmware v4.3 and later; see [TX Controls](./tx-controls.md#hardware-ptt-and-the-tx-source)), Max Power, Show TX in Waterfall, Slice/TX Follow, and **TX Band Settings** (per-band RF/tune power, PTT inhibit, interlocks and RCA suppression during TUNE on firmware 4.3 and later). The Show TX in Waterfall choice is remembered per [Hermes-Lite 2](./hermes-lite-2.md); a FlexRadio owns and reports its setting. See [TX Controls](./tx-controls.md). |
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
| **Appearance & Behavior** | Slice Letter Display (see [Multi-Flex](./multi-flex.md)), slice colors (see [Slice Colors](./slice-colors.md)), Single-Click Delay and mouse-wheel behavior. Themes are under **View → Theme**. See [Themes and Theme Editor](./themes-and-theme-editor.md). |
| **SmartLink** | Pinned SmartLink certificates. See [SmartLink Setup](./smartlink-setup.md). |
| **QRZ & Callsigns** | QRZ.com account and lookup cache. See [Callsign Lookup](./callsign-lookup.md). |

## Known issues

- On macOS, opening the **Audio** page for the first time can send the Preferences window behind the main window ([#6277](https://github.com/aethersdr/AetherSDR/issues/6277)).

## Troubleshooting

### A page you expect is missing

Radio Setup hides pages for hardware the connected radio does not have. For example, **Calibration** is hidden on a FlexRadio, and **HL2 Hardware**, **RTL Receiver** and **Droop Correction** appear only when that radio is connected.

**ANAN Front End** is also listed while disconnected so you can configure the first G2 connection. Connecting another radio family hides that page.

1. Check which radio you are connected to.
2. Use **Search settings** (Ctrl+F, ⌘F on macOS) to find where the setting lives on this radio. For example, a FlexRadio calibrates on the **Receive** page.

### A control is grayed out

The connected radio or this build cannot use that control.

1. Hover the control. Its tooltip gives the reason.

### A timing edit is not applied

The **Timings (in ms)** group on **Transmit** requires a connected radio that
supports those controls. **Timeout (sec):** is entered in seconds; the other
timings are entered in milliseconds. The Demo does not implement these settings.

1. Connect to your radio before editing a timing.
2. Enter a whole number. Negative delays are treated as zero; invalid or
   excessively large entries are not sent and restore the radio's reported value.

### Calibration is interrupted by a disconnect

On **Receive → Frequency Offset**, **Start** begins radio-side calibration.
An accepted request is not a completed calibration; completion comes from the
radio's status. The reference frequency and correction reset must each be
accepted before calibration starts. A rejected or unanswered step stops the
operation. If the connection ends, the operation stops waiting and shows
**Disconnected**.

1. Reconnect to the radio.
2. Start calibration again when the reference signal is ready.

After switching radios or reconnecting, close and reopen **Radio Setup**
before editing an existing transverter. An editor from the
previous session cannot change a reused band or transverter identifier.

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
