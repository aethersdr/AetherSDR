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

It can relay several controllers at once: CTR2s on Wi-Fi and CTR2s on USB,
in any mix. Each one stays an independent radio client with its own
connection to the radio, so the radio's own limit on connected clients
applies. The relay never passes through AetherSDR's own command or transmit
paths.

## Requirements

- AetherSDR connected to a radio that accepts more than one client, over the
  LAN or a VPN. The proxy is unavailable on SmartLink connections.
- **Wi-Fi (TCP)** works with current CTR2 firmware.
- **USB** needs CTR2 firmware with USB mode, which has not been released
  yet, and a build with USB HID support (hidapi).

## Setup

The applet's button is **CTR2**, in the **Integration** category. It is not in
the default applet bar; right-click any applet button to open the picker and
add it.

The proxy is **off at every launch** and nothing about it is saved: Wi-Fi,
the listen address and the USB devices are chosen again each session, and it
only starts when you press **Start**.

The applet has a **Wi-Fi** checkbox with its listen address and port, and a
**USB** list with a checkbox for each USB HID device. **Start** relays
everything that is checked, all at once; **Stop** stops all of it.

<img src="/img/screens/ctr2-applet.png" width="248" alt="CTR2 Proxy applet. An unchecked Wi-Fi checkbox sits beside a dimmed listen address selector, port 4992 and a Rescan button. Below, the USB list has checkboxes for a CTR2-Max, an AetherKnob (both checked) and an unchecked touchpad. The Radio row reads Not connected beside a dimmed Start button, an amber hint reads Connect AetherSDR to a radio first, and the state reads Stopped." />

*The CTR2 Proxy applet with two USB controllers checked, before AetherSDR connects to a radio.*

> **Check Wi-Fi every time.** On builds with USB support, Wi-Fi starts
> unchecked and every recognized CTR2 on USB starts checked; the choices are
> not saved. USB waits on a CTR2 USB firmware release, so with current CTR2
> firmware check **Wi-Fi** at every launch.

## Using Wi-Fi (TCP)

1. On each CTR2, set the radio IP address to **this PC's LAN address**.
2. In the applet, check **Wi-Fi** and pick the listen address: one of this
   PC's local IPv4 addresses. The listen port is 4992; it can be changed
   while stopped.
3. Press **Start**.

Every CTR2 that connects gets its own fresh connection to the radio, and its
bytes never reach any other CTR2's connection. Up to eight CTR2s can be
connected over Wi-Fi at once; more are closed and counted as rejected.
Wi-Fi relays TCP only (no UDP discovery or UDP streams), so features of the
CTR2 that rely on UDP may not work this way.

## Using USB

The host side is complete: the CTR2's traffic travels in USB HID reports, and
AetherSDR starts the link, so you only pick the device and press **Start**. It
needs CTR2 firmware with USB mode, which has not been released yet.

- The **USB** list shows candidate HID devices, with known CTR2 boards named
  and listed first. Every recognized CTR2 is checked when it first appears;
  uncheck any you do not want relayed. Each checked device gets its own
  connection to the radio, and one that fails (or is unplugged) does not stop
  the others. **Rescan** refreshes the list (and the local addresses).
- **Linux:** a udev rule for the CTR2 ships with the build. If a checked
  device cannot be opened, **Start** offers to install the rule
  (`/etc/udev/rules.d/70-aethersdr-ctr2.rules`) through `pkexec`, asking for
  your administrator password once. The rule only opens CTR2 controllers to
  whoever is logged in at the computer.
- **AetherKnob** (an AetherSDR controller on the Elecrow CrowPanel 2.1"
  rotary display) uses USB mode the same way. It appears in the list as
  **AetherKnob**. While it is relaying, AetherSDR also sends it a 32-bar
  spectrum for its centre display. While receiving it shows the audio you
  are hearing (after the radio's DSP, AetherSDR's noise reduction and the
  RX chain up to the EQ) across the active slice's filter width, for
  example 20–600 Hz for a 600 Hz CW filter. While transmitting it shows your
  transmit audio across the TX filter width. Either way it reaches at most
  12 kHz, and the bars are spaced logarithmically in frequency, as on an
  audio analyzer, so the low end gets more detail. Every relaying AetherKnob
  receives it; a CTR2 never does.

## Reference

### Status

While running, the applet shows **State**: Stopped, or Running with the number
of controllers relaying. Below it is one row per link:

- **Wi-Fi** with the listen address: Listening (with how many CTR2s are
  connected) or Error, and the number of rejected connections.
- **CTR2** with each connected Wi-Fi CTR2's address.
- Each checked **USB** device: Waiting for CTR2, Connecting, Relaying,
  Closing or Error.

Each CTR2 row has byte counters in each direction (`↑ Radio`, `↓ CTR2`) with
queued bytes, USB rows add a UDP counter, and every row shows its last error,
kept apart from the relayed data.

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

### The USB list reads "This build has no USB HID support (hidapi)"

The build was made without hidapi.

1. Check **Wi-Fi** instead.

### One link shows Error while the others relay

Each controller runs on its own, so one failure leaves the rest running. The
row shows why (for example, a USB device that was unplugged or could not be
opened).

1. Fix the cause, for example plug the device back in.
2. Press **Stop**, then **Start** to bring every checked link up again.

### The proxy stopped on its own

It stops when AetherSDR disconnects or switches radios.

1. Reconnect AetherSDR to the radio.
2. Check Wi-Fi and the USB devices again, then press **Start**.

### Settings are locked: "Stop the proxy to change its settings"

The proxy is running.

1. Press **Stop**.
2. Change Wi-Fi, the address or the USB devices, then press **Start**.

## See also

- [Multi-Flex](./multi-flex.md)
- [MIDI Controller Mapping](./midi-controller-mapping.md)
- [Tailscale Remote Access](./tailscale-remote-access.md)
- [docs/ctr2-tcp-proxy-design.md](https://github.com/aethersdr/AetherSDR/blob/main/docs/ctr2-tcp-proxy-design.md): Wi-Fi relay and operator controls
- [docs/ctr2-usb-relay-design.md](https://github.com/aethersdr/AetherSDR/blob/main/docs/ctr2-usb-relay-design.md): USB link format and firmware status
