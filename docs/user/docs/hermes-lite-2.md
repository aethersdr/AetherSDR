---
title: "Hermes-Lite 2"
slug: "/hermes-lite-2"
description: "AetherSDR can operate a Hermes-Lite 2 (HL2) over openHPSDR Protocol 1."
status: "Experimental"
applies_to: ["Hermes-Lite 2"]
---

:::info[Status]

**Status:** Experimental · **Applies to:** Hermes-Lite 2

:::

AetherSDR can operate a **Hermes-Lite 2** (HL2) over openHPSDR Protocol 1. Support is **experimental**: the radio receives and transmits, but FlexRadio remains AetherSDR's supported target (see [Supported Radios](./supported-radios.md)). An **EXPERIMENTAL** badge shows in the title bar while an HL2 is connected.

The HL2 has no on-board DSP, so AetherSDR does the demodulation, filtering, AGC, noise blanking and transmit modulation on your computer, using WDSP.

## Where to go next

- [Before You Transmit](./before-you-transmit.md) and [TX Controls](./tx-controls.md): SSB voice, CW, TUNE and data.
- [Aetherial Audio](./aetherial-audio.md): the transmit EQ, PROC and ALC that act on the HL2.
- [TCI Server](./tci-server.md) and [WSJT-X Integration](./wsjt-x-integration.md): FT8, WSPR and other data modes.
- [CAT Control](./cat-control.md): rigctl control, including RF gain.
- [CW Decoder](./cw-decoder.md), [RTTY Operation](./rtty-operation.md) and [AetherModem Packet Radio](./aethermodem-packet-radio.md).
- [TNF (Tracking Notch Filters)](./tnf-tracking-notch-filters.md) and [DSP Noise Mitigation](./dsp-noise-mitigation.md).
- [Memory Channels](./memory-channels.md): stored on this computer.

## Setup

### Connecting

The HL2 is found automatically on your LAN: open **File → Connect to Radio...** → **On This Network**, select it and click **Connect Selected Radio**.

Across a VPN or another subnet, use **Connect by IP** with **Radio type:** set to **Hermes-Lite 2** (see [Manual Connection](./manual-connection.md)).

- **Connect to last radio on start up** works with the HL2.
- **Nickname:** right-click the radio in the list → **Set Nickname…** / **Clear Nickname** (no need to connect), or **Settings → Radio Setup... → Radio → Nickname:** while connected. HL2 nicknames are stored on this computer, keyed by the radio's MAC address.
- The very first connect on a computer spends a while planning its DSP filters; the app stays responsive while it does. The result is saved, so later connects are quick.
- The status bar shows the gateware version.

### Board variant and hardware: HL2 Hardware

**Settings → Radio Setup... → HL2 Hardware** describes the physical radio. It appears while an HL2 is connected, and the settings are stored against that radio.

| Group | Controls |
|-------|----------|
| **Local Audio** | **Codec:** **None — plain Hermes-Lite 2**, **AK4951 — HL2+ companion board**, or **SquareSDR 2 — codec on the mainboard**. A board with a codec can play receive audio on the radio's own speaker; **Speaker level:** sets it. |
| **Dither Bit** | **Dither bit** and **Random bit**. On some variants the first box is relabelled to what that board uses the line for (internal loudspeaker or band-voltage output). |
| **Companion Filter Board** | **Board:** **None — nothing on J16**, **N2ADR — receive and transmit**, or **N2ADR — transmit only**. **Receive through the N2ADR 3 MHz high-pass** adds the AM-broadcast high-pass on receive (80 m to 10 m). Choose **None** only when nothing is fitted, because then nothing filters the transmitter's harmonics. |
| **Reference Clock and Tuner** | **External 10 MHz reference at CL1** locks the radio to a reference such as a GPSDO instead of its crystal. CL1 expects a 3.3 V logic-level clock: pad a typical 5 V GPSDO output with at least 6 dB of 50 Ω attenuation. Lock is not read back. **Antenna tuner driven by the HL2 gateware** raises the tune request during TUNE for an AH-4-protocol ATU; leave it off for a tuner on the N2ADR IO board. |

The HL2 IO board's amplifier control follows the band.

### Frequency calibration

**Settings → Radio Setup... → Calibration** corrects the HL2's crystal. It appears only on radios that cannot calibrate themselves.

1. Let the radio warm up for 15 minutes.
2. Pick a **Reference:** (WWV/WWVH 2.5–25 MHz, CHU 3.330 / 7.850 / 14.670 MHz, or **GPSDO / signal generator (custom)** with its frequency). Attenuate a GPSDO by at least 30 dB.
3. Zero-beat the reference with normal tuning, then click **Calibrate from current VFO**.
4. Fine-tune with **Trim:** − / + (steps of 0.1, 1 or 10 Hz at 10 MHz), or type the **Error:** in ppb (±50,000 ppb). **Reset** returns to 0.

One value corrects every band, receive and transmit. It is stored per radio (by MAC), so a second HL2 does not inherit it. While **External 10 MHz reference at CL1** is on, the manual calibration is zeroed and disabled.

## Receivers and Panadapters

- **Up to four receivers** (A–D), each with its own slice, panadapter, audio and S-meter. Add one with **Tools → Add Panadapter...**; close its pane to remove it.
- The receivers share one converter, so the **span (sample rate), LNA gain, band and antenna are radio-wide**. Mode, passband, AGC and frequency are per receiver.
- **Span** snaps to 48, 96, 192 or 384 kHz. Four receivers run up to 192 kHz, three at 384 kHz. **Use low bandwidth mode** caps the span at 96 kHz. Only the pane holding the TX slice (otherwise the first docked pane) has live span buttons; the others are dimmed with the reason.
- **Band buttons** switch the hardware filters and preamp as well as the frequency.
- **Spots** from the DX cluster, RBN, WSJT-X, POTA and manual entry are drawn on the panadapter. Right-click a spot → **Remove Spot**.
- **Display:** **FFT AVG** really averages (about 10 ms per step); at 0 the trace is unaveraged. Manual **Black Level** and waterfall **NB Blank** work. **WtrFall Rate** is a rate in rows per second. FFT FPS and the dBm range are remembered across restarts.
- **Tools → Wideband Bandscope...** shows one raw record of the whole converter band (0 to 38.4 MHz) on demand.

## RF Gain and the dBm Scale

- **RF gain** is the HL2's LNA, −12 to +48 dB. It is remembered per band; a band you have not visited starts at +20 dB.
- The dBm scale is derived from the LNA setting (full scale is +3 dBm at 0 dB LNA). It is not a calibrated measurement.
- **Auto RF gain** adjusts the LNA from the measured wideband headroom, in 6 dB steps, across the full range. If it cannot arm, the reason appears on the checkbox and in a notice.
- CAT `L RF` / `l RF` (rigctl) drive the panadapter RF gain across the same range; 0.5 is +18 dB.

> **Upgrading from an older version:**
> - A band you have not visited comes up at **+20 dB** LNA on every profile. A profile that carried a stuck lower value comes up hotter (26 dB in the reported case) on unvisited bands. Visited bands keep their gain.
> - At the default +20 dB LNA, a given squelch level gates **32 dB higher** than before. Where Auto squelch is not available, a slice in Auto drops to Manual at its manual level.
> - Selecting **DIGU** or **DIGL** now turns AGC off on that receiver. A DIGU/DIGL band-stack bookmark saved earlier can still restore its saved AGC mode.
> - A profile that ticked **Auto RF gain** (always refused before) now arms on its first connect, and runs in 6 dB steps.

## Receive

- **Latency:** outside CW the receive filter runs at minimum phase, so audio returns 44 ms after an unmute. In CW, onset is 85 ms; a notch or a passband under 100 Hz lengthens the filter.
- After you unkey, receive stays muted through the transmit/receive turnaround, so you don't hear the PA's own carrier.
- **S-meter** reads WDSP's average level. It holds steady across key-down and unkey.
- **AM and SAM** audio is DC-blocked and clean.
- **CW:** the passband is centred on the signal marker (a real BFO at your CW pitch).

### Squelch

The **SQL** button works per mode family:

- **FM:** FM squelch.
- **AM, SAM, DSB, LSB, USB:** a level squelch with a threshold map measured on the HL2. It tracks RF gain dB for dB.
- **CW and data modes:** no squelch.

The panadapter SQL line and **Auto** squelch use the HL2's own scale.

### AGC and APF

- AGC mode and threshold are remembered across restarts.
- **AGC off:** the **AGC-T** slider sets a fixed gain, 4–64 dB (10 dB by default). The AGC-off level is remembered per receiver.
- Selecting **DIGU** or **DIGL** turns AGC off on that receiver. Raise the AGC-off level if data audio is too quiet.
- **APF**, the CW audio peaking filter, is centred on your CW pitch. Its toggle and level are in the P/CW pane.

### RIT and XIT

RIT and XIT work per receiver, ±9999 Hz. When transmit passes to another receiver, that receiver's XIT applies.

### Notches and noise blanker

- **Manual notches:** right-click the panadapter → **Add TNF at X MHz**. Drag a notch sideways to move it and up or down to change its width; right-click it to remove it. Notches keep tracking as you tune. See [TNF (Tracking Notch Filters)](./tnf-tracking-notch-filters.md).
- **NB** runs WDSP's impulse blanker on the raw IQ, ahead of the demodulator. It is held during transmit.
- Client-side noise reduction works as on any radio; see [DSP Noise Mitigation](./dsp-noise-mitigation.md).

## Transmit

- **SSB voice** uses WDSP's TXA modulator. The EQ, PROC, TX low/high cut, eSSB, ALC and compression gauges act on the HL2.
- **The Mic Level slider is the transmit level.** The ALC only ever reduces. The Phone panel shows the **ALC Gain** the radio reports.
- **Data:** WSJT-X and similar programs work over the [TCI Server](./tci-server.md). Their power slider changes RF output, and the speech ALC does not raise client audio. The WSPR beacon defaults to −3 dBFS and is not moved by the mic slider.
- **CW** is keyed with host-timed edges, with sidetone. CW settings are restored after a restart.
- **TUNE** keys at **Tune Power**, and moving Tune Power during a tune changes the carrier.
- **Max Power:** **Settings → Radio Setup... → Transmit → Max Power** shows the HL2's rated watts, read-only. The power gauge uses a 5 W scale, and forward power reads near PEP on speech.
- **RF power slider:** the radio has 16 drive steps, so several slider positions do nothing and then one position adds about 1.25 dB.
- **Modes that refuse to transmit:** AM, SAM, DSB, FM, NFM, WBFM and DRM refuse MOX and TUNE, because the HL2 modulator is SSB only. A mode the HL2 cannot demodulate is refused everywhere.
- **TX drive and LNA gain** are remembered per band.

## Other Features

- CW, RTTY and AetherClock decoders, and the QSO recorder.
- AX.25 packet with AetherModem (APRS, KISS TNC, terminal and mailbox).
- **Memory channels**, stored on this computer in the bank shared by radios without memory slots. See [Memory Channels](./memory-channels.md).
- **Tools → Radio Health...** shows link, temperature, supply, PA current, gateware and the transmit voice chain.
- Connection health in the title bar, status bar and network statistics.

## Reference

### Not available on the HL2

- DAX, SmartLink or Multi-Flex.
- The status-bar **CWX**, **DVK** and **FDX** toggles are hidden: the HL2 has no radio-side CW text keyer, voice keyer or full duplex.
- FM repeater duplex and CTCSS encode.
- Controls the HL2 cannot honour are dimmed with their reason, or refuse with a notice.

## Known issues

- Receivers B, C and D do not come back after you restart AetherSDR; only receiver A reconnects ([#5777](https://github.com/aethersdr/AetherSDR/issues/5777)).
- Graphic EQ band values are lost when you restart AetherSDR ([#5611](https://github.com/aethersdr/AetherSDR/issues/5611)).
- Keying with the HL2's hardware PTT jack does not show as transmit in AetherSDR, and does not carry host audio ([#5998](https://github.com/aethersdr/AetherSDR/issues/5998)).
- **TX Delay** and the other fields under **Settings → Radio Setup... → Transmit → Timings** do not hold a value and are not applied to the HL2 ([#5370](https://github.com/aethersdr/AetherSDR/issues/5370)).
- Each over starts with about 100 ms of keyed silence before your audio is transmitted ([#6052](https://github.com/aethersdr/AetherSDR/issues/6052)).
- Changing CW break-in or its delay shows a "nothing was sent to the radio" notice, although both settings take effect ([#6153](https://github.com/aethersdr/AetherSDR/issues/6153)).

## Troubleshooting

### The first connect takes a long time

On a computer's first connect, AetherSDR plans the HL2's DSP filters.

1. Wait for it to finish; the app stays responsive meanwhile.
2. Later connects reuse the saved result and are quick.

### Span buttons are dimmed on a panadapter

The receivers share one sample rate, and only one pane controls it.

1. Change the span from the pane that holds the TX slice (or, if none does, the first docked pane).
2. If the span will not go above 96 kHz, untick **Use low bandwidth mode** in the Connect to Radio window and reconnect.

### Data-mode audio is too quiet

**DIGU** and **DIGL** turn AGC off on that receiver, so the fixed AGC-off gain sets the level.

1. Raise the **AGC-T** slider on that receiver (4–64 dB, 10 dB by default).

### MOX or TUNE is refused

The HL2 modulator is SSB only, so AM, SAM, DSB, FM, NFM, WBFM and DRM refuse to transmit.

1. Switch the slice to USB, LSB, CW or a data mode before keying.

### The Calibration controls are disabled

**External 10 MHz reference at CL1** is on, which zeroes and disables the manual calibration.

1. Open **Settings → Radio Setup... → HL2 Hardware**.
2. Untick **External 10 MHz reference at CL1** if you want to calibrate the crystal instead.

### Moving the RF power slider changes nothing

The HL2 has 16 drive steps, so several slider positions fall on the same step.

1. Keep moving the slider; the next step adds about 1.25 dB.

## See also

- [Supported Radios](./supported-radios.md)
- [Manual Connection](./manual-connection.md)
- [Low Bandwidth Connections](./low-bandwidth-connections.md)
- [Before You Transmit](./before-you-transmit.md)
- [`docs/HERMES.md`](https://github.com/aethersdr/AetherSDR/blob/main/docs/HERMES.md): bring-up field notes for developers
- [`docs/architecture/hl2-frequency-calibration.md`](https://github.com/aethersdr/AetherSDR/blob/main/docs/architecture/hl2-frequency-calibration.md)
