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

To reach it across a VPN or another subnet, use **Connect by IP** with **Radio type:** set to **ANAN-G2** (see [Manual Connection](./manual-connection.md)). That form also holds the G2's connection options, which apply on both paths:

| Option | Default | What it does |
|--------|---------|--------------|
| **Sample rate:** | 48 ksps | DDC0 sample rate: 48, 96, 192, 384, 768 or 1536 ksps. It is also the starting width of the panadapter. Higher rates use more of the radio's Ethernet link. |
| **Select ADC:** | ADC0 (ANT1/2/3) | Which receive chain feeds the receiver: **ADC0 (ANT1/2/3)**, behind the switched antenna relays, or **ADC1 (RX2)**, wired straight to the RX2 jack. |
| **Dither** / **Random** | On | The ADC's linearisation bits. Leave them on unless you are testing. |
| **ADC0 RF filter bypass** / **ADC1 RF filter bypass** | On | Routes the antenna around the front-end filter bank. **Leave these checked:** no band filter is selected in this version, so unchecked means nothing reaches the ADC. |
| **Send RX audio to the radio's speaker** | Off | Also plays receive audio through the G2's own speaker and headphone jack. Takes effect on the next connect. |

## Receiving with the G2

- **Receive** with live tuning and zoom. Changing the zoom changes the DDC rate live, without rebuilding the session.
- **Panadapter** computed by WDSP's display analyser: a ≥16384-point windowed FFT at one point per screen pixel. **FFT AVG** and **Weighted** averaging drive it, and noise-floor auto-adjust works.
- **S-meter.**
- **NB** (noise blanker): WDSP's impulse blanker, run on this computer before the demodulator.
- **RF Gain** from −31 to 0 dB drives the G2's ADC step attenuator, and is restored per radio.
- **AF gain, mute and balance** apply. With **Send RX audio to the radio's speaker** on, the receiver's mute and volume apply to both the computer and the radio's speaker, and the title-bar speaker button mutes the radio's speaker too.
- Manual **Black Level** and waterfall **NB Blank**. See [Panadapter Controls](./panadapter-controls.md).
- **Tools → Radio Health...**

The panadapter is not dBm-calibrated: its levels are relative.

## Droop Correction

The G2's DDC loses a little amplitude near the edges of the displayed span. AetherSDR applies a correction derived from the radio's own (Saturn) gateware automatically.

To measure your own radio instead, open **Settings → Radio Setup... → Droop Correction** while connected:

1. Disconnect the antenna or terminate it in a dummy load. The sweep uses the receiver's own noise floor as a flat reference, so a live signal biases the result.
2. Click **Start Sweep**. It steps through every DDC0 sample rate and takes several minutes. **Stop** ends it early.
3. Review the measured correction per rate, then click **Apply** to use and save it for this radio, or **Discard**.

> **Upgrading from an older version:** G2 panadapter tones now read about **7.4 dB lower** than they did before the WDSP panadapter (a test tone that read −6.0 dB now reads −13.4 dB). Re-trim a saved reference level or waterfall black level once.

## Reference

### Not available on the G2

- **Transmit.** Keying is refused; transmit is a future phase.
- DAX, SmartLink or Multi-Flex.
- More than one receiver or panadapter.

Controls the G2 cannot honour are dimmed with a reason, or refuse with a notice; see [Supported Radios](./supported-radios.md).

## Known issues

- Weak signals sound quieter than in Thetis on the same radio unless the **AGC-T** slider is near the top ([#5988](https://github.com/aethersdr/AetherSDR/issues/5988)).
- DX cluster, RBN, WSJT-X, POTA and manual spots are never drawn on the panadapter ([#6039](https://github.com/aethersdr/AetherSDR/issues/6039)).

## Troubleshooting

### Nothing is received at all

An **RF filter bypass** box is unchecked. No band filter is selected, so an unchecked bypass means nothing reaches the ADC.

1. Disconnect from the G2.
2. Open **Connect by IP** with **Radio type:** set to **ANAN-G2**.
3. Tick **ADC0 RF filter bypass** and **ADC1 RF filter bypass**, then connect again.

### No audio from the radio's own speaker

**Send RX audio to the radio's speaker** is off, or was turned on during the session. It takes effect on the next connect.

1. Tick **Send RX audio to the radio's speaker** in the ANAN-G2 options.
2. Disconnect and connect again.
3. Check that the receiver and the title-bar speaker button are not muted; they mute the radio's speaker too.

## See also

- [Supported Radios](./supported-radios.md)
- [Manual Connection](./manual-connection.md)
- [Panadapter Controls](./panadapter-controls.md)
- [`docs/architecture/anan-p2-backend-design.md`](https://github.com/aethersdr/AetherSDR/blob/main/docs/architecture/anan-p2-backend-design.md): background for developers
