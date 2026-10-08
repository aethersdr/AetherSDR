---
title: "DAX IQ Streaming"
slug: "/dax-iq-streaming"
description: "DAX IQ delivers raw I/Q samples from the radio's digital down-converter to other SDR programs."
status: "Supported"
applies_to: ["FlexRadio"]
---

:::info[Status]

**Status:** Supported · **Applies to:** FlexRadio

:::

DAX IQ delivers raw I/Q samples from the radio's digital down-converter to other SDR programs. Regular [DAX Virtual Audio](./dax-virtual-audio.md) carries demodulated audio from a slice; DAX IQ carries the unprocessed complex signal of a **panadapter**, before any mode, filter or AGC. AetherSDR offers **four IQ channels** at 24, 48, 96 or 192 kHz. DAX IQ is a FlexRadio feature.

Typical uses:

- **Second-opinion SDR software:** run SDR#, GQRX, SDR Console or similar alongside AetherSDR.
- **Skimmers:** CW Skimmer and SDC take IQ through the [TCI Server](./tci-server.md), up to four streams at once.
- **Recording and analysis:** record raw IQ, or feed GNU Radio or your own DSP.
- **Satellite data:** the built-in WFM demodulator (below).

## Setup

### Choosing which panadapter feeds a channel

IQ channels belong to panadapters, not slices. In the panadapter overlay, open the **DAX** flyout and pick the channel in **IQ Ch** (None, 1–4). The stream itself is switched on in the DAX IQ applet.

### Starting a stream

1. Open the **DAX IQ** applet (applet-tray button **IQ**, closed by default).
2. Choose a rate for the channel.
3. Press **On** for that channel.

## Using DAX IQ

### Getting IQ into other programs

On **Linux**, each running channel appears as a virtual capture device named **AetherSDR DAX IQ n** (PulseAudio/PipeWire source `aethersdr-iq-n`): float32 stereo, I on the left channel and Q on the right, at the rate you chose.

- **SDR#:** pick the audio IQ input, choose **AetherSDR DAX IQ 1**, and set the same sample rate.
- **GQRX:** use a PulseAudio I/Q input on **AetherSDR DAX IQ 1** at the matching rate.
- **GNU Radio:** an Audio Source on `aethersdr-iq-1` at the matching rate gives interleaved I/Q.

On other platforms, take IQ through the [TCI Server](./tci-server.md) instead, which is how CW Skimmer and SDC connect.

### WFM demodulator (satellite data)

AetherSDR can FM-demodulate a panadapter's DAX IQ stream on the computer, for G3RUH 9600-baud satellite data. The chain applies phase-continuous Doppler correction, resamples to exactly 48 kHz and uses a flat discriminator, then sends the audio to a virtual audio cable for a program such as HS-SoundModem.

1. In the panadapter overlay **DAX** flyout, set **IQ Ch** first.
2. Press **WFM**, directly below it. It works in any slice mode, on the flyout's slice.
3. The first time, a **Select WFM Audio Output Device** dialog asks for the output. Choose a virtual cable (for example Hi-Fi Cable, BlackHole or a PipeWire null sink) and optionally remember the choice.

WFM needs a radio with DAX IQ; on other radios it is refused with a notice.

## Reference

### The DAX IQ applet

The applet has one row per channel, **IQ 1** to **IQ 4**:

| Control | What it does |
|---|---|
| Rate | 24k, 48k, 96k or 192k |
| **Off / On** | Creates or removes the IQ stream for that channel |
| Level meter | Signal level in dBFS over a −70 to −10 dBFS window. Normal signals sit low; a full bar means you are near overload. |

The rate and on/off state of each channel are saved and restored at startup.

Higher rates use more of the radio's resources and more network bandwidth.

### Limits

- The radio reports how many IQ channels it can supply; four is the maximum.
- Receivers on the same panadapter share that panadapter's IQ stream, so four independent band views need four panadapters.

## Troubleshooting

### A program sees no AetherSDR DAX IQ device

The device only exists on Linux, and only while that channel's stream is on.

1. In the panadapter overlay **DAX** flyout, set **IQ Ch** for the panadapter you want.
2. In the **DAX IQ** applet, press **On** for that channel.
3. On macOS or Windows, connect the program through the [TCI Server](./tci-server.md) instead.
4. Set the program's sample rate to the rate chosen in the applet.

### WFM is refused with a notice

The connected radio has no DAX IQ.

1. Use a FlexRadio, which supplies DAX IQ.

## See also

- [DAX Virtual Audio](./dax-virtual-audio.md)
- [TCI Server](./tci-server.md)
