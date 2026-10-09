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

## Requirements

Official AppImage, macOS DMG and Windows release packages require the RTL-SDR backend and bundle its RTL-SDR, USB and FFTW libraries. A developer build can omit it. If **Connect by IP → Radio type:** has no **RTL-SDR (USB)** entry, and no dongle ever appears in the radio list, your build does not include it. Packaging does not install a USB driver or Linux device-access rules.

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
- **FM and FM-N** have symmetric filters, 48 kHz audio and their own squelch. Squelch exists only in FM and FM-N; its manual threshold is a signal-level gate in dBFS per 2048-point detector bin, not calibrated dBm. **AUTO** follows a nearby noise estimate inside each receiver, independently of display zoom and FFT averaging. Its margin is 5–20 dB (default 10 dB). The SQL threshold line is not drawn against the differently scaled display FFT.
- **Confirmed settings:** a change is shown only after the dongle confirms it, so a refused setting never becomes a saved one.
- Manual **Black Level**, waterfall **NB Blank** and **FFT AVG** work on the panadapter. See [Panadapter Controls](./panadapter-controls.md).
- **Tools → Radio Health...** shows the dongle's status.

Normal launches admit **one receiver**, even though the capture is wide.

### Broadcast FM

Select **WFM** to use the **Broadcast FM** applet. It shows the selected slice letter and confirmed frequency. **Mono / Auto Stereo**, **De-emphasis** (50 or 75 µs) and **Bandwidth** belong to that receiver. **Auto Stereo** falls back to mono without a detected stereo pilot. The observed pilot status is separate from your selected audio mode; parking clears observations while retaining the controls.

### Receive meters

The RTL receive meter reports relative RF peak-bin level in **dBFS**, not calibrated antenna dBm or audio level. **Settings → Radio Setup... → RTL Receiver → Enable receive meters** turns these updates on or off. Turning them off clears the readout; reception, squelch and audio continue.

### Sending receive audio to another program

Use the [TCI Server](./tci-server.md) on Linux, macOS or Windows. On Linux and macOS, [DAX Virtual Audio](./dax-virtual-audio.md) can also export receive audio when the build includes WebSockets support. DAX input 1 follows TCI receiver 0, and the applet shows its slice assignment. Linux DAX is mono; macOS DAX and stereo TCI retain left and right WFM audio. Speaker volume and mute do not change either export; receiver squelch still applies.

[AetherModem](./aethermodem-packet-radio.md) can decode packets from the selected RTL slice. For VHF APRS, choose **FM** or **FM-N** and **1200 baud**. The modem follows slice selection; it has no fixed-source selector or APRS-IS gateway.

### Multi-receiver evaluation

The process-only evaluation setting admits 2, 4 or 8 receivers on Linux x86-64, Windows x64 and macOS arm64. It requires both `AETHER_AUTOMATION=1` and `AETHER_RTL_EVALUATION_RECEIVERS=2`, `4` or `8` at launch; it is not saved and cannot resize a running session. Use an isolated settings profile for evaluation. It does **not** change the one-receiver production limit or qualify a platform for sustained multi-receiver use.

FM, FM-N and analog WFM can share the capture in this evaluation. Each receiver retains its frequency, filter, squelch and audio controls when parked. Selecting another slice does not retune the capture or change its audio settings. AM, SAM, USB, LSB, CW and CW-R remain singleton configurations, even if another configured receiver is parked. See the [evaluation procedure](https://github.com/aethersdr/AetherSDR/blob/main/docs/rtl-multirx-analog.md) for the validation boundaries.

## Reference

### RTL Receiver settings

**Settings → Radio Setup... → RTL Receiver** (shown while a dongle is connected) has a **Receiver corrections** group:

| Control | Use |
|---------|-----|
| **Frequency correction:** | Your dongle's crystal error in ppm (−1000 to +1000). Click **Apply frequency correction** to send it. |
| **Suppress IQ DC** | Removes the spike at the centre of the capture. Off by default. |
| **Enable receive meters** | Enables relative RF meter updates. On by default. |
| **Device serial** | The serial number the dongle reports. |

The page shows the values the dongle actually applied and whether they were saved. Corrections are saved per dongle serial number. A dongle that reports no serial keeps its corrections for the current session only.

### What is remembered

Frequency, mode, passband, sample rate and RF gain are saved per dongle, keyed by the serial number it reports. Dongles that report no serial share one family-wide entry, and so do two dongles that report the same serial.

> **Upgrading from an older version:** saved RTL-SDR state used to be keyed by USB position. On the first connection after upgrading, a dongle whose only saved state is under the old key may start from defaults once. It then saves under its serial number (or the family-wide entry). The old entries are left in place but no longer read.

### Not available on an RTL-SDR

- Transmit.
- DAX transmit, DAX IQ, SmartLink or Multi-Flex.
- More than one receiver in a normal launch, or more than one panadapter.

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

- [RX Controls](./rx-controls.md)
- [DAX Virtual Audio](./dax-virtual-audio.md)
- [TCI Server](./tci-server.md)
- [AetherModem Packet Radio](./aethermodem-packet-radio.md)
- [Supported Radios](./supported-radios.md)
- [Building from Source](./building-from-source.md)
- [Panadapter Controls](./panadapter-controls.md)
- [`docs/rtl-m1-runtime.md`](https://github.com/aethersdr/AetherSDR/blob/main/docs/rtl-m1-runtime.md)
- [`docs/rtl-slice-settings.md`](https://github.com/aethersdr/AetherSDR/blob/main/docs/rtl-slice-settings.md)
