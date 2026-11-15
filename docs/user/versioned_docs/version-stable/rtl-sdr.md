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

The radio tab shows **Connected** with a green status dot once the USB connection is established. Amber indicates discovery; a USB receiver does not need a network heartbeat to show its connected state.

## Using an RTL-SDR

- **Modes:** AM, SAM, FM, FM-N, WFM, USB, LSB, CW and CWR.
- **Tuning range:** 24 kHz to 1.766 GHz. Below 24 MHz the dongle uses direct sampling; HF reception on particular dongles (for example ones with an upconverter or the RTL-SDR Blog V4) has not been qualified.
- **Sample rates** up to 3.0 MS/s; 2.4 MS/s is the default.
- **Browsing the capture:** pan around the captured bandwidth and your slice keeps its frequency and settings. A slice that falls outside the usable capture is parked (silent) and resumes when it fits again.
- **Zoom** uses a continuous 65,536-point FFT, which gives 36.6 Hz bins at 2.4 MS/s.
- **FM and FM-N** have symmetric filters, 48 kHz audio and their own squelch. Squelch exists only in FM and FM-N; its manual threshold is a signal-level gate in dBFS per 2048-point detector bin, not calibrated dBm. **AUTO** follows a nearby noise estimate inside each receiver, independently of display zoom and FFT averaging. Its margin is 5–20 dB (default 10 dB). The SQL threshold line is not drawn against the differently scaled display FFT.
- **Confirmed settings:** a change is shown only after the dongle confirms it, so a refused setting never becomes a saved one.
- Manual **Black Level**, waterfall **NB Blank** and **FFT AVG** work on the panadapter. See [Panadapter Controls](./panadapter-controls.md).
- **Tools → Radio Health...** shows the dongle's status.

Normal launches admit **one receiver**, even though the capture is wide.

The **RX ANT** controls show the dongle's fixed **RX** input. They do not offer FlexRadio antenna ports. The RX applet and VFO antenna button are dimmed unless a configured virtual receiver is available. **TX ANT** shows **None** and is dimmed because RTL-SDR cannot transmit.

### SSB and CW

Select **USB** or **LSB** for single-sideband reception, or **CW** / **CWR** (reverse CW) for Morse. These modes use independent receive DSP and 48 kHz audio. Filter presets and draggable edges change the actual receive passband. The CW marker remains on the station carrier; **Pitch** changes the audio tone without moving the dial frequency. Pitch is shared by the RTL session's CW receivers.

**AGC** offers Off, Slow, Med and Fast. **AGC-T** sets the local DSP's maximum gain; with AGC Off, **Off Level** controls fixed gain. These are local receiver controls, not a calibrated match for a FlexRadio's AGC-T scale. **NB** operates on receiver IQ; **APF** narrows CW audio around the selected pitch. **RIT** offsets reception while keeping the displayed dial frequency. These settings belong to each slice, except the shared CW pitch, and survive parking and reconnecting.

The speaker icon opens a slice's audio controls; right-click it to toggle mute. A crossed speaker marks muted audio. The RX Controls speaker button also follows the selected slice's mute state.

The [CW decoder](./cw-decoder.md) follows the selected CW or CWR slice. [Copy Assist](./copy-assist.md) follows the selected voice slice's independent audio before speaker volume, mute and client audio effects. A muted slice can still decode; a parked slice cannot. Selecting another slice or parking it discards queued audio from the old source. Copy Assist needs a downloaded speech model.

Each slice remembers its last accepted filter separately for each mode. Switching from USB to FM and back recalls that slice’s USB passband, including after reconnecting. A mode you have not used on that slice starts with its normal default filter. Pending or refused filter changes do not replace the remembered passband.

**NR**, **ANF** and **MN** run independently inside each SSB/CW receiver. NR reduces noise, ANF suppresses sustained tones, and MN places a fixed-width notch within the passband. Their settings use the local WDSP algorithms, so the values are not equivalent to a FlexRadio's settings. [Tracking notch filters](./tnf-tracking-notch-filters.md) stay at an RF frequency as receivers tune past them.

Client **AetherDSP** voice processing applies to the speaker stream; it is not independent per-slice processing and is not included in the native Copy Assist feed.

### AM and synchronous AM

**AM** uses envelope detection. **SAM** follows the station carrier with a phase-locked synchronous detector; it combines both received sidebands. Both modes have independent 48 kHz audio and the same **AGC**, **AGC-T**, **Off Level**, **NB**, **NR**, **ANF** and **RIT** controls described above. **APF** remains a CW control, and squelch remains available only in FM and FM-N.

Filter edges describe RF relative to the displayed carrier. Each edge can be moved independently, from −12,000 to −100 Hz below the carrier and +100 to +12,000 Hz above it. Narrow the interfered side while keeping the other side wide. Switching between AM and SAM preserves these cuts. SAM needs a received carrier within its tracking range (up to ±2 kHz); tune close to the station for acquisition. If the carrier jumps after a dropout and does not recover, retune to restart acquisition. It is not a suppressed-carrier SSB decoder.

**MN** places a 400 Hz notch in both RF sidebands at the selected audio frequency, leaving the carrier intact. Tracking notches keep their absolute RF positions; placing one on the station carrier can prevent SAM acquisition. The receiver removes demodulated carrier/DC from its audio, independently of **Suppress IQ DC**. Copy Assist, TCI and DAX receive the independent audio before speaker mute and volume.

Tuning or using **Move capture away from DC** places the carrier away from the converter's centre where possible. Free panning preserves station frequencies. If you bring an AM/SAM carrier onto the capture centre while **Suppress IQ DC** is enabled, move the capture away or disable that correction: removing the wanted carrier distorts AM and can break SAM tracking.

### Broadcast FM

Select **WFM** to use the **Broadcast FM** applet. It shows the selected slice letter and confirmed frequency. **Mono / Auto Stereo**, **De-emphasis** (50 or 75 µs) and **Bandwidth** belong to that receiver. **Auto Stereo** falls back to mono without a detected stereo pilot. The observed pilot status is separate from your selected audio mode; parking clears observations while retaining the controls.

### Receive meters

The RTL receive meter reports relative RF peak-bin level in **dBFS**, not calibrated antenna dBm or audio level. **Settings → Radio Setup... → RTL Receiver → Enable receive meters** turns these updates on or off. Turning them off clears the readout; reception, squelch and audio continue.

### Sending receive audio to another program

Use the [TCI Server](./tci-server.md) on Linux, macOS or Windows. On Linux and macOS, [DAX Virtual Audio](./dax-virtual-audio.md) can also export receive audio when the build includes WebSockets support. DAX input 1 follows TCI receiver 0, and the applet shows its slice assignment. Linux DAX is mono; macOS DAX and stereo TCI retain left and right WFM audio. Speaker volume and mute do not change either export; receiver squelch still applies.

[AetherModem](./aethermodem-packet-radio.md) can decode packets from the selected RTL slice. For VHF APRS, choose **FM** or **FM-N** and **1200 baud**. The modem follows slice selection; it has no fixed-source selector or APRS-IS gateway.

### Multi-receiver evaluation

The process-only evaluation setting admits 2, 4 or 8 receivers on Linux x86-64, Windows x64 and macOS arm64. It requires both `AETHER_AUTOMATION=1` and `AETHER_RTL_EVALUATION_RECEIVERS=2`, `4` or `8` at launch; it is not saved and cannot resize a running session. Use an isolated settings profile for evaluation. It does **not** change the one-receiver production limit or qualify a platform for sustained multi-receiver use.

AM, SAM, FM, FM-N, analog WFM, USB, LSB, CW and CWR can share the capture in this evaluation. Each receiver retains its frequency, filter, squelch and audio controls when parked. Selecting another slice does not retune the capture or change its audio settings. See the [evaluation procedure](https://github.com/aethersdr/AetherSDR/blob/main/docs/rtl-multirx-analog.md) for the validation boundaries.

## Reference

### RTL Receiver settings

**Settings → Radio Setup... → RTL Receiver** (shown while a dongle is connected) shows the dongle's **Device serial:** and a **Receiver settings** group:

| Control | Use |
|---------|-----|
| **Frequency correction:** | Your dongle's crystal error in whole ppm (−1000 to +1000). It applies on its own shortly after an arrow step or a completed entry (Enter or moving focus); **Apply frequency correction** sends it at once. Reception briefly reacquires after each correction. Closing or leaving the page discards an edit that has not been sent yet. |
| **Suppress IQ DC** | Removes the spike at the center of the capture. It also affects a signal right at the capture center, especially an AM or CW carrier. Off by default. |
| **Enable receive meters** | Enables relative RF meter updates. On by default. |

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

### A spike sits at the center of the capture

This is the dongle's IQ DC offset.

1. Open **Settings → Radio Setup... → RTL Receiver**.
2. Tick **Suppress IQ DC**.

### Stations appear off frequency

The dongle's crystal is off.

1. Open **Settings → Radio Setup... → RTL Receiver**.
2. Adjust **Frequency correction:** with the arrows, or type the error in ppm and press Enter; it applies on its own. **Apply frequency correction** sends it at once.

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
