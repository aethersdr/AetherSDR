---
title: "Green Heron Everyware"
slug: "/green-heron-everyware"
description: "The GHE applet selects the antenna on a Green Heron Everyware remote antenna switch and turns a rotator attached to the same station."
---

The **GHE** applet selects the antenna on a Green Heron *Everyware* remote
antenna switch and turns a rotator attached to the same station. It talks to
the Everyware server directly over TCP, knows nothing about the radio, and
works with any radio connected, or none.

The switch and the rotator arrive on one connection, which is why they share
one applet.

## Requirements

The address you enter is the Everyware **server**: a service running on a PC
with the switch hardware attached to it over serial, not a switch. One server
can present several switches (for example `AS-84F-1` to `AS-84F-4`), each with
the same set of antenna ports.

## Setup

The GHE applet is in the **Antennas & Switching** category. It is configured
in the applet itself, not in [Peripherals](./peripherals.md).

<img src="/img/screens/ghe-applet.png" width="248" alt="Green Heron applet. An IP field showing the placeholder 192.0.2.10, an empty Port field, an empty Switch field with a Connect button, and the status Not connected." />

*The Green Heron applet before a server is configured.*

| Field | Meaning |
|---|---|
| **IP** | Address of the Everyware server |
| **Port** | Its TCP port; **10000** unless the installation moved it |
| **Switch** | Which switch feeds this radio. Filled from the server's roster once connected, and remembered |

Click **Connect**. The status line reads "Connected — waiting for roster",
then "Connected — waiting for switch state" until the server reports.

## Using the antenna rows

A radio is wired to one switch, so the applet shows only that switch's
antennas, one row per port:

| Row | Meaning |
|---|---|
| `Beam-20  ·  ON` (lit) | The antenna the **device** reports this switch is on |
| `Beam-15  ·  in use by AS-84F-3` (greyed) | Held by another switch; an antenna feeds one switch at a time, so the device would refuse it |
| `EFHW-40` | Free: click to select it |

Nothing is assumed from a click. The row lights only when the device reports
the change, which it does within about an eighth of a second. A relay that
fails to move shows up as a row that does not light.

When the link drops, the last known state stays on screen marked stale
("Reconnecting — state is stale") and the rows are disabled until it comes
back.

## Using the rotator

The rotator section appears under the antennas **only while the device is
reporting a heading**:

```
Rotor            62.9° · asked 64.3° · Δ1.4°
[ 64.3        ]  [ Turn ]
```

- Type a heading in degrees, then press **Turn** or Enter. Typing alone sends
  nothing.
- The readout is the **reported** heading. After a turn it adds what was asked
  and the difference. It never says "on target": the measured sensor noise at
  rest is about ±3.8°, larger than any useful arrival threshold.
- There is no stop or park command in the protocol, so a rotation that has
  started cannot be cancelled from AetherSDR. Use the rotator controller.
- If the server reports more than one rotator, a **Rotator** selector picks
  which one this applet drives.
- The section disappears when the rotator stops reporting ("The rotator has
  stopped reporting a heading").

Popped out into its own window, the applet adds a compass rose under the Turn
row. The needle is the reported heading; a dashed tick shows the asked
heading. The rose is display-only: clicking it does nothing, by design.

## Known issues

- If an Everyware server restarts and announces its switches in a different order, the applet can name the wrong switch as holding an antenna ([#5250](https://github.com/aethersdr/AetherSDR/issues/5250)).

## Troubleshooting

### Re-sending a heading does not correct a small error

The heading is inside the controller's deadband, so the same request does
nothing.

1. Type a slightly different heading.
2. Press **Turn**.

### The antenna rows are greyed out and marked stale

The link to the Everyware server has dropped. The applet shows "Reconnecting —
state is stale" and keeps the last known state on screen.

1. Wait for the link to come back; the rows are enabled again when it does.
2. If it does not, check that the server is running and that this PC can reach
   its **IP** and **Port**.

## See also

- [ShackSwitch](./shackswitch.md)
- [Peripherals](./peripherals.md)
- [MQTT Station Automation](./mqtt-station-automation.md)
- [docs/green-heron-everyware.md](https://github.com/aethersdr/AetherSDR/blob/main/docs/green-heron-everyware.md): more detail, including the protocol notes
