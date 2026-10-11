---
title: "RTL-SDR"
slug: "/rtl-sdr"
description: "AetherSDR can use an RTL-SDR USB dongle as a receiver."
status: "Experimental, receive-only"
applies_to: ["RTL-SDR"]
---

:::info[Status]

**Status:** Experimental, receive-only · **Applies to:** RTL-SDR

:::

AetherSDR can use an RTL-SDR USB dongle as a receiver. Support is **experimental and receive-only**: one slice on one panadapter, demodulated on this computer. FlexRadio remains the supported target (see [Supported Radios](./supported-radios.md)).

## Where to go next

- [Panadapter Controls](./panadapter-controls.md): FFT AVG, Black Level and NB Blank on the dongle's panadapter.
- [RX Controls](./rx-controls.md): modes, filters and squelch.
- [DSP Noise Mitigation](./dsp-noise-mitigation.md): client-side noise reduction.
- [Memory Channels](./memory-channels.md): stored on this computer.
- [Building from Source](./building-from-source.md): building with the RTL-SDR backend.

## Requirements

The RTL-SDR backend is built only when the `librtlsdr` library is available, so whether your copy of AetherSDR has it depends on the package. If **Connect by IP → Radio type:** has no **RTL-SDR (USB)** entry, and no dongle ever appears in the radio list, your build does not include it. The Windows installer is built without it ([#5800](https://github.com/aethersdr/AetherSDR/issues/5800)).

Building from source: the `ENABLE_RTL` option is on by default and takes effect when both `librtlsdr` and single-precision FFTW (`fftw3f`) are found. See [Building from Source](./building-from-source.md).

## Setup

1. Plug in the dongle.
2. Open **File → Connect to Radio...** → **On This Network**. Supported dongles on USB are listed with the network radios.
3. Select the dongle and click **Connect Selected Radio**.

## Using an RTL-SDR

- **Modes:** AM, SAM, FM, FM-N, WFM, USB, LSB, CW and CW-R.
- **Tuning range:** 24 kHz to 1.766 GHz. Below 24 MHz the dongle uses direct sampling; HF reception on particular dongles (for example ones with an upconverter or the RTL-SDR Blog V4) has not been qualified.
- **Sample rates** up to 3.0 MS/s; 2.4 MS/s is the default.
- **Browsing the capture:** pan around the captured bandwidth and your slice keeps its frequency and settings. A slice that falls outside the usable capture is parked (silent) and resumes when it fits again.
- **Zoom** uses a continuous 65,536-point FFT, which gives 36.6 Hz bins at 2.4 MS/s.
- **FM and FM-N** have symmetric filters, 48 kHz audio and their own squelch. Squelch exists only in FM and FM-N; its level is a signal-level gate in dBFS per bin, not calibrated dBm.
- **Confirmed settings:** a change is shown only after the dongle confirms it, so a refused setting never becomes a saved one.
- Manual **Black Level**, waterfall **NB Blank** and **FFT AVG** work on the panadapter. See [Panadapter Controls](./panadapter-controls.md).
- **Tools → Radio Health...** shows the dongle's status.

Only one receiver is available, even though the capture is wide.

## Reference

### RTL Receiver settings

**Settings → Radio Setup... → RTL Receiver** (shown while a dongle is connected) has a **Receiver corrections** group:

| Control | Use |
|---------|-----|
| **Frequency correction:** | Your dongle's crystal error in ppm (−1000 to +1000). Click **Apply frequency correction** to send it. |
| **Suppress IQ DC** | Removes the spike at the centre of the capture. Off by default. |
| **Device serial** | The serial number the dongle reports. |

The page shows the values the dongle actually applied and whether they were saved. Corrections are saved per dongle serial number. A dongle that reports no serial keeps its corrections for the current session only.

### What is remembered

Frequency, mode, passband, sample rate and RF gain are saved per dongle, keyed by the serial number it reports. Dongles that report no serial share one family-wide entry, and so do two dongles that report the same serial.

> **Upgrading from an older version:** saved RTL-SDR state used to be keyed by USB position. On the first connection after upgrading, a dongle whose only saved state is under the old key may start from defaults once. It then saves under its serial number (or the family-wide entry). The old entries are left in place but no longer read.

### Not available on an RTL-SDR

- Transmit.
- DAX, SmartLink or Multi-Flex.
- More than one slice or panadapter.

## Known issues

- An RTL-SDR Blog V4 on macOS shows only noise on the panadapter and in the audio, with no signals ([#6016](https://github.com/aethersdr/AetherSDR/issues/6016)).
- DX cluster, RBN, WSJT-X, POTA and manual spots are never drawn on the panadapter ([#6039](https://github.com/aethersdr/AetherSDR/issues/6039)).

## Troubleshooting

### No dongle in the list and no RTL-SDR (USB) radio type

Your copy of AetherSDR was built without the RTL-SDR backend.

1. Check **Connect by IP → Radio type:** for an **RTL-SDR (USB)** entry.
2. If it is missing, use a build that includes the backend, or build from source with `librtlsdr` and `fftw3f` installed (see [Building from Source](./building-from-source.md)).

### The slice goes silent while you pan

The slice has fallen outside the usable capture, so it is parked.

1. Pan back until the slice is inside the capture again; it resumes on its own.

### A spike sits at the centre of the capture

This is the dongle's IQ DC offset.

1. Open **Settings → Radio Setup... → RTL Receiver**.
2. Tick **Suppress IQ DC**.

### Stations appear off frequency

The dongle's crystal is off.

1. Open **Settings → Radio Setup... → RTL Receiver**.
2. Enter the error in ppm in **Frequency correction:**.
3. Click **Apply frequency correction**.

## See also

- [Supported Radios](./supported-radios.md)
- [Building from Source](./building-from-source.md)
- [Panadapter Controls](./panadapter-controls.md)
- [`docs/rtl-m1-runtime.md`](https://github.com/aethersdr/AetherSDR/blob/main/docs/rtl-m1-runtime.md)
- [`docs/rtl-slice-settings.md`](https://github.com/aethersdr/AetherSDR/blob/main/docs/rtl-slice-settings.md)
