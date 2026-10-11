---
title: "ShackSwitch"
slug: "/shackswitch"
description: "ShackSwitch is an open-source antenna / accessory switch built by Nigel Fenton."
---

ShackSwitch is an open-source antenna / accessory switch built by Nigel
Fenton. It speaks the 4O3A Antenna Genius protocol on port 9007, so AetherSDR
sets it up in [Peripherals](./peripherals.md) and gives it a dedicated **SS** applet for
one-click antenna switching.

## Setup

1. Open **Settings → Radio Setup... → Peripherals**.
2. Click **Add → ShackSwitch** and enter the device's IP address. Control
   always uses port 9007.
3. Click **Connect**. The **SS** applet appears in the applet panel
   (Antennas & Switching category).

**Connect automatically** is on by default, so AetherSDR reconnects the
ShackSwitch at startup and after a drop. Turn it off to connect only when you
click **Connect**. The **⚙ Web UI** button on the Peripherals page opens the
device's own web interface.

ShackSwitch shares the Antenna Genius connection and its credential slot, so
one of the two is connected at a time. See [Peripherals](./peripherals.md) for access codes
and removal.

## Using the ShackSwitch applet

| Area | What it shows |
|---|---|
| Status line | Connection state ("Not connected" until connected) |
| **INPUT A** | Band and antenna currently on input A |
| **INPUT B** | Band and antenna on input B (two-input devices only) |
| **ANTENNA** rows | One row per antenna, named from the device's own configuration (set in its web UI), with an **A** button and, on two-input devices, a **B** button |
| **Dummy Load:** | Which antenna port is your dummy load (None by default) |
| **Settings ⚙** | Opens the device's web interface |

Click **A** or **B** on a row to put that input on that antenna. The selected
button is lit in that input's colour (cyan for A, orange for B).

### Two-input (SO2R) mode

On a two-input device the applet shows both input cards and both button
columns. A single-input device hides input B.

If both inputs land on the same antenna:

- With a **Dummy Load** set, AetherSDR moves input B to the dummy load and
  blinks the B button on the antenna B wanted, until you move B yourself.
- Without one, the B button blinks amber on the shared antenna as a warning.

## Reference

| Key | Description |
|---|---|
| `SS_ManualIp` | ShackSwitch address |
| `SS_ControlPort` | Control port (9007) |
| `SS_DummyLoadAnt` | Antenna chosen as the dummy load (−1 = none) |
| `Peripherals.shackswitch.AutoConnect` | **Connect automatically** toggle |

These live in `AetherSDR.db`; see [Settings and Backups](./settings-and-backups.md).

## Troubleshooting

### The applet doesn't appear

The ShackSwitch is not connected.

1. Check the ShackSwitch row in **Settings → Radio Setup... → Peripherals**.
   `● OFFLINE` with **Connect automatically** off means it waits for
   **Connect**.
2. Confirm the address in the device's web UI, and that this PC can reach port
   9007 on it.

### Buttons are greyed out

The applet enables its buttons only after a successful connection.

1. Check the status word on the Peripherals page.
2. Click **Connect** if it reads `● OFFLINE`.

## See also

- [Peripherals](./peripherals.md)
- [Green Heron Everyware](./green-heron-everyware.md) for the Everyware switch
- [USB Cable Management](./usb-cable-management.md) for radio-side BCD/Bit cables that switch accessories by band
