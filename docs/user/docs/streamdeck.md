---
title: "StreamDeck"
slug: "/streamdeck"
description: "AetherSDR drives the Elgato Stream Deck+ directly over USB, with no Elgato software and no plugin."
---

AetherSDR drives the **Elgato Stream Deck+** directly over USB, with no Elgato software and no plugin. Its eight LCD keys show their assigned action, and its four dials tune, adjust RIT/XIT, volume, power and more. Other Stream Deck models are not driven natively; use them through the control-surface software of your choice talking to the [TCI Server](./tci-server.md) or the [Automation Bridge and MCP](./automation-bridge-and-mcp.md).

> **Upgrading from an older version:** the Elgato Stream Deck and StreamController plugins are no longer shipped with AetherSDR ([#5600](https://github.com/aethersdr/AetherSDR/issues/5600)). If you used them, set up a Stream Deck+ natively (below), or point your Stream Deck software at TCI or the automation bridge.

## Requirements

- An Elgato Stream Deck+.
- A build with HID support (hidapi).

## Setup

1. Open **Settings → Radio Setup... → Serial & Controllers**.
2. In **USB Control Surfaces**, tick **Enable HID encoders / StreamDeck+ (RC-28, PowerMate, ShuttleXpress, …)**.
3. **macOS:** allow AetherSDR under **System Settings → Privacy & Security → Input Monitoring** when asked.
4. **Linux:** install the udev rule from the source tree so your user can open the device:
   ```bash
   sudo cp packaging/linux/60-hid-encoders.rules /etc/udev/rules.d/
   sudo udevadm control --reload-rules && sudo udevadm trigger
   ```
   Then unplug and replug the Stream Deck+.

## Using the LCD keys

**StreamDeck+ LCD Button Actions** assigns an action to each of the 8 keys (**Key 1–8**). The key's image updates on the device to show its action. All keys start as **None**.

## Using the dials

**HID Encoder / StreamDeck+ Encoders** sets what each dial does when turned, and **StreamDeck+ Encoder Push Actions** sets what pressing each dial does. Defaults and choices are under [Reference](#reference).

### Touch strip

The touch strip above the dials shows each dial's turn and push labels, with ON/OFF state for dials assigned to RIT or XIT.

## Using other Stream Deck models

Other Stream Deck models (MK.2, XL, Mini, Pedal and so on) can still control AetherSDR, through software you choose. AetherSDR provides the protocol, not the button layer:

- **[TCI Server](./tci-server.md)**: the network protocol many radio-control plugins already speak.
- **[Automation Bridge and MCP](./automation-bridge-and-mcp.md)**: a local socket with verbs for reading state and operating controls.

## Reference

### Key actions

None, Toggle TX (MOX), Toggle Tune, Toggle RIT on/off, Toggle XIT on/off, Clear RIT offset, Clear XIT offset, Step Size Up, Step Size Down, Toggle Mute, Toggle Slice Lock, Toggle APF, Cycle AGC Mode, Toggle Band Zoom, Toggle Segment Zoom, Next Slice, Previous Slice, Volume Up (+5), Volume Down (−5), Toggle Split, Monitor TX Frequency.

### Dial turn actions

| Dial | Default |
|------|---------|
| Encoder 1 | Tune Slice |
| Encoder 2 | RIT (Receive Incremental Tuning) |
| Encoder 3 | XIT (Transmit Incremental Tuning) |
| Encoder 4 | Master Volume |

Other choices: Slice Audio Volume, Headphone Volume, AGC Threshold, APF Level, CW Speed, RF Power, None.

### Dial push actions

| Dial | Default |
|------|---------|
| Encoder 1 push | Cycle Tuning Step |
| Encoder 2 push | Toggle RIT on/off |
| Encoder 3 push | Toggle XIT on/off |
| Encoder 4 push | None |

Other choices: Toggle TX (MOX), Toggle Mute, Lock Slice.

## Known issues

- Volume up/down actions start from the saved master volume, not the level of the audio path you are hearing, so the first step can jump ([#6132](https://github.com/aethersdr/AetherSDR/issues/6132)).

## Troubleshooting

### The Stream Deck+ does nothing

HID encoders are off by default.

1. Check that **Enable HID encoders / StreamDeck+** is ticked in **Settings → Radio Setup... → Serial & Controllers**. If the checkbox is missing, this build has no HID support.
2. Turn on the **Ext Devices** log category (see [Support and Logging](./support-and-logging.md)) to see HID connection events.

### Linux: the device is not found

Your user cannot open the HID device.

1. Install the udev rule (see [Setup](#setup)).
2. Unplug and replug the Stream Deck+.

### macOS: no response

AetherSDR does not have the Input Monitoring permission.

1. Allow AetherSDR under **System Settings → Privacy & Security → Input Monitoring**.

## See also

- [USB Control Surfaces](./usb-control-surfaces.md): RC-28, PowerMate, Contour Shuttle and serial keying
- [Ulanzi Dial](./ulanzi-dial.md)
- [MIDI Controller Mapping](./midi-controller-mapping.md)
- [TCI Server](./tci-server.md)
