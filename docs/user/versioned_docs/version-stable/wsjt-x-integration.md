---
title: "WSJT-X Integration"
slug: "/wsjt-x-integration"
description: "WSJT-X (and JTDX, JS8Call and similar programs) needs two things from AetherSDR: rig control (frequency, mode, PTT) and audio."
---

WSJT-X (and JTDX, JS8Call and similar programs) needs two things from AetherSDR: **rig control** (frequency, mode, PTT) and **audio**. There are two ways to provide them:

| Route | Rig control | Audio | Platforms |
|---|---|---|---|
| **TCI** (simplest) | [TCI Server](./tci-server.md) | Same TCI connection | Linux, macOS, Windows |
| **CAT + DAX** | [CAT Control](./cat-control.md) (Hamlib NET rigctl) | [DAX Virtual Audio](./dax-virtual-audio.md) devices | Linux and macOS (FlexRadio) |

On Windows AetherSDR does not ship a DAX audio driver, so use TCI there (or FlexRadio's own SmartSDR DAX).

## Setup

### Option 1: TCI

1. In AetherSDR, enable the TCI server: **Settings → Autostart TCI with AetherSDR**, or the **Enabled** button in the **TCI** applet. The default port is **50001**.
2. In WSJT-X, open **File → Settings → Radio**:
   - **Rig:** `Expert Electronics (TCI)` (needs a WSJT-X build with TCI support, such as WSJT-X Improved or WSJT-X 3.x)
   - **Network Server:** `localhost:50001`
   - **PTT Method:** `CAT`
3. Click **Test CAT**; it turns green.

Audio flows over the TCI connection, so no sound-card devices need setting up. PC Audio does not have to be on in AetherSDR for WSJT-X to receive audio. WSJT-X's **Pwr** slider acts as a digital attenuator on the transmitted audio.

Several WSJT-X instances can share one TCI server. Each one keys the slice it asked for, and a refused PTT is reported back so WSJT-X shows "TCI failed to set ptt" instead of keying the wrong slice. See [TCI Server](./tci-server.md).

### Option 2: CAT and DAX (Linux and macOS)

#### 1. AetherSDR

1. Put the slice in **DIGU** mode.
2. Give the slice a DAX channel: VFO widget → **DAX** tab → channel **1**.
3. Turn on **DAX**: the **Enabled** button in the **DAX** applet, or **Settings → Autostart DAX with AetherSDR**.
4. Turn on **CAT**: pop out the **CAT** applet, enable a **Rigctld** port (port **4532** on a fresh install) and switch the master **Enabled** on, or use **Settings → Autostart CAT with AetherSDR**.

#### 2. WSJT-X audio

**File → Settings → Audio**:

- **Soundcard Input:** **AetherSDR DAX 1**
- **Soundcard Output:** **AetherSDR TX**

#### 3. WSJT-X radio

**File → Settings → Radio**:

- **Rig:** `Hamlib NET rigctl`
- **Network Server:** `127.0.0.1:4532`
- **PTT Method:** `CAT`

Click **Test CAT**; it turns green. The test can take a while: WSJT-X runs a test sequence on first connect.

#### 4. Verify

- FT8 signals appear in the WSJT-X waterfall, and the **DAX 1** meter in the DAX applet moves.
- Click **Tune** in WSJT-X: the radio keys and the TX applet shows power.

## Using WSJT-X with AetherSDR

### Several instances at once

Run one WSJT-X (or JTDX, JS8Call, fldigi …) per slice. Give each its own CAT port with a different **VFO A** slice, and its own DAX channel. For example:

| Program | CAT port | VFO A | Audio input |
|---|---|---|---|
| WSJT-X #1 | 4532 | A | AetherSDR DAX 1 |
| WSJT-X #2 | 4533 | B | AetherSDR DAX 2 |

Split is reported per CAT client, so every instance can use **Split Operation: Fake It**. Band changes from WSJT-X move the panadapter when the new frequency is off-screen or on another band; a tune within the visible span moves only the slice.

### Decode spotting

AetherSDR can listen to WSJT-X's UDP decode messages and put the stations it decodes on the panadapter. Each spot lands on the band of the WSJT-X instance that reported it. See the WSJT-X tab on the [SpotHub](./spothub.md) page for filters (CQ, CQ POTA, Calling Me) and colours.

## Known issues

- FT8 transmit audio can drop out for about half a second roughly one second into each transmission ([#5469](https://github.com/aethersdr/AetherSDR/issues/5469)).
- FT8 transmitted on slice B can show heavy ALC and be decoded far less often than the same signal on slice A ([#5340](https://github.com/aethersdr/AetherSDR/issues/5340)).
- Over TCI, the mic profile, **MIC** level and **PROC** in the Phone/CW applet change the transmitted digital signal ([#5844](https://github.com/aethersdr/AetherSDR/issues/5844)).
- Over TCI, the transmitted FT8 signal can show random jumps ([#5133](https://github.com/aethersdr/AetherSDR/issues/5133)).
- TCI receive audio can stop after moving to a voice frequency with DAX off and back to a data frequency with DAX on ([#4824](https://github.com/aethersdr/AetherSDR/issues/4824)).
- On macOS, DAX transmit can stop sending audio to the radio until AetherSDR is restarted ([#5870](https://github.com/aethersdr/AetherSDR/issues/5870)).

## Troubleshooting

### Nothing decodes

The slice is not delivering audio to WSJT-X.

1. Check the slice is in **DIGU**.
2. With CAT and DAX: check the slice has a DAX channel, and that WSJT-X's input is that DAX device (not your microphone).
3. With TCI: check WSJT-X is connected to the right port.

### PTT does nothing

WSJT-X is not keying through CAT, or a different slice holds TX.

1. Set WSJT-X's **PTT Method** to `CAT`.
2. Make sure the intended slice holds TX (red TX badge).

### The frequency does not follow WSJT-X

WSJT-X is not talking to a running CAT port.

1. Check that the port in WSJT-X matches an enabled CAT port.
2. Check that the master CAT switch is on.

### WSJT-X shows "Error in Sound Input"

DAX was switched off while WSJT-X was using it.

1. Turn DAX back on.
2. Restart WSJT-X.

### Transmit audio does not reach the radio

WSJT-X sends to the wrong device, or the TX slice is not in a digital mode.

1. Set WSJT-X's **Soundcard Output** to **AetherSDR TX**.
2. Put the TX slice in **DIGU**.

## See also

- [CAT Control](./cat-control.md)
- [DAX Virtual Audio](./dax-virtual-audio.md)
- [TCI Server](./tci-server.md)
- [SpotHub](./spothub.md)
- [PSK Reporter Map](./psk-reporter-map.md)
- [WSJT-X home page](https://wsjt.sourceforge.io/wsjtx.html)
