---
title: "AetherClock and GPS"
slug: "/aetherclock-and-gps"
description: "Two tools help you know where you are and what time it is."
---

Two tools help you know where you are and what time it is. **AetherClock** decodes the WWV/WWVH and WWVB time signals off the air and shows how far your computer's clock is from them. The **GPS & Station Location** window shows what a GPS-equipped radio knows: position, satellites, frequency reference and time.

## Requirements

- **AetherClock** needs a receiver tuned to WWV, WWVH or WWVB. On a FlexRadio it listens through a DAX channel; on radios without DAX (Hermes-Lite 2 and others) it uses the slice's own audio.
- **GPS & Station Location** needs a radio that reports GPS hardware, for example a FlexRadio fitted with the GPSDO option, or an IC-705. A radio without GPS shows no GPS window, indicators or controls.

## Setup

### Setting up AetherClock

AetherClock is an applet (applet-tray button **CLK**, title **AetherClock**, Station category). It is off by default.

<img src="/img/screens/aetherclock-applet.png" width="248" alt="AetherClock applet. An empty display reads no signal, above a status row with placeholder time fields. Below are Enable, Settings (expanded) and a DAX 1 selector, then the settings drawer with Station: WWV 10 MHz, a Tune slice → WWV 10 MHz button, the note dial 9.999 MHz USB, 1 kHz below carrier, and a Debug expander." />

*The AetherClock applet with its settings drawer open, before it is enabled.*

1. Open the applet and its settings drawer.
2. Pick a **Station:** preset: **WWV 2.5 / 5 / 10 / 15 / 20 MHz** or **WWVB 60 kHz**.
3. Press **Tune slice → *preset*** to put the bound slice on the right frequency (USB, 1 kHz below the carrier). This works while the decoder is stopped.
4. On a FlexRadio, choose a DAX channel in the applet's DAX chooser (**DAX Off**, **DAX 1** …). On radios without DAX the chooser is hidden.
5. Press **Enable** (it turns green while running).

On a FlexRadio with a DAX channel assigned, AetherClock decodes that slice alone, regardless of your speaker volume or mute.

## Using AetherClock

| Item | Meaning |
|---|---|
| Lock LED | Green when locked |
| Decoded UTC | The time decoded from the signal |
| Offset | How far the computer's clock is from the decoded time |
| Station tag | **WWV** or **WWVH**, shown as "--" until it is sure |
| Quality · age | Decode confidence and how old the last good decode is |
| Alignment scope | The received signal against the expected pattern |
| **Car**, **Tick**, **Frm**, **Dec**, **Vote** | The five decoding stages, from carrier to a voted time; each turns green as it succeeds |
| Verdict line | A plain-language status, for example "Locked — all stages green", "No carrier — check antenna / band / preset dial" or "Syncing frame… (marker search, up to ~2 min)" |

A debug log in the settings drawer shows the decoder's internal steps, useful when reporting a problem.

## Using the GPS dashboard

Open it from **Tools → GPS Dashboard…**, or click the GPS indicator in the status bar.

### Header cards

**GPS FIX**, **SATELLITES**, **10 MHz REFERENCE** and **TELEMETRY** (the age of the latest GPS report).

### Current Location

Position source, **Grid square**, **Latitude**, **Longitude**, the coordinates in the radio's own format, **Altitude**, **Speed** and **Course**, plus the nearest mapped address (an online OpenStreetMap lookup). Buttons: **Copy gridsquare**, **Copy address** and **Refresh address**.

### Station Map

A map of the radio's position. Use the arrow keys to pan, plus and minus to zoom, and Home to re-centre.

### Satellite Reception and Frequency Reference

Satellites **Tracked** and **Visible**, the tracking ratio, the GPS **Frequency error**, the reference **setting**, **actual** source and **lock**, and how long GPS has been locked.

### Satellite Time

UTC, local time and time zone, the radio's GPS UTC, and **Clock agreement** between the radio's GPS time and your computer. The radio's GPS supplies the time of day but not the date, so dates come from your computer. Use Clock agreement as a rough guide when looking at FT8 DT, and keep the computer synchronised with NTP for precise work.

### Radio Clock Synchronization

On radios that support it, this group shows and changes clock settings stored in the radio itself: **Enable NTP client**, the **NTP server** (with **Apply**), **Correct radio clock from GPS automatically**, and **Sync now**. On a local FLEX-8000 the window also shows a tip for using the radio as an NTP server on a trusted network.

### Where your position is used

The GPS page in **Radio Setup** holds the radio's GPS settings. Your position also feeds the [AetherMap](./psk-reporter-map.md) home location, the APRS beacon in [AetherModem Packet Radio](./aethermodem-packet-radio.md) and the distance and bearing on [Callsign Lookup](./callsign-lookup.md) cards.

## Troubleshooting

### AetherClock shows "Bound slice has no DAX channel — no audio"

On a FlexRadio, AetherClock listens through DAX, and the bound slice has no DAX channel.

1. Choose a channel in the applet's DAX chooser (**DAX 1** …) instead of **DAX Off**.

### AetherClock says "No carrier — check antenna / band / preset dial"

The slice is not hearing the time station.

1. Check the **Station:** preset matches a station you can receive.
2. Press **Tune slice → *preset*** to put the slice on the right frequency.
3. Check the antenna and try another WWV frequency.

### AetherClock stays on "Syncing frame…"

The decoder is searching for the frame marker, which can take up to about two minutes.

1. Wait for the marker search to finish before changing anything.

### There is no GPS window or indicator

The connected radio does not report GPS hardware. The GPS window only appears for a radio that has GPS, such as a FlexRadio with the GPSDO option or an IC-705.

## See also

- [Radio Setup](./radio-setup.md)
- [DAX Virtual Audio](./dax-virtual-audio.md)
- [AetherMap](./psk-reporter-map.md)
- [AetherModem Packet Radio](./aethermodem-packet-radio.md)
- [Callsign Lookup](./callsign-lookup.md)
