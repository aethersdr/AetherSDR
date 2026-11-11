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

The HL2 is found automatically on your LAN: open **File → Connect to Radio...** → **Nearby radios**, select it and click **Connect Selected Radio**. AetherSDR probes from every active IPv4 interface, so a computer with several network adapters finds the HL2 on the one it is connected to, and keeps using that interface for the session and its reconnects.

Across a VPN or another subnet, use **Connect by IP** with **Radio type:** set to **Hermes-Lite 2** (see [Manual Connection](./manual-connection.md)).

- **Connect to last radio on start up** works with the HL2.
- **Nickname:** right-click the radio in the list → **Set Nickname…** / **Clear Nickname** (no need to connect), or **Settings → Radio Setup... → Radio → Nickname:** while connected. HL2 nicknames are stored on this computer, keyed by the radio's MAC address.
- The very first connect on a computer spends a while planning its DSP filters; the app stays responsive while it does. The result is saved, so later connects are quick.
- The status bar shows the gateware version as major.minor, such as **Gateware 74.3** for stock Hermes-Lite 2 gateware. A SQUARE SDR 2 (minor version 52) and a SQUARE 3 (53) are named: **SQUARE SDR 2 gateware 75.52**. A first-generation SQUARE SDR runs stock HL2 gateware and shows as a Hermes-Lite 2.

On Linux and macOS, the WDSP filter-planning cache uses `XDG_CACHE_HOME` when
nonempty, otherwise `$HOME/.cache`. If both variables are unset or empty,
it uses the system temporary directory. On Windows, it uses `LOCALAPPDATA`,
with the same temporary-directory fallback. The cache file is
`aethersdr/wdsp-fftw-wisdom` beneath that directory.

### Board variant and hardware: HL2 Hardware

**Settings → Radio Setup... → HL2 Hardware** describes the physical radio. It appears while an HL2 is connected, and the settings are stored against that radio.

| Group | Controls |
|-------|----------|
| **Local Audio** | **Codec:** **None — plain Hermes-Lite 2**, **AK4951 — HL2+ companion board**, or **SquareSDR 2 — codec on the mainboard**. A board with a codec can play receive audio on the radio's own speaker; **Speaker level:** sets it. A SQUARE SDR 2 with no codec chosen yet starts with **SquareSDR 2** selected and **Detected: SQUARE SDR 2 — suggested, not saved** beside it; choosing a codec, the selected one included, stores it, and any choice you have made, **None** included, is kept. |
| **Dither Bit** | **Dither bit** and **Random bit**. On some variants the first box is relabeled to what that board uses the line for (internal loudspeaker or band-voltage output). |
| **Companion Filter Board** | **Board:** **None — nothing on J16**, **N2ADR — receive and transmit**, or **N2ADR — transmit only**. **Receive through the N2ADR 3 MHz high-pass** adds the AM-broadcast high-pass on receive (80 m to 10 m). Choose **None** only when nothing is fitted, because then nothing filters the transmitter's harmonics. A SQUARE SDR 2 with no board chosen yet starts with **N2ADR — transmit only**, its own arrangement, selected and **Detected: SQUARE SDR 2 — suggested, not saved** beside it; as with the codec, choosing a board, the selected one included, stores it, and any choice you have made is kept. |
| **Reference Clock and Tuner** | **External 10 MHz reference at CL1** locks the radio to a reference such as a GPSDO instead of its crystal. CL1 expects a 3.3 V logic-level clock: pad a typical 5 V GPSDO output with at least 6 dB of 50 Ω attenuation. Lock is not read back. **Antenna tuner driven by the HL2 gateware** raises the tune request during TUNE for an AH-4-protocol ATU; leave it off for a tuner on the N2ADR IO board. |

The HL2 IO board's amplifier control follows the band.

### Frequency calibration

**Settings → Radio Setup... → Calibration** corrects the HL2's crystal. It appears only on radios that cannot calibrate themselves.

1. Let the radio warm up for 15 minutes.
2. Pick a **Reference:** (WWV/WWVH 2.5–25 MHz, CHU 3.330 / 7.850 / 14.670 MHz, or **GPSDO / signal generator (custom)** with its frequency). Attenuate a GPSDO by at least 30 dB.
3. Zero-beat the reference with normal tuning, then click **Calibrate from Current VFO**.
4. Fine-tune with **Trim:** − / + (steps of 0.1, 1 or 10 Hz at 10 MHz), or type the **Error:** in ppb (±50,000 ppb). **Reset** returns to 0.

One value corrects every band, receive and transmit. It is stored per radio (by MAC), so a second HL2 does not inherit it. While **External 10 MHz reference at CL1** is on, the manual calibration is zeroed and disabled.

## Receivers and Panadapters

- **Up to four receivers** (A–D), each with its own slice, panadapter, audio and S-meter. Add one with **Tools → Add Panadapter...**; close its pane to remove it.
- The receivers share one converter, so the **span (sample rate), LNA gain, band and antenna are radio-wide**. Mode, passband, AGC and frequency are per receiver. The RX antenna menu offers **ANT**, the one receive input; KiwiSDR receivers, when configured, are listed beside it.
- **After a restart** the receivers you had open come back, each with its own frequency, mode, passband, noise blanker, APF, squelch, RIT/XIT, audio gain and pan, and transmit returns to the receiver that had it. If the span allows fewer receivers than you had, the others are kept and come back on a later connect with room for them, unless you add or close a receiver first.
- After disconnecting and reconnecting to the same HL2, transmit returns to the receiver that held it and uses that receiver’s mode and transmit passband. Reconnecting does not key the radio.
- **Span** snaps to 48, 96, 192 or 384 kHz. Four receivers run up to 192 kHz, three at 384 kHz. **Use low bandwidth mode** caps the span at 96 kHz. Only the pane holding the TX slice (otherwise the first docked pane) has live span buttons; the others are dimmed with the reason.
- **Band buttons** switch the hardware filters and preamp as well as the frequency.
- **Spots** from the DX cluster, RBN, WSJT-X, POTA and manual entry are drawn on the panadapter. Right-click a spot → **Remove Spot**.
- **Display:** **FFT AVG** really averages (about 10 ms per step); at 0 the trace is unaveraged. Manual **Black Level** and waterfall **NB Blank** work. **WtrFall Rate** is a rate in rows per second. FFT FPS, FFT AVG, **Wt Avg** and the dBm range are remembered across restarts.
- **Tools → Wideband Bandscope...** shows one raw record of the whole converter band (0 to 38.4 MHz) on demand.

## RF Gain and the dBm Scale

- **RF gain** is the HL2's LNA, −12 to +48 dB. It is remembered per band; a band you have not visited starts at +20 dB.
- The dBm scale is derived from the LNA setting (full scale is +3 dBm at 0 dB LNA). It is not a calibrated measurement.
- Dragging the dBm scale or clicking its strip adjusts the display locally without an unsupported-command notice. The automatic noise-floor adjustment remains available.
- **Auto RF gain** adjusts the LNA from the measured wideband headroom, in 6 dB steps, across the full range. If it cannot arm, the reason appears on the checkbox and in a notice.
- CAT `L RF` / `l RF` (rigctl) drive the panadapter RF gain across the same range; 0.5 is +18 dB.

> **Upgrading from an older version:**
> - A band you have not visited comes up at **+20 dB** LNA on every profile. A profile that carried a stuck lower value comes up hotter (26 dB in the reported case) on unvisited bands. Visited bands keep their gain.
> - At the default +20 dB LNA, a given squelch level gates **32 dB higher** than before. Where Auto squelch is not available, a slice in Auto drops to Manual at its manual level.
> - Selecting **DIGU** or **DIGL** now turns AGC off on that receiver.
> - A profile that ticked **Auto RF gain** (always refused before) now arms on its first connect, and runs in 6 dB steps.

## Receive

- **Panadapter centre:** panning stops at the supported centre range, 100 kHz to 38.4 MHz. At wide spans, the lower edge of the display stays at or above 0 Hz. Panning moves the receive window independently of the slice frequency.
- **Latency:** outside CW the receive filter runs at minimum phase, so audio returns 44 ms after an unmute. In CW, onset is 85 ms; a notch or a passband under 100 Hz lengthens the filter.
- After you unkey, receive stays muted through the transmit/receive turnaround, so you don't hear the PA's own carrier.
- **S-meter** reads WDSP's average level. It holds steady across key-down and unkey.
- **AM and SAM** audio is DC-blocked and clean.
- **CW:** the passband is centered on the signal marker (a real BFO at your CW pitch).

> **Upgrading from an older version:** receive audio is about 12 dB quieter at every AF gain setting: on the speaker, the radio's own codec, TCI and the decoders. If you turned a slice's AF gain down (to about 25) to stop strong stations distorting, you can turn it back up.

### Receive filters

Choose filter presets in [RX Controls](./rx-controls.md) or the
[VFO flag's mode tab](./vfo-widget.md#mode-tab). In **FM**, the VFO offers widths
from **6K** to **20K** in 2 kHz steps; the compact RX applet shows the first six,
**6K** to **16K**. In **WBFM**, the passband control is disabled and the preset
row shows **Filter unavailable**.

Saved **CW/CWL** presets must straddle the signal marker: the low cut is
−6000 to −10 Hz and the high cut is +10 to +6000 Hz. Presets outside those
ranges are skipped; if none fit, the normal CW presets appear.

> **Upgrading from an older version:** a saved CW preset with both cuts on the
> same side of the marker is skipped. Recreate it with cuts on opposite sides
> of the marker, or choose a standard CW width.

### Squelch

The **SQL** button works per mode family:

- **FM:** FM squelch.
- **AM, SAM, DSB, LSB, USB:** a level squelch with a threshold map measured on the HL2. It tracks RF gain dB for dB.
- **CW and data modes:** no squelch.

The panadapter SQL line and **Auto** squelch use the HL2's own scale.

### AGC and APF

- AGC mode and threshold are remembered across restarts.
- **AGC off:** the **AGC-T** slider sets a fixed gain, 4–64 dB (10 dB by default). The AGC-off level is remembered per receiver.
- Selecting **DIGU** or **DIGL** turns AGC off on that receiver. Raise the AGC-off level if data audio is too quiet. A band-stack bookmark recalled into DIGU or DIGL also keeps AGC off, whatever AGC mode it stored.
- **APF**, the CW audio peaking filter, is centered on your CW pitch. Its toggle and level are in the P/CW pane.

### RIT and XIT

RIT and XIT work per receiver, ±9999 Hz. When transmit passes to another receiver, that receiver's XIT applies.

### Notches and noise blanker

- **Manual notches:** right-click the panadapter → **Add TNF at X MHz**. Drag a notch sideways to move it and up or down to change its width; right-click it to remove it. Notches keep tracking as you tune, and are remembered across restarts. See [TNF (Tracking Notch Filters)](./tnf-tracking-notch-filters.md).
- **NB** cycles Off → **NB** → **NB2** → Off. Both run WDSP impulse blankers on the raw IQ, ahead of the demodulator, and both are held during transmit. **NB** silences each impulse; **NB2** reconstructs the blanked window from the samples either side, which sounds better when an impulse lands on a signal and worse when the noise is dense. While NB2 runs, an **NB2** fill selector appears under the DSP controls: **Zero**, **Sample hold**, **Mean hold**, **Hold sample** or **Interpolate**. Neither blanker is offered in FM.
- Client-side noise reduction works as on any radio; see [DSP Noise Mitigation](./dsp-noise-mitigation.md).

## Transmit

- **SSB voice** uses WDSP's TXA modulator. The EQ, PROC, TX low/high cut, eSSB, ALC and compression gauges act on the HL2.
- **Transmit timing:** voice and data audio wait for a three-block IQ queue at the start of an over. Its two-block margin absorbs up to about 43 ms of accumulated delivery lateness; larger delays can still cause gaps. USB and LSB use a minimum-phase transmit filter; data modes keep linear phase. CW and TUNE bypass the queue hold.
- **TX filter-loss notice:** when transmitting from receiver A, AetherSDR can warn if the transmit audio is strongly attenuated through the modulator. The check uses a 40 dB threshold, so a narrow passband can still reduce audio without triggering a notice. It does not detect this loss in FM or while transmitting from another receiver.
- **The Mic Level slider is the transmit level.** The ALC only ever reduces. The Phone panel shows the **ALC Gain** the radio reports.
- **The ALC gauge shows the audio peak after the ALC, before the modulator.** It sits at the ALC target while the ALC is reducing. It is not the transmitted envelope, which can stand above it when the audio is limited or compressed.
- **Data:** WSJT-X and similar programs work over the [TCI Server](./tci-server.md). Their power slider changes RF output, and the speech ALC does not raise client audio. The WSPR beacon defaults to −3 dBFS and is not moved by the mic slider.
- **CW** is keyed with host-timed edges, with sidetone, at the same carrier level as TUNE. CW settings are restored after a restart.
- **A key or PTT on the radio's own jack** shows as transmit and mutes the receiver for as long as the radio reports it. AetherSDR does not key or modulate because of it.
- **TUNE** keys at **Tune Power**, and moving Tune Power during a tune changes the carrier. Tune Power is remembered per band.
- **Session safety:** MOX, CW PTT and TUNE requests are refused while the radio session is stopped. Stopping the session clears a held software key; reconnecting requires a fresh transmit request.
- **Show TX in Waterfall:** **Settings → Radio Setup... → Transmit → Show TX in Waterfall** lets the waterfall draw spectrum rows over the TX passband while transmitting. It starts off, and AetherSDR remembers your choice separately for each HL2 across reconnects and restarts. The control does not key the radio.
- **Max Power:** **Settings → Radio Setup... → Transmit → Max Power** shows the HL2's rated watts, read-only. The power gauge uses a 5 W scale, and forward power reads near PEP on speech.
- **SWR while transmitting** is the ratio of the highest forward and the highest reflected reading in each 0.1 s telemetry window, so it reads the load rather than the speech envelope. It appears about 0.2 s after keying and holds its last reading through short pauses in speech; when the carrier drops to a lower level and stays there, it also holds for a moment before following. This applies to overs AetherSDR keys (MOX, PTT from a client, tune); an over keyed on the radio's own PTT input still shows the latest pair. Unkeyed, and in **Tools → Radio Health...**, SWR is the ratio of the latest pair of readings.
- **RF power slider:** the radio has 16 drive steps, so several slider positions do nothing and then one position adds about 1.25 dB.
- **AM, DSB and FM** transmit through the same TXA modulator as SSB. FM is simplex and carries no CTCSS tone.
- **Modes that refuse to transmit:** SAM, WBFM and DRM refuse MOX and TUNE. SAM is a receive detector choice; wide FM has no transmit modulator, and DRM has no encoder. A mode the HL2 cannot demodulate is refused everywhere.
- **TX drive and LNA gain** are remembered per band.
- **TX output, ANT or RF1:** the slice's TX antenna button chooses where transmit leaves the board. **ANT**, the default, uses the onboard PA. **RF1** is the low-power output with the PA off. The choice is remembered per radio and comes back on the next connect; RF1's drive starts at 0 % each time. The choice cannot change while the radio transmits.
- **RF power on RF1** is a separate setting. It starts at 0 % each time you select RF1 and is not remembered per band; your ANT drive for each band is kept and returns when you select ANT. TUNE on RF1 runs at the RF1 setting, and Tune Power has no effect there.
- **On RF1 the power and SWR gauges read about 0.** That is expected, not a fault. An amplifier keyed from EXTTR still keys on RF1, the IO board still receives the TX frequency, and the gateware tuner request is not raised. A CAT program's antenna command (`set_ant`) changes only the receive antenna, and recalling a band-stack entry leaves the TX output where it is.
- **MON** and its volume slider, and **VOX** and its level slider, are disabled because these controls are unavailable on the HL2. Their tooltips and accessible descriptions explain why.

> **Upgrading from an older version:** CW keys at the same carrier level as TUNE, about 6 dB (four times the power) above earlier versions at the same RF power setting. Check your RF power before you key CW into an amplifier or a low-power load.

> **Upgrading from an older version:** the transmit queue adds about 43 ms of audio delay and can discard about 43 ms more audio when you release PTT. The shorter voice filter partly offsets the start delay: one HL2 tone measurement showed about 12 ms more leading silence overall. Allow the audio to finish before releasing PTT; external digital-mode timing must accommodate the queue delay.

## Other Features

- CW, RTTY and AetherClock decoders, and the QSO recorder.
- AX.25 packet with AetherModem (APRS, KISS TNC, terminal and mailbox).
- **Memory channels**, stored on this computer in the bank shared by radios without memory slots. See [Memory Channels](./memory-channels.md).
- **Tools → Radio Health...** shows link, temperature, PA bias current (raw counts), gateware, the TX output and the transmit voice chain.
- Connection health in the title bar, status bar and network statistics.

## Reference

### Not available on the HL2

- DAX, SmartLink or Multi-Flex.
- The status-bar **CWX**, **DVK** and **FDX** toggles are hidden: the HL2 has no radio-side CW text keyer, voice keyer or full duplex.
- FM repeater duplex and CTCSS encode.
- Line out and headphone controls: the **Line Out:** and **Headphone:** rows under **Settings → Radio Setup... → Audio → Radio Audio Outputs** are dimmed. A codec board's speaker level is under **HL2 Hardware**.
- Transmit timings. The fields under **Settings → Radio Setup... → Transmit → Timings (in ms)** are dimmed: "Unavailable: this radio does not accept transmit timings from here. No delay or timeout set here is applied."
- Controls the HL2 cannot honor are dimmed with their reason, or refuse with a notice.

## Known issues

- Keying with the HL2's hardware PTT jack shows as transmit in AetherSDR and mutes the receiver, but does not carry host audio: only the radio is keyed, and AetherSDR sends no modulation ([#5998](https://github.com/aethersdr/AetherSDR/issues/5998)).
- RF1's output level against the RF power slider, and its harmonics, have not been measured ([#4954](https://github.com/aethersdr/AetherSDR/issues/4954)). Start low and use a fixed attenuator before any receiver or analyzer.
- Each over starts with keyed silence before your audio is transmitted: about 125 ms from the microphone and 175 to 195 ms from a TCI client, longer when the client's audio arrives unevenly ([#6052](https://github.com/aethersdr/AetherSDR/issues/6052)).
- A CW key on the HL2's own key jack gives no sidetone in AetherSDR: the radio makes the CW itself, the receiver is muted while it does, and the client sidetone follows only AetherSDR's own keyer ([#5909](https://github.com/aethersdr/AetherSDR/issues/5909)).
- Paddle CW timing can vary with load: each dit and dah edge from AetherSDR's keyer waits in the app's main event queue before it reaches the radio, so a busy computer can stretch or shorten elements ([#5065](https://github.com/aethersdr/AetherSDR/issues/5065)).
- One user reports a row of evenly spaced peaks on the 10 m panadapter while transmitting FT8 at 28.074 MHz. It is not confirmed: a receive-only check of 10 m on another HL2 found none on a dummy load ([#6577](https://github.com/aethersdr/AetherSDR/issues/6577)).

### What has not been tested

This page was checked with one Hermes-Lite 2 (gateware 74) on macOS, at one station. Reports from other setups are welcome ([#5678](https://github.com/aethersdr/AetherSDR/issues/5678)). Not yet tested:

- Windows, and boards built on the HL2. Linux has had one short check, without a desktop or sound hardware.
- Transmitting into an antenna: every transmit test went into a dummy load.
- Calibrated power: the forward power and SWR readings are not checked against a calibrated meter, so they are not calibrated watts.
- CW sent with a key on the radio's own jack.
- What the radio does when AetherSDR disconnects or crashes while transmitting.
- Two HL2s used from one computer.

## Troubleshooting

### The first connect takes a long time

On a computer's first connect, AetherSDR plans the HL2's DSP filters.

1. Wait for it to finish; the app stays responsive meanwhile.
2. Later connects reuse the saved result and are quick.

### The HL2 is not in the radio list

The discovery probe got no answer, or could not use a network interface on this computer.

1. Open **Help → Support & Diagnostics...** and look in the log for lines starting `HL2 discovery:`. `cannot bind` or `no usable IPv4 interface` means a network interface on this computer could not be used; `no Hermes-Lite 2 answered 3 sweeps` means the probes went out and no HL2 replied.
2. If the HL2 is on another subnet or across a VPN, use **Connect by IP** instead (see [Manual Connection](./manual-connection.md)).

### Span buttons are dimmed on a panadapter

The receivers share one sample rate, and only one pane controls it.

1. Change the span from the pane that holds the TX slice (or, if none does, the first docked pane).
2. If the span will not go above 96 kHz, untick **Use low bandwidth mode** in the Connect to Radio window and reconnect.

### Data-mode audio is too quiet

**DIGU** and **DIGL** turn AGC off on that receiver, so the fixed AGC-off gain sets the level.

1. Raise the **AGC-T** slider on that receiver (4–64 dB, 10 dB by default).

### MOX or TUNE is refused

SAM is receive-only. WBFM has no transmit modulator, and DRM has no encoder, so those modes also refuse to transmit.

1. Switch the slice to USB, LSB, AM, DSB, FM, CW or a data mode before keying.

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
