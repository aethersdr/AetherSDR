---
title: "AetherSweep"
slug: "/aethersweep"
description: "AetherSweep is AetherSDR's in-panadapter SWR sweep."
---

AetherSweep is AetherSDR's in-panadapter SWR sweep. It steps a 1 W tune
carrier across the current TX band, reads SWR at each step, and draws the
curve directly on the panadapter, with the resonance point and the usable
bandwidth marked.

## Requirements

- A connected radio, with split off.
- The slice on the band you want to sweep must be the **TX slice**, unlocked,
  with its panadapter visible.
- A supported amateur band other than **60 m**, which is channelized.
- Any amplifier in standby. AetherSweep refuses to start while a **Power
  Genius XL** is in OPERATE.

> **Other amplifiers are not checked.** AetherSweep only knows about the
> PGXL. If you have an ACOM, SPE, VK3AMP, KPA1500 or any other amplifier in
> line, put it in standby or bypass yourself before sweeping. See
> [Amplifiers](./amplifiers.md).

## Using AetherSweep

### Running a sweep

<img src="/img/screens/ant-panel-sweep.png" width="292" alt="The ANT panel open beside the overlay menu, with the ANT button lit. It shows RX ANT: ANT1, an RF Gain slider at 8 dB, a WNB button with its level at 90, then the AetherSweep buttons Start Sweep, Clear Sweep and Save CSV in amber, and a Limit range option with From and To frequency fields." />

*The ANT panel: receive antenna, RF gain and wideband noise blanker, with the AetherSweep controls below.*

1. Make the slice on the band you want to sweep the **TX slice**, with its
   panadapter visible.
2. Open the panadapter overlay's **ANT** panel.
3. Optionally tick **Limit range** and set **From** / **To** (see
   [What gets swept](#what-gets-swept)).
4. Click **Start Sweep**. The panadapter widens to show the whole band and the
   curve is drawn as the sweep runs. Press **Esc** to stop early.
5. When it finishes, the slice frequency, tune power and panadapter range are
   put back as they were.
6. **Save CSV** exports the result; **Clear Sweep** removes the overlay.

The sweep can also be started from **Tools → Start SWR Scan…**, which sweeps
the whole band on the TX slice.

### Licence confirmation

Before a sweep transmits, a modal "Antenna SWR Sweep — License Confirmation"
explains that it sends a 1 W tune carrier at several frequencies across the
band, and that you are responsible for checking the band is clear and for
complying with your licence. Tick **Remember my answer** to skip it next time
from the ANT panel. **Tools → Start SWR Scan…** always shows it.

### What gets swept

- The current band, narrowed to the active regional band plan, minus a 5 kHz
  guard at each edge.
- Fixed **20 kHz** steps, up to 260 points.
- **Limit range** (off by default) confines the sweep to **From** / **To**.
  The fields start at the band edges, and the range can only narrow the sweep,
  never extend it past the band plan.

### Reading the overlay

| Visual | Meaning |
|---|---|
| Background shading | Green for SWR 1.0–1.5, amber for 1.5–2.0, red above 2.0, with grid lines at 1.5, 2, 3, 5 and 10 |
| Amber curve and dots | Measured SWR at each step |
| Notches at each end | Sweep start and end |
| Dashed vertical line | The frequency being measured right now |
| Green caret and circle | Resonance — the lowest SWR measured |
| Bracket spans along the bottom | Bandwidth where SWR ≤ 1.5 (green) and ≤ 2.0 (amber), drawn once the sweep is complete |

The corner readout shows:

- `SWR 1.23:1` — the best SWR (with `RUN` while sweeping)
- `Res 14.185 MHz` — the resonant frequency, followed by the meter source
  (`TGXL BYPASS` or `RADIO`) when a TGXL is present
- `BW 1.5 180 kHz  2.0 320 kHz` — the bandwidth at SWR ≤ 1.5 and ≤ 2.0, after
  the sweep completes

### Saving the result

**Save CSV** in the ANT panel exports the most recent completed sweep with
the header `Frequency (Hz),SWR`. The suggested file is
`swr_sweep_<band>_<YYYYMMDD>_<HHMMSS>.csv` in your Documents folder. It is
refused while a sweep is running, and when there is no sweep to save.

### Sweeping with a TGXL

If a 4O3A Tuner Genius XL is present and in OPERATE, AetherSweep puts it into
**BYPASS** automatically before applying RF, waits for the relays to settle,
and reads SWR from the radio — so you measure the antenna and feedline, not
the tuner's match. The tuner is returned to its previous state after the
sweep. If the tuner does not confirm bypass, the sweep stops before
transmitting.

## Reference

### Limitations

- The sweep power is fixed at 1 W.
- Resolution is the 20 kHz step. Use **Limit range** to concentrate on part
  of the band.
- One slice at a time: the sweep runs on the TX slice.

### Settings

| Key | Description |
|---|---|
| `SwrSweepLicenseConfirmed` | Set when you accept the licence confirmation with **Remember my answer** |

## Troubleshooting

### A sweep refuses to start

A message names the reason. Fix the one it gives:

1. **No radio connected, or split active:** connect, or turn split off.
2. **Not the TX slice, TX slice locked, or no visible panadapter:** make the
   slice the TX slice, unlock it, and show its panadapter.
3. **Already transmitting or tuning:** stop transmit or tune first.
4. **Power Genius XL in OPERATE:** put it in STANDBY.
5. **Outside a supported amateur band, or on 60 m:** tune to another amateur
   band. 60 m is channelized and can't be swept.
6. **Band too wide for one panadapter, more than 260 points, or a limited
   range too narrow to measure:** tick **Limit range** and set a **From** /
   **To** range that is narrower, or wider if it was too narrow.

### The sweep stops before transmitting with a TGXL in line

The Tuner Genius XL did not confirm **BYPASS**, so AetherSweep stopped rather
than measure through the tuner.

1. Check the TGXL is connected and responding (see [TGXL Tuner Control](./tgxl-tuner-control.md)).
2. Start the sweep again.

### Save CSV is refused

A sweep is still running, or there is no completed sweep to save.

1. Wait for the sweep to finish, or run one, then press **Save CSV**.

## See also

- [Panadapter Controls](./panadapter-controls.md) — the overlay panels
- [TGXL Tuner Control](./tgxl-tuner-control.md) — the tuner
- [Amplifiers](./amplifiers.md)
