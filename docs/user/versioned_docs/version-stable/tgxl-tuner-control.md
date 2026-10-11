---
title: "TGXL Tuner Control"
slug: "/tgxl-tuner-control"
description: "AetherSDR controls the 4O3A Tuner Genius XL (TGXL) through the TUN applet, including manual adjustment of the tuner's Pi-network relays."
---

> **Applies to:** All radios · the radio relay and OPERATE / STANDBY / BYPASS need a FlexRadio

AetherSDR controls the 4O3A Tuner Genius XL (TGXL) through the **TUN** applet,
including manual adjustment of the tuner's Pi-network relays.

## Setup

AetherSDR can reach the TGXL two ways, and the applet shows which one it is
using with an indicator at the end of its readout row:

| Indicator | Path |
|---|---|
| `● DIRECT` | A direct TCP connection to the tuner on port 9010 |
| `● RADIO` | Through the FlexRadio, which relays the tuner's status and commands |
| `● OFFLINE` | Neither is available |

When a TGXL is connected to your FlexRadio, the radio reports its address and
AetherSDR opens the direct connection automatically. For a tuner the radio
does not report (on another subnet, over a VPN, or with a non-Flex radio),
add it in [Peripherals](./peripherals.md) (**Add → Tuner Genius XL**) with its address.

- **Connect directly when available** (on by default, in Peripherals) controls
  the automatic direct connection. Turn it off to use only the radio's relay.
- **Access code:** if the TGXL asks for authorization, enter its code in
  Peripherals. AetherSDR sends it before any other command and saves it in the
  system keychain once the tuner accepts it. See [Peripherals](./peripherals.md) for how codes
  are stored.
- If direct authorization fails, AetherSDR falls back to the radio relay and
  the indicator shows `● RADIO`.

The direct connection is what adds port detail, the antenna buttons and
manual relay control. Without it the applet still works from what the radio
relays.

**OPERATE, STANDBY and BYPASS go through the radio.** A TGXL reached by IP
only, with no FlexRadio relaying it, cannot be switched between them; pressing
those keys raises a notice saying the control is not available rather than
doing nothing silently.

## Using the tuner applet

Docked in the applet panel, the TUN applet (title "TGXL") shows:

- **Fwd Power** gauge. It scales for barefoot, Aurora or PGXL power levels,
  and uses the tuner's own peak reading, so voice peaks read correctly.
- **SWR** gauge (1.0–3.0).
- **C1**, **L** and **C2** relay bars.
- **TUNE**: starts an auto-tune. While a tune runs it reads **STOP**;
  pressing it stops the tune. The tuner stays in operate.
- **OPERATE**: cycles OPERATE → BYPASS → STANDBY.
- **ANT 1 / ANT 2 / ANT 3** antenna buttons, shown with a direct connection to
  a TGXL that has an antenna switch.

Messages from the tuner appear across the applet for as long as the tuner
shows them: after a successful tune it shows the SWR it settled on
(`Tuned SWR: 1.15:1`), and a tune that could not run says why.

Meters update at about 60 Hz while transmitting and 4 Hz while receiving.

### Popped out or on the canvas

Popped out into its own window, or placed on the [Workspace Canvas](./workspace-canvas.md), the
applet is laid out like the tuner's front panel: the two meters, a status
strip for each RF port showing what is feeding it and where it is tuned, the
C1/L/C2 relay positions as dials, and separate **STBY**, **BYP** and **TUNE**
keys. A port shows its source only while it has a live reading.

## Using manual relay control

With the direct connection active, the C1, L and C2 bars (and the dials in the
popped-out layout) are adjustable:

- **Scroll up** over a relay to increase its position by 1.
- **Scroll down** to decrease it by 1.
- The cursor changes to ↕ over an adjustable relay.

This is useful for:

- Tweaking an auto-tune result for lower SWR
- Experimenting with different matching configurations
- Matching antennas the auto-tuner struggles with

## Reference

### Relays

| Relay | Component | Range |
|-------|-----------|-------|
| C1 | Input capacitor | 0–255 |
| L | Inductor | 0–255 |
| C2 | Output capacitor | 0–255 |

### Protocol

The TGXL uses a text protocol in the same format as the SmartSDR API:

| Direction | Format |
|-----------|--------|
| TGXL → Client (greeting) | `V<version>` or, when authorization is required, `V<version> AUTH` |
| Client → TGXL | `C<seq>\|<command>\n` |
| TGXL → Client | `R<seq>\|<code>\|<body>\n` |
| TGXL → Client (push) | `S0\|state key=val ...\n` |

Authorization: `auth <code>`. Manual relay command:
`tune relay=<0\|1\|2> move=<+1\|-1>`.

## Known issues

- TUNE can fail on a remote FLEX-8600 while the Genius peripherals are connected in AetherSDR ([#6001](https://github.com/aethersdr/AetherSDR/issues/6001)).
- The TUN indicator in the status bar may not change the tuner's state, and the OPERATE cycle can disagree with the tuner's real state ([#4946](https://github.com/aethersdr/AetherSDR/issues/4946)).

## Troubleshooting

### A tune stops with "LOW RF POWER"

There was too little drive for the tuner to measure against.

1. Raise the drive.
2. Press **TUNE** again.

### Relay bars don't scroll

Relay control needs the direct connection.

1. Check the indicator reads `● DIRECT`. `● RADIO` means AetherSDR is using
   the radio's relay.
2. Check the TGXL's row in **Settings → Radio Setup... → Peripherals**.
   **Needs attention** means a rejected code or failed connection; enter the
   code and click **Connect**.
3. Make sure AetherSDR can reach TCP port 9010 on the tuner. Over SmartLink or
   a remote link, add the tuner's reachable address in Peripherals.

### TGXL not detected

The radio is not reporting the tuner, or AetherSDR cannot reach it.

1. Verify the TGXL is connected to the radio and powered on.
2. Add it manually in Peripherals if the radio does not report it.
3. Check the logs in **Help → Support & Diagnostics...**.

### OPERATE, STANDBY or BYPASS says the control is not available

These go through the radio, and no FlexRadio is relaying this tuner.

1. Connect the TGXL to your FlexRadio, and connect AetherSDR to that radio, so
   the radio relays the tuner.

## See also

- [Peripherals](./peripherals.md)
- [AetherSweep](./aethersweep.md) (automatic TGXL bypass during a sweep)
- [Amplifiers](./amplifiers.md)
