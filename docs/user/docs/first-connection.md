---
title: "First Connection"
slug: "/first-connection"
description: "This page walks through connecting to a FlexRadio for the first time."
status: "Supported"
applies_to: ["FlexRadio"]
---

:::info[Status]

**Status:** Supported · **Applies to:** FlexRadio

:::

This page walks through connecting to a FlexRadio for the first time. AetherSDR also connects to other radio families, and has a demo mode that needs no radio at all; see [Supported Radios](./supported-radios.md) and [Demo Mode](./demo-mode.md).

<img src="/img/screens/main-window.png" width="1600" alt="AetherSDR main window in the default dark theme. The title bar has three radio tabs, Hermes-Lite 2 available, FLX8600 connected with callsign KK7GWY, and the Simulator, then PC Audio and two volume sliders. The panadapter spans 14.150 to 14.350 MHz with several SSB signals in the trace, the slice A VFO flag at 14.250.000 and band-plan bars marking Phone Extra and Phone General; the waterfall below shows the SSB signals as speckled vertical columns. The applet panel on the right shows the S-Meter, RX Controls, TX Controls, Phone and Phone/CW applets. The status bar along the bottom shows the radio model and firmware, the nickname FLX8600, GPS lock, CPU and memory, PA temperature and voltage, network quality, and the UTC clock." />

*The AetherSDR main window connected to a FLEX-8600 on 20 m: title bar along the top, panadapter and waterfall on the left, applet panel on the right, status bar along the bottom.*

## Requirements

- A FlexRadio transceiver (FLEX-6000, FLEX-8000 or Aurora series) powered on
- Your computer on the same network as the radio
- AetherSDR installed ([Installation](./installation.md))

## Setup

<img src="/img/screens/connect-dialog.png" width="760" alt="Connect to Radio window. Three cards across the top read On This Network (selected), Remote with SmartLink and Connect by IP. The Available radios list holds three entries: Hermes-Lite 2, ready on your local network, highlighted, with its address blacked out; FLEX-8600 FLX8600 KK7GWY, shared radio on your network via multiFLEX at AetherSDR, USA; and AetherSDR Demo, Simulator (not on the air), DEMO, ready on your local network at 127.0.0.1. A Connect Selected Radio button and checkboxes for the adaptive frame-rate throttle, connecting to the last radio on start-up, waking an Icom on connect and showing the demo simulator follow, with Connected and a Disconnect button at the bottom." />

*The Connect to Radio window, On This Network: every radio discovery found on the LAN. The addresses are blacked out.*

1. **Launch AetherSDR.** The **Connect to Radio** window opens. You can reopen it at any time with **File → Connect to Radio...**, or from the **+** button beside the title bar's radio tabs.
2. Choose **On This Network** ("Recommended for new users when the radio and computer are on the same LAN").
3. Your radio appears under **Available radios** within a few seconds.
4. Select it and click **Connect Selected Radio**.
5. The spectrum and waterfall appear, and audio starts playing through your default output device.

If no radio appears, the window shows "No local radios found yet" with **Retry Discovery**, **Remote with SmartLink**, **Connect by IP** and **Open Network Diagnostics** buttons. See [Troubleshooting](#troubleshooting).

The list can also include:
- **Hermes-Lite 2** and **ANAN-G2** radios on the same LAN, and RTL-SDR dongles plugged into this computer (on builds with the RTL-SDR backend).
- **AetherSDR Demo**, labelled "Simulator (not on the air)". It always sorts below real radios. Untick **Show the AetherSDR demo simulator** to hide it.

## What to Expect

After connecting, you'll see:
- **Spectrum** (top) showing the RF environment
- **Waterfall** (bottom) scrolling in real time
- **VFO widget** with frequency, S-meter and controls
- **Title bar** with a tab for the radio; its dot pulses with each heartbeat and turns red with "link lost" if the connection drops
- **Applet panel** (right) with the RX, TX, P/CW and other applets

Controls that the connected radio cannot use are hidden or dimmed with a reason. Continue with [Your First Session](./your-first-session.md).

## Other Ways to Connect

### Auto-connect

With **Connect to last radio on start up** checked (the default), AetherSDR reconnects to the last radio on the next launch if it is reachable. Untick it in the Connect to Radio window to choose a radio yourself each time. If startup auto-connect gives up, the Connect to Radio window opens again.

### Connect by IP (routed networks)

If your radio is on a different subnet or behind a VPN, discovery cannot see it. Choose the **Connect by IP** card, set **Radio type:**, enter **Radio IP:** and click **Connect by IP**. See [Manual Connection](./manual-connection.md).

### SmartLink (remote operation)

Choose **Remote with SmartLink** to reach a FlexRadio over the internet. See [SmartLink Setup](./smartlink-setup.md).

## Known issues

- On a FLEX-8600 on macOS, receive audio can stutter badly on the first connection until the radio is restarted ([#5700](https://github.com/aethersdr/AetherSDR/issues/5700)).
- On Windows, a FLEX-6300 that SmartSDR finds on the network may not appear under **On This Network** ([#6221](https://github.com/aethersdr/AetherSDR/issues/6221)).

## Troubleshooting

### The radio does not appear in the list

Discovery broadcasts do not cross routers or most VPNs, and guest Wi-Fi isolation, VPN software or firewall rules can block them.

1. Make sure the radio and computer are on the same subnet.
2. Check the radio is reachable: `ping <radio-ip>`.
3. Allow UDP port 4992 through the firewall (for example `sudo ufw allow 4992/udp`).
4. Click **Open Network Diagnostics** to narrow it down.
5. If the radio is on another subnet or behind a VPN, use **Connect by IP**; see [Manual Connection](./manual-connection.md).

### The radio refuses the connection

The radio rejected AetherSDR's client registration (for example with error `F3000001`). AetherSDR closes the connection, does not retry on its own, and returns to the Connect to Radio window with the radio's error.

1. Follow the message shown: typically another client must disconnect first, or the radio needs a restart.
2. If the **Connected Stations** dialog appears, select a station and click **Disconnect Station**; see [Multi-Flex](./multi-flex.md).

### Connected, but no audio

The PC audio path is off, or the wrong output device is selected.

1. Check that **PC Audio** is on in the title bar.
2. Open **Settings → Radio Setup... → Audio** and check the output device. See [Audio Settings](./audio-settings.md).
3. Check that the slice and the title-bar speaker are not muted.

### Spectrum, but no waterfall

The waterfall needs data from the radio before it starts drawing.

1. Wait a few seconds.

## See also

- [Your First Session](./your-first-session.md)
- [Supported Radios](./supported-radios.md)
- [Manual Connection](./manual-connection.md)
- [SmartLink Setup](./smartlink-setup.md)
- [Demo Mode](./demo-mode.md)
- [Troubleshooting](./troubleshooting.md)
- [Support and Logging](./support-and-logging.md)
