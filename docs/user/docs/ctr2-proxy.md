---
title: "CTR2 Proxy"
slug: "/ctr2-proxy"
description: "The CTR2 Proxy applet connects a CTR2-Max controller to your FlexRadio through AetherSDR."
status: "Early"
applies_to: ["FlexRadio"]
---

:::info[Status]

**Status:** Early · **Applies to:** FlexRadio

:::

The **CTR2 Proxy** applet connects a CTR2-Max controller to your FlexRadio
through AetherSDR. The CTR2 talks to the radio exactly as it would directly;
AetherSDR forwards every byte unchanged in both directions over its own
network route to the radio, including a VPN or tailnet route the CTR2 could
not reach by itself.

The CTR2 stays an independent radio client. The relay never passes through
AetherSDR's own command or transmit paths.

## Requirements

- AetherSDR connected to a radio that accepts more than one client, over the
  LAN or a VPN. The proxy is unavailable on SmartLink connections.
- **Wi-Fi (TCP) mode** works with current CTR2 firmware.
- **USB mode** needs CTR2 firmware with USB mode, which has not been released
  yet, and a build with USB HID support (hidapi).

## Setup

The applet's button is **CTR2**, in the **Integration** category. It is not in
the default applet bar; right-click any applet button to open the picker and
add it.

The proxy is **off at every launch** and nothing about it is saved: mode,
address and device are chosen again each session, and it only starts when you
press **Start**.

The **Mode** selector offers **Wi-Fi (TCP)** and **USB**.

<img src="/img/screens/ctr2-applet.png" width="248" alt="CTR2 Proxy applet. Mode is set to USB with a Rescan button, the USB selector reads Select CTR2 USB device, and the Radio row (blacked out here) sits beside a dimmed Start button. An amber hint reads Select the CTR2 USB device, followed by State: Stopped and traffic counters of 0 B to the radio and to the CTR2." />

*The CTR2 Proxy applet before a CTR2 is selected. The radio's address is blacked out.*

> **Check the mode every time.** On builds with USB support, USB is the
> default and the choice is not saved. USB mode waits on a CTR2 USB firmware
> release, so with current CTR2 firmware pick **Wi-Fi (TCP)** at every launch.

## Using Wi-Fi (TCP) mode

1. On the CTR2, set the radio IP address to **this PC's LAN address**.
2. In the applet, pick the **Listen** address: one of this PC's local IPv4
   addresses. The listen port is 4992; it can be changed while stopped.
3. Press **Start**.

Each CTR2 connection gets a fresh connection to the radio. Only one CTR2
connection is active at a time; extra ones are closed and counted as
rejected. Wi-Fi mode relays TCP only (no UDP discovery or UDP streams), so
features of the CTR2 that rely on UDP may not work this way.

## Using USB mode

The host side is complete: the CTR2's traffic travels in USB HID reports, and
AetherSDR starts the link, so you only pick the device and press **Start**. It
needs CTR2 firmware with USB mode, which has not been released yet.

- The **USB** device list shows candidate HID devices, with known CTR2 boards
  named and listed first. **Rescan** refreshes the list (and the local
  addresses).
- **Linux:** a udev rule for the CTR2 ships with the build. If the device
  cannot be opened, **Start** offers to install the rule
  (`/etc/udev/rules.d/70-aethersdr-ctr2.rules`) through `pkexec`, asking for
  your administrator password once. The rule only opens CTR2 controllers to
  whoever is logged in at the computer.
- **AetherKnob** (an AetherSDR controller on the Elecrow CrowPanel 2.1"
  rotary display) uses USB mode the same way. It appears in the list as
  **AetherKnob**. While it is relaying, AetherSDR also sends it a 32-bar
  spectrum of the receive audio you are hearing, after all DSP, for its
  centre display. A CTR2 never receives this.

## Reference

### Status

While running, the applet shows:

- **State:** Stopped, Listening, Connecting, Relaying, Closing or Error.
- The endpoints: the listener, the connected CTR2, and the radio.
- Byte counters in each direction (`↑ Radio`, `↓ CTR2`) with queued bytes, and
  in USB mode a UDP counter.
- The last error, kept apart from the relayed data.

Stop the proxy to change its settings.

### Limits

- It always relays to the radio AetherSDR is connected to, captured at
  **Start**, and **stops when AetherSDR disconnects or switches radios**.
- It is unavailable on SmartLink connections and on radios without
  multi-client sessions.
- It is not a VPN or tunnel; the PC must be reachable from the CTR2 and the
  radio from the PC.

## Troubleshooting

### Start is unavailable: "Connect AetherSDR to a radio first"

The proxy relays to the radio AetherSDR is connected to, and there is none.

1. Connect AetherSDR to your radio.
2. Press **Start**.

### "AetherSDR is connected through SmartLink"

The relay needs a direct LAN or VPN connection to the radio.

1. Disconnect from SmartLink.
2. Connect to the radio over the LAN or a VPN (see [Tailscale Remote Access](./tailscale-remote-access.md)).
3. Press **Start**.

### "This radio does not accept another client alongside AetherSDR"

The connected radio has no multi-client sessions, so the CTR2 cannot join as a
second client. The proxy cannot be used with this radio.

### USB mode is dimmed: "This build has no USB HID support (hidapi)"

The build was made without hidapi.

1. Pick **Wi-Fi (TCP)** mode instead.

### The proxy stopped on its own

It stops when AetherSDR disconnects or switches radios.

1. Reconnect AetherSDR to the radio.
2. Check the mode and settings again, then press **Start**.

### Settings are locked: "Stop the proxy to change its settings"

The proxy is running.

1. Press **Stop**.
2. Change the mode, address or device, then press **Start**.

## See also

- [Multi-Flex](./multi-flex.md)
- [MIDI Controller Mapping](./midi-controller-mapping.md)
- [Tailscale Remote Access](./tailscale-remote-access.md)
- [docs/ctr2-tcp-proxy-design.md](https://github.com/aethersdr/AetherSDR/blob/main/docs/ctr2-tcp-proxy-design.md): Wi-Fi relay and operator controls
- [docs/ctr2-usb-relay-design.md](https://github.com/aethersdr/AetherSDR/blob/main/docs/ctr2-usb-relay-design.md): USB link format and firmware status
