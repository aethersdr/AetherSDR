---
title: "MQTT Station Automation"
slug: "/mqtt-station-automation"
description: "AetherSDR connects to your station automation system over MQTT, the lightweight publish/subscribe protocol used by Node-RED, Home Assistant and many shack automation tools."
---

AetherSDR connects to your station automation system over MQTT, the lightweight publish/subscribe protocol used by Node-RED, Home Assistant and many shack automation tools. You can show values from your station (rotator heading, selected antenna, amplifier state) on the panadapter, send commands with custom buttons, and let other software follow the radio through AetherSDR's own built-in topics.

Configuration lives in **Settings → MQTT...**; the **MQTT applet** is the live control surface.

## Requirements

An MQTT broker. Any broker works. Popular options:

- **Mosquitto**: lightweight, installs in seconds on most Linux distributions (`sudo apt install mosquitto` or `sudo pacman -S mosquitto`)
- **Home Assistant**: its MQTT broker add-on
- **Node-RED**: can act as a broker via the `node-red-contrib-aedes` package

AetherSDR uses the Eclipse Mosquitto client library (libmosquitto), bundled with the app. No separate client installation is needed.

For a quick local test broker:

```bash
# Install
sudo pacman -S mosquitto      # Arch
sudo apt install mosquitto    # Debian/Ubuntu

# Start (unauthenticated, local only)
mosquitto -v

# Test subscribe in another terminal
mosquitto_sub -t "test/#" -v

# Test publish
mosquitto_pub -t "test/hello" -m "world"
```

Once that works, point AetherSDR at `localhost:1883`.

## Setup

1. Open **Settings → MQTT...** (or **Settings...** in the MQTT applet).
2. On the **Broker** tab, enter the **Host** and **Port** (default `1883`), and a **User** and **Password** if your broker needs them.
3. On the **Subscriptions** tab, click **Add** for each topic you want. Tick **Display** to show a topic's latest value on the panadapter.
4. Optionally add **Publish Buttons**.
5. Open the MQTT applet from the applet panel and click **On** to connect.

The status turns green when connected. If the broker goes away, AetherSDR reconnects automatically, backing off from 5 s up to 60 s. The On/Off state is remembered, so MQTT reconnects by itself at the next launch.

The password is stored in your operating system's keychain (macOS Keychain, Windows Credential Manager, or libsecret/KWallet on Linux), never in the settings database. Builds without keychain support keep it for the current session only.

> **Upgrading from an older version:** MQTT used to be configured inside the applet, with comma-separated topics and a `*` prefix for panadapter display. Those settings are migrated automatically, once, into the MQTT Settings dialog; a `*` topic becomes a subscription with **Display** ticked. The password is moved into the OS keychain.

## Using the MQTT applet

The applet shows **On/Off**, the connection status, a log of incoming messages, and your publish buttons. Click a button to publish its payload. **Settings...** opens the MQTT Settings dialog. Antenna alias topics are subscribed automatically.

### Panadapter display

Topics with **Display** ticked show their latest value on the panadapter, so a beam heading or the selected antenna is visible without switching windows. Several displayed topics appear side by side.

## Using the built-in topics

AetherSDR publishes and subscribes to its own topics under `aethersdr/`. Each can be enabled or disabled in the MQTT Settings dialog, except the antenna-alias topics, which are always on. The full list is under [Built-in topics](#built-in-topics).

To give the radio's antenna ports friendly display names in AetherSDR, publish to the built-in `aethersdr/antenna/name/+` or `aethersdr/antenna/names` topics.

> **Transmit topics:** `cw/transmit` and `ax25/tx` key your transmitter when a message arrives. Leave them off unless you mean to transmit from automation, and secure your broker. A relay script that forwards `cw/decode` into `cw/transmit` must filter on the topic namespace, or it will loop.

## Example: rotator control via Node-RED

- Node-RED reads your rotator controller and publishes `rotator/pos` with the current heading (e.g. `240`).
- Node-RED subscribes to `rotator/cmd` and sends commands to the controller.

| AetherSDR setting | Value |
|-------|-------|
| Subscription | `rotator/pos`, **Display** ticked |
| Button: CW | topic `rotator/cmd`, payload `CW` |
| Button: CCW | topic `rotator/cmd`, payload `CCW` |
| Button: Stop | topic `rotator/cmd`, payload `STOP` |

The beam heading stays on the panadapter and you can turn the beam without leaving AetherSDR.

## Example: antenna switching

For any MQTT-connected antenna switch (Node-RED, ESP32, Raspberry Pi, …):

| AetherSDR setting | Value |
|-------|-------|
| Subscription | `ant/selected`, **Display** ticked |
| Button: Hexbeam | topic `ant/select`, payload `1` |
| Button: Vertical | topic `ant/select`, payload `2` |
| Button: Wire | topic `ant/select`, payload `3` |

## Example: SteppIR controller

| AetherSDR setting | Value |
|-------|-------|
| Subscriptions | `steppir/band` and `steppir/direction`, **Display** ticked |
| Button: Normal | topic `steppir/cmd`, payload `normal` |
| Button: 180° | topic `steppir/cmd`, payload `reverse` |
| Button: Bi-Dir | topic `steppir/cmd`, payload `bidir` |

## Reference

### MQTT Settings dialog

**Broker**

<img src="/img/screens/mqtt-settings-dialog.png" width="620" alt="MQTT Settings window with tabs Broker, Subscriptions and Publish Buttons, on Broker. Host is localhost, Port 1883, User and Password are empty, TLS has a Use TLS option and CA cert is optional, blank = system CA bundle with a Browse… button. OK, Cancel and Apply buttons sit at the bottom." />

*The MQTT Settings window with the default broker fields.*

| Field | Notes |
|-------|-------|
| **Host** / **Port** | Broker address; port defaults to 1883 |
| **User** / **Password** | Optional; leave blank for unauthenticated brokers |
| **TLS**: **Use TLS** | Encrypt the connection |
| **CA cert** | Optional CA certificate file (**Browse...**); blank uses the system CA bundle |

**Subscriptions**: a table of **Topic** and **Display** with **Add** / **Remove**. Ticking **Display** shows that topic's latest value on the panadapter. Below it, **Internal AetherSDR Topics** lists the built-in subscribe topics with an enable for each.

**Publish Buttons**: a table of **Label**, **Topic** and **Payload** with **Add** / **Remove**, up to 12 buttons, shown three per row in the applet. Below it, **Internal AetherSDR Topics** lists the built-in publish topics with an enable for each.

### Built-in topics

Published by AetherSDR:

| Topic | Default | Contents |
|-------|---------|----------|
| `aethersdr/cw/decode` | **On** | CW decoder output, one JSON message per chunk: `text`, `rx` (`false` for the decoder watching your own transmitted CW), `freq`, and `pitch_hz` / `speed_wpm` when known. Published only while the CW decoder panel is showing decoded text. |
| `aethersdr/radio/state` | Off | Radio state as JSON: `slice`, `freq`, `mode`, `tx`, `connected` (`false` on disconnect), and the RF `drive`, its ceiling `max_power_level` and `drive_confirmed`. Fields are present only when known, so key off whether a field exists, not on zero. |
| `aethersdr/ax25/rx` | Off | Received AX.25 packet frames |

Subscribed by AetherSDR:

| Topic | Default | Effect |
|-------|---------|--------|
| `aethersdr/cw/transmit` | Off | **Transmits CW** through the CWX keyer. Payload: `{"text":"de N0CALL","speed_wpm":28,"pitch_hz":600}`; speed and pitch are optional. Needs a radio with a radio-side CW keyer. |
| `aethersdr/ax25/tx` | Off | **Transmits** an AX.25 packet |
| `aethersdr/antenna/name/+` and `aethersdr/antenna/names` | Always on | Antenna display names (per port, or all at once) |

### Technical notes

- QoS 0 (fire-and-forget) is used for subscribe and publish.
- Reconnect backs off exponentially: 5 s initially, doubling up to 60 s.

## Troubleshooting

### Nothing is published on `aethersdr/cw/decode`

The topic is published only while the CW decoder panel is showing decoded text.

1. Check that `aethersdr/cw/decode` is enabled under **Internal AetherSDR Topics** in **Settings → MQTT...**.
2. Open the CW decoder panel (see [CW Decoder](./cw-decoder.md)) and make sure it is showing decoded text.

### `aethersdr/radio/state` or a transmit topic does nothing

These built-in topics are off by default.

1. Open **Settings → MQTT...** and enable the topic under **Internal AetherSDR Topics**.
2. For `aethersdr/cw/transmit`, the radio must have a radio-side CW keyer.

### The broker password is asked for again after a restart

The build has no keychain support, so the password is kept for the current session only.

1. Enter the password again in **Settings → MQTT...**, or use a build with keychain support.

## See also

- [CW Decoder](./cw-decoder.md)
- [CWX Panel](./cwx-panel.md)
- [AetherModem Packet Radio](./aethermodem-packet-radio.md)
- [Automation Bridge and MCP](./automation-bridge-and-mcp.md)
- [Green Heron Everyware](./green-heron-everyware.md)
