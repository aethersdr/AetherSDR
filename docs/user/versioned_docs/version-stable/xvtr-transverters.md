---
title: "XVTR (Transverters)"
slug: "/xvtr-transverters"
description: "AetherSDR supports transverters for VHF, UHF and microwave operation through the FlexRadio's XVTR configuration and its XVTA / XVTB antenna ports."
status: "Supported"
applies_to: ["FlexRadio"]
---

:::info[Status]

**Status:** Supported · **Applies to:** FlexRadio

:::

AetherSDR supports transverters for VHF, UHF and microwave operation through
the FlexRadio's XVTR configuration and its XVTA / XVTB antenna ports.

## Setup

### Configuring a transverter

1. Open **Settings → Radio Setup... → Transverters** (or click **XVTR** in the
   panadapter's Band panel, which jumps there).
2. Click **Create New Transverter**.
3. Fill in:
   - **Name** — e.g., "2m"
   - **RF Freq (MHz)** — transverter RF frequency (e.g., 144.000)
   - **IF Freq (MHz)** — radio IF frequency (e.g., 28.000)
   - **LO Freq (MHz)** — calculated for you (RF − IF)
   - **LO Error (MHz)** — local-oscillator calibration offset
   - **RX Gain (dB)** — transverter receive gain
   - **RX Only** — disable TX for receive-only transverters
   - **Max Power (dBm)** — maximum safe TX drive
4. The entry shows **Valid** or **Invalid** as the radio checks it. **Remove**
   deletes it.

### Antenna ports

XVTR bands use the **XVTA** or **XVTB** antenna ports. The radio routes
through the transverter when these ports are selected. You can give the ports
friendly names in **Radio Setup → Antennas**; the radio still uses XVTA/XVTB.

## Using transverter bands

### Selecting a band

1. Click **Band** in the panadapter's overlay menu.
2. Your configured transverter bands appear as their own buttons in the band
   grid, between the HF bands and the utility row.
3. Click one to tune to it.

Band stacks are kept per transverter band.

### Built-in 4 m and 2 m (FLEX-6500 / FLEX-6700)

On the FLEX-6500 (4 m, Region 1) and FLEX-6700 (4 m and 2 m), **4** and **2**
band buttons appear in the band grid. They open a slice through the radio's
XVTR path, using its built-in transverter or an external one, as SmartSDR
does.

### Entering a frequency

When on an XVTR band (antenna XVTA/XVTB or frequency above 54 MHz):

- Bare integers get the decimal inserted after the 3rd digit
- `1446` → 144.6 MHz
- `14696` → 146.96 MHz
- `4401` → 440.1 MHz
- Explicit decimals work normally: `144.6` → 144.6 MHz

Direct entry works above 450 MHz.

## Reference

### Microwave bands

AetherSDR knows the microwave bands for band-stack and bookmark grouping:

| Band | Range (MHz) | Default (MHz) |
|---|---|---|
| 13 cm | 2300–2450 | 2304.100 |
| 9 cm | 3300–3500 | 3456.100 |
| 5 cm | 5650–5925 | 5760.100 |
| 3 cm | 10000–10500 | 10368.100 |

All default to USB. They need a configured transverter; they are not band-grid
buttons of their own. Common band-name aliases are accepted (for example
`70cm` for 440).

A radio can also advertise which bands it supports, and the band UI follows
it.

## Known issues

- Selecting a transverter band can put the VFO on the wrong frequency (for example 1317.100 MHz instead of 1296.100 MHz for a 1296 MHz transverter with a 28 MHz IF), and typing the right frequency snaps back ([#6197](https://github.com/aethersdr/AetherSDR/issues/6197)).
- A band change from a CAT or TCI program onto a transverter band does not recall the band stack, so the saved **XVTA**/**XVTB** antenna port is not selected and the radio can key into the wrong port ([#3543](https://github.com/aethersdr/AetherSDR/issues/3543)).

## See also

- [Radio Setup](./radio-setup.md)
- [Panadapter Controls](./panadapter-controls.md)
- [CAT Control](./cat-control.md)
- [TCI Server](./tci-server.md)
