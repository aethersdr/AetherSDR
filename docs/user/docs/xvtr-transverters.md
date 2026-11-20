---
title: "XVTR (Transverters)"
slug: "/xvtr-transverters"
description: "AetherSDR supports transverters for VHF, UHF and microwave operation through the FlexRadio's XVTR configuration and its XVTA / XVTB antenna ports, and receive-only transverters on a networked Icom."
status: "Supported"
applies_to: ["FlexRadio", "Networked Icom (early, receive-only)"]
---

:::info[Status]

**Status:** Supported · **Applies to:** FlexRadio, Networked Icom (early, receive-only)

:::

AetherSDR supports transverters for VHF, UHF and microwave operation through
the FlexRadio's XVTR configuration and its XVTA / XVTB antenna ports. A
networked Icom keeps no transverter list, so AetherSDR holds one for it and
does the frequency conversion itself; see
[Transverters on a networked Icom](#transverters-on-a-networked-icom).

## Setup

### Configuring a transverter

<img src="/img/screens/radio-setup-transverters.png" width="679" alt="Transverters page headed FLEX-8600, with tabs 2m, 70cm, 23cm and a plus tab. The 2m tab shows Name 2m, RF Freq 144.000 MHz, IF Freq 28.000 MHz, LO Freq 116.000 MHz, LO Error 0.000000, RX Gain 0.0 dB, RX Only Disabled, Max Power 10.0 dBm, a green Valid status and a red Remove button." />

*Radio Setup, Transverters page, showing a transverter already set up on the radio.*

1. Open **Settings → Radio Setup... → Transverters** (or click **XVTR** in the
   panadapter's Band panel, which jumps there).
2. Click **Create New Transverter**.
3. Fill in:
   - **Name** — up to four characters, e.g., "2m"
   - **RF Freq (MHz)** — transverter RF frequency (e.g., 144.000)
   - **IF Freq (MHz)** — radio IF frequency (e.g., 28.000)
   - **LO Freq (MHz)** — calculated for you (RF − IF)
   - **LO Error (MHz)** — local-oscillator calibration offset
   - **RX Gain (dB)** — transverter receive gain
   - **RX Only** — disable TX for receive-only transverters
   - **Max Power (dBm)** — maximum safe TX drive
4. The entry shows **Valid** or **Invalid** as the radio checks it. **Remove**
   deletes it after the radio confirms removal.

Transverter tabs follow the radio’s reported inventory. A new tab appears when
the radio reports the transverter, including changes made by another client.
After a reconnect or replacement, edit the current tab; an old editor cannot
change a newly created transverter that reuses the same number.

### Antenna ports

XVTR bands use the **XVTA** or **XVTB** antenna ports. The radio routes
through the transverter when these ports are selected. You can give the ports
friendly names in **Radio Setup → Antennas**; the radio still uses XVTA/XVTB.

The internal antenna tuner is outside the transverter TX path. Selecting
**XVTA**, **XVTB** or **XVTR** as the TX antenna dims **ATU**, **MEM** and their
status indicators in [TX Controls](./tx-controls.md). **TUNE** remains available
to check transverter drive with a carrier; set the transverter's **Max Power
(dBm)** for its rated input.

### Transverters on a networked Icom

On a single-receiver networked Icom, **Settings → Radio Setup... →
Transverters** shows a table that AetherSDR stores for that radio. (The
dual-receiver IC-7610, IC-7760, IC-7851 and IC-9700 are not served.) The radio only ever tunes the IF;
AetherSDR adds the LO, so the VFO, the panadapter, spots, TCI and CAT show the
operating (RF) frequency.

1. Click **Add** and fill in the row:
   - **Name** — the band button's label, e.g., "QO-100". Each row needs its own name;
     **Apply** refuses two rows with the same name
   - **IF low (MHz)** / **IF high (MHz)** — the part of the radio's range the
     transverter uses, e.g., 28.500 to 29.000
   - **RX LO (MHz)** — receive LO; RF = IF + LO, e.g., 10461.000
   - **TX LO (MHz)** — transmit LO of a split up/down converter, e.g.,
     2371.500; leave it empty when transmit uses the receive LO
   - **Enabled** — untick to keep a row without using it
2. **RF range** shows the resulting band, and the transmit range when the TX LO
   differs.
3. Click **Apply**. Incomplete rows are saved but ignored.

A transverter becomes active when you tune into its RF range: by its band
button, a typed frequency or a spot. It stays active while the radio's IF stays
inside the IF range. Tuning up to 1 MHz past either end of the RF range holds
the radio at that edge. A frequency that is outside the radio's own range and
outside every transverter's RF range is refused with a notice, and the radio
does not move. Tuning to one of the radio's own frequencies, or turning the radio's
dial out of the IF range, returns to the radio's own frequencies.

Transverter bands on a networked Icom are **receive-only**. While one is
active, **MOX**, **TUNE** and the antenna tuner are refused with the notice
"The *name* transverter is receive-only in AetherSDR", because AetherSDR does
not yet limit the radio's RF power to what a transverter input takes. The
radio's own PTT, VOX and front-panel key are outside AetherSDR's control: set
the radio's power for your transverter before keying it from the radio.

Receive-only follows the **active** transverter: the one whose band you tuned
to, and only while the radio's dial stays in its IF range. On the radio's own
bands it transmits as usual, and on an IF frequency with no transverter active
AetherSDR has nothing to protect. This interlock is a stopgap. The real
protection is a **Max Power** limit per transverter, which is planned as a
follow-up and will allow transmitting on a transverter band at a safe power.

TCI clients see `tx_enable:<trx>,false;` while a transverter is active, and
`true` again when it is cleared, so a client can disable its own TX controls.

While the radio is transmitting or sending a TUNE carrier, a tune onto a
transverter band is refused with the notice "Unkey before tuning to the *name*
transverter band", and the radio stays where it is. Unkey first, then tune.

TCI reports the transmit frequency through the TX LO (`tx_frequency` is
2400.040 MHz for a 10489.540 MHz QO-100 downlink).

## Using transverter bands

### Selecting a band

1. Click **Band** in the panadapter's overlay menu.
2. Your configured transverter bands appear as their own buttons in the band
   grid, between the HF bands and the utility row. On a networked Icom they
   appear beside the radio's own bands.
3. Click one to tune to it.

The [Band applet](./panadapter-controls.md#band-applet) (button **BAND**) shows the same transverter buttons in the applet panel.

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
