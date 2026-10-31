---
title: "ANAN-G2"
slug: "/anan-g2"
description: "AetherSDR can receive with an Apache Labs ANAN-G2 over openHPSDR Protocol 2."
status: "Experimental, receive-only"
applies_to: ["ANAN-G2"]
---

:::info[Status]

**Status:** Experimental, receive-only · **Applies to:** ANAN-G2

:::

AetherSDR can receive with an Apache Labs **ANAN-G2** over openHPSDR Protocol 2. Support is **experimental and receive-only**: one receiver, one panadapter, no transmit yet. FlexRadio remains the supported target (see [Supported Radios](./supported-radios.md)). An **EXPERIMENTAL** badge shows in the title bar while a G2 is connected.

## Where to go next

- [Panadapter Controls](./panadapter-controls.md): FFT AVG, Black Level and NB Blank on the G2's panadapter.
- [RX Controls](./rx-controls.md): tuning, AF gain, mute and balance.
- [DSP Noise Mitigation](./dsp-noise-mitigation.md): client-side noise reduction.
- [Memory Channels](./memory-channels.md): stored on this computer.

## Setup

The G2 is found automatically on your LAN: open **File → Connect to Radio...** → **On This Network**, select it and click **Connect Selected Radio**.

Discovery also sends a directed broadcast to each attached IPv4 subnet. This can find a G2 on a second NIC with its own subnet. Two NICs sharing a subnet still use the preferred route; VPNs and routers may block broadcast discovery. If the G2 does not appear, use **Connect by IP** with **Radio type:** set to **ANAN-G2** (see [Manual Connection](./manual-connection.md)).

Configure the receive front end under **Settings → Radio Setup... → Radio → ANAN Front End**. The page is available while disconnected, and while connected to a G2. Its options apply on the next connection, whether you use **Available radios** or **Connect by IP**. **Filter bank:** and **Mute the radio's speaker amplifier** also apply at once to a connected G2:

| Option | Default | What it does |
|--------|---------|--------------|
| **Sample rate:** | 48 ksps | DDC0 sample rate: 48, 96, 192, 384, 768 or 1536 ksps. It is also the starting width of the panadapter. Higher rates use more of the radio's Ethernet link. |
| **Feed the receiver from:** | ADC0 — behind the ANT1/2/3 relays | Which receive chain feeds the receiver: **ADC0 — behind the ANT1/2/3 relays**, behind the switched antenna relays, or **ADC1 — the RX2 jack**, wired straight to the RX2 jack. |
| **Dither** / **Randomization** | On | The ADC's linearization bits. Leave them on unless you are testing. |
| **Filter bank:** | Bypassed | **Bypassed** keeps both ADCs' band-pass filter banks out of circuit. **Follow the tuned frequency** selects the band-pass filter that covers the receiver's frequency on both ADCs, and changes it as you tune, so strong signals on other bands are rejected before the ADC. A filter that does not cover the tuned frequency leaves no signal at all. After switching it on, check each band you use. Applies at once. |
| **Bypass ADC0's bank** / **Bypass ADC1's bank (RX2)** | On | With **Filter bank:** set to **Bypassed**, routes the antenna around that ADC's filter bank. **Leave these checked:** unchecked means nothing reaches the ADC. Dimmed while the filter bank follows the tuned frequency, which switches the same relay. |
| **Send receive audio to the radio** | Off | Also plays receive audio through the G2's own speaker and headphone jack. Takes effect on the next connect. |
| **Mute the radio's speaker amplifier** | Off | Switches off the amplifier inside the G2. Sending the radio silence leaves the amplifier running and hissing; this stops it. Applies at once. A clean disconnect hands the radio back unmuted. |

## Receiving with the G2

- **Receive** with live tuning and zoom. Changing the zoom changes the DDC rate live, without rebuilding the session.
- **Panadapter** computed by WDSP's display analyzer: a ≥16384-point windowed FFT at one point per screen pixel. **FFT AVG** and **Wt Avg** drive it, and noise-floor auto-adjust works.
- **S-meter.**
- **NB** cycles Off → **NB** → **NB2** → Off: WDSP's two impulse blankers, run on this computer before the demodulator. **NB** silences each impulse; **NB2** reconstructs the blanked window from the samples either side, and shows an **NB2** fill selector (**Zero**, **Sample hold**, **Mean hold**, **Hold sample** or **Interpolate**) while it runs. Neither is offered in FM.
- **AGC-T** sets the AGC's maximum gain from −20 to 120 dB, 1.4 dB per slider step, starting at 71 (79.4 dB). With AGC off it sets the fixed gain instead, starting at 29 (20.6 dB). AGC-T and the AGC-off level are not remembered across launches; each launch starts at 71 and 29.
- **RF Gain** from −31 to 0 dB drives the G2's ADC step attenuator, and is restored per radio.
- **AF gain, mute and balance** apply. With **Send receive audio to the radio** on, the receiver's mute and volume apply to both the computer and the radio's speaker, and the title-bar speaker button mutes the radio's speaker too.
- Manual **Black Level** and waterfall **NB Blank**. See [Panadapter Controls](./panadapter-controls.md).
- **Status bar:** the **Network:** indicator follows the G2's link (received data and packet loss; round-trip time and jitter are not measured on this radio). The G2's supply voltage is shown in the status bar and the Meter applet. The G2 reports no PA temperature, so that reading is not shown.
- **Tools → Radio Health...**

The panadapter is not dBm-calibrated: its levels are relative.

> **Upgrading from an older version:** **AGC-T** covers −20 to 120 dB, so a given slider position gives more gain than before (at 100, 120 dB instead of 60 dB). Band-stack bookmarks saved earlier replay their slider position on this scale, so they come back louder. Lower **AGC-T** on each one and save it again.

## Droop Correction

The G2's DDC loses a little amplitude near the edges of the displayed span. AetherSDR applies a correction derived from the radio's own (Saturn) gateware automatically.

To measure your own radio instead, open **Settings → Radio Setup... → Droop Correction** while connected:

1. Disconnect the antenna or terminate it in a dummy load. The sweep uses the receiver's own noise floor as a flat reference, so a live signal biases the result.
2. Click **Start Sweep**. It steps through every DDC0 sample rate and takes several minutes. **Stop** ends it early.
3. Review the measured correction per rate, then click **Apply** to use and save it for this radio, or **Discard**.

> **Upgrading from an older version:** G2 panadapter tones now read about **7.4 dB lower** than they did before the WDSP panadapter (a test tone that read −6.0 dB now reads −13.4 dB). Re-trim a saved reference level or waterfall black level once.

## Reference

### What is remembered

The G2 stores none of its settings, so AetherSDR keeps these per radio: frequency and mode (14.175 MHz USB on a first connect), tuning step, **RF Gain**, the radio's own output level (what the volume slider sets with PC Audio off) and the panadapter dBm range. The span always starts at the connect form's **Sample rate:**. AGC settings and FFT averaging start from their defaults.

### Not available on the G2

- **Transmit.** Keying is refused; transmit is a future phase.
- DAX, SmartLink or Multi-Flex.
- More than one receiver or panadapter.
- A headphone level control: the **Headphone:** row under **Settings → Radio Setup... → Audio → Radio Audio Outputs** is dimmed; **Line Out:** stays available.

Controls the G2 cannot honor are dimmed with a reason, or refuse with a notice; see [Supported Radios](./supported-radios.md).

## Known issues

- DX cluster, RBN, WSJT-X, POTA and manual spots are never drawn on the panadapter ([#6039](https://github.com/aethersdr/AetherSDR/issues/6039)).

## Troubleshooting

### Nothing is received at all

With **Filter bank:** set to **Bypassed**, a **Bypass ADC0's bank** or **Bypass ADC1's bank (RX2)** box is unchecked, so nothing reaches the ADC.

1. Disconnect from the G2.
2. Open **Settings → Radio Setup... → Radio → ANAN Front End**.
3. Tick **Bypass ADC0's bank** and **Bypass ADC1's bank (RX2)**, then connect again.

### One band goes quiet with the filter bank on

**Filter bank:** is set to **Follow the tuned frequency**, and the filter selected for that band does not pass it.

1. Open **Settings → Radio Setup... → Radio → ANAN Front End**.
2. Set **Filter bank:** to **Bypassed**. It applies at once.

### No audio from the radio's own speaker

**Send receive audio to the radio** is off, or was turned on during the session. It takes effect on the next connect.

1. Open **Settings → Radio Setup... → Radio → ANAN Front End** and tick **Send receive audio to the radio**.
2. Disconnect and connect again.
3. Check that the receiver and the title-bar speaker button are not muted; they mute the radio's speaker too.
4. Check that **Mute the radio's speaker amplifier** is unticked.

If AetherSDR closed without disconnecting, or the network cable was pulled, while the amplifier was muted, the G2 keeps it muted. Connect again with **Mute the radio's speaker amplifier** unticked.

## See also

- [Supported Radios](./supported-radios.md)
- [Manual Connection](./manual-connection.md)
- [Panadapter Controls](./panadapter-controls.md)
- [`docs/architecture/anan-p2-backend-design.md`](https://github.com/aethersdr/AetherSDR/blob/main/docs/architecture/anan-p2-backend-design.md): background for developers
