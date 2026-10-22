---
title: "RX Controls"
slug: "/rx-controls"
description: "The RX applet in the applet panel is the slice-centered receive control surface."
---

The **RX** applet in the applet panel is the slice-centered receive control
surface. It repeats the slice's identity at the top and gives you step size,
filter, audio, squelch, AGC and RIT/XIT for the selected slice, with more room
than the compact VFO flag. Radio-side DSP buttons (NR, NB, ANF, APF, NRL, NRS,
RNN, NRF, ANFL, ANFT) are on the VFO flag's DSP tab; see [VFO Widget](./vfo-widget.md).

Most of this page describes a FlexRadio. On other radio families, controls the
radio cannot honor are dimmed with the reason in their tooltip, or refuse
with a notice; see [Using the controls on other radios](#using-the-controls-on-other-radios) below.

<img src="/img/screens/rx-applet.png" width="248" alt="RX Controls applet tuned to 14.250.000 USB. The filter presets and passband graph sit beside the audio, squelch and AGC controls. The Med selector, enabled threshold slider at 41 and lit A button share one row; RIT and XIT are below." />

*The RX Controls applet with the compact A button beside the AGC-T slider.*

## Using the RX applet

### Header row

- **Slice badge** — the slice letter, in the slice's color (see
  [Slice Colors](./slice-colors.md))
- **Lock icon** — tune lock
- **Antenna** — RX and TX antenna buttons. They show the local display names
  you set in **Settings → Radio Setup... → Antennas** (for example "80m Dipole"), while the
  radio still uses ANT1/ANT2.
- **Filter width** — current filter bandwidth (reads **AUTO** while the
  adaptive RX filter is fitting); grouped at the right of the header,
  beside **QSK** in CW modes
- **QSK** — read-only full break-in indicator shown in CW modes. It follows
  the radio's state, including changes made at the radio or by another client.
  Use **Breakin** in the P/CW applet to request a change.
- **TX badge** — shows whether this slice transmits
- **Frequency** — double-click to enter directly

### Step size

Step stepper with ◀ ▶ buttons. Available sizes: 10, 50, 100, 250, 500, 1000,
2500, 5000, 10000 Hz. Step sizes are mode-dependent.

### Filter presets

Mode-dependent filter width buttons (FlexRadio):

- **USB/LSB**: 1.8K, 2.1K, 2.4K, 2.7K, 2.9K, 3.3K, 4K, 6K
- **CW**: 50, 100, 250, 400, 500, 600, 800, 1K
- **AM/SAM**: 5.6K, 6K, 8K, 10K, 12K, 14K, 16K, 20K
- **DIGU/DIGL**: 100, 300, 600, 1K, 1.5K, 2K, 3K, 6K
- **RTTY**: 250, 300, 350, 400, 500, 1K, 1.5K, 3K
- **DFM**: 6K to 20K in 2K steps
- **FM/NFM**: fixed by the radio

Radios that publish their own filter widths (such as networked Icom) show
those instead.

#### Adaptive RX filter (USB/LSB)

An automatic filter that fits the passband to the station you are hearing. It
is on the VFO flag's mode tab, under the filter-width buttons, not in this
applet: tick **Adaptive RX filter** and set the **Lo cut / Hi cut** bounds and
the **SNR**, **Speed**, **Splat** and **Het** options. The filter readout shows
**AUTO** while a fit is live, it is suspended while you transmit, and any
manual filter change takes back control. See [VFO Widget](./vfo-widget.md).

### AF gain, pan and mute

- **AF slider** (0–100) — slice audio volume
- **Pan slider** (L/R) — stereo balance with a center marker
- **Mute** (speaker icon):
  - **single-click** mutes or unmutes this slice;
  - **double-click** mutes or unmutes **all** your slices.

AetherSDR waits briefly after a click to see whether it is a double-click. Set
the wait in **Settings → Radio Setup... → Appearance & Behavior → Single-click
delay** (0–1000 ms, with **Reset** for the platform default). At 0, single
clicks act instantly and double-click actions are disabled.

Mute belongs to the radio: AetherSDR does not restore a slice's mute itself
when you reconnect.

### Squelch

The **SQL** button cycles through three states:

| State | Button | What the slider sets |
|---|---|---|
| **Off** | plain | — |
| **SQL** (manual) | lit | The squelch level |
| **AUTO** | amber, labeled AUTO | A margin of **5–20 dB** above the measured panadapter noise floor (default 10 dB) |

- In **AUTO**, squelch tracks the band's noise floor as it changes, opening
  only for signals that stand the margin above it. On FlexRadio, AUTO and its
  margin belong to each slice. Selecting another slice does not enable AUTO
  there or stop a background AUTO slice. Each slice uses only its own
  panadapter's noise floor; its SQL lines appear only on that panadapter.
  When multiple slices share a panadapter, the SQL overlay follows the selected
  slice on that pan, or the slice shown in its title while another pan is selected.
  AUTO starts at the remembered manual threshold until that pan supplies a floor.
- In **SQL**, a threshold line shows on the panadapter while you adjust it and
  hides itself 3 seconds after you stop.
- Your manual level is remembered **per slice**, whichever control changes it
  (applet, VFO flag, MIDI or other controllers, the radio or another client).
  Auto tracking never overwrites it, so switching back from AUTO returns to
  your manual level.
- On FlexRadio, SQL and AUTO are unavailable in **DIGU, DIGL, RTTY, NT,
  DFM and CW**. **RADE** follows its underlying DIGU/DIGL slice. Both panadapter
  squelch lines disappear and Auto tracking stops in these modes, including
  after incoming radio status. Data modes request SQL Off; firmware may reject
  that request and report its fixed native state. The client still stops AUTO
  and clears both lines. CW retains the radio's own squelch state without
  client Auto updates. Leaving AUTO through an unavailable mode cancels AUTO;
  returning to a supported mode restores SQL at that slice's remembered manual
  threshold after the radio reports the returning mode. Selecting
  another slice does not transfer or discard the pending recovery. DFM and
  RADE use digital-voice synchronization, so an analog squelch threshold does
  not control their decoder.
- On Hermes-Lite 2 and Icom profiles without mode-independent squelch, the
  existing CW, RTTY and data-mode restrictions also apply. Supported mode-independent
  Icom profiles retain native manual SQL in those modes.
- The squelch scale, the panadapter SQL line and Auto SQL follow each radio's
  own scale. On radios where Auto SQL is not available, a slice set to AUTO
  drops to manual at its manual level.

On RTL-SDR FM/FM-N, **AUTO** uses each receiver's own detector noise estimate, independently of panadapter zoom, FFT averaging or whether the applet is visible. Manual SQL uses dBFS per 2048-point detector bin; its threshold line is suppressed because the display FFT has a different scale. Manual and Auto settings are saved per RTL receiver, and Auto does not overwrite the manual threshold. WFM does not expose this FM/FM-N squelch control.

> **Upgrading from an older version:** FlexRadio squelch levels come from the radio at connection. For RTL-SDR, explicitly choose **AUTO** after upgrading to enable the receiver-owned Auto policy; an older display-Auto preference does not enable it.

### AGC

<img src="/img/screens/rx-agc-calibration.png" width="460" alt="AGC-T Calibration window. The heading reads AGC: MED, finding the knee (agc_threshold) just above the noise floor, with a warning that a signal is in the passband and you should tune to a clear spot. An empty plot with an AGC-T axis from 0 to 100 and a dashed marker fills the centre, above Auto Sweep and Apply buttons and a hint: tune to a clear spot, then Auto Sweep, or move the AGC-T slider and watch where the noise bends." />

*The AGC-T Calibration window.*

- **Mode** — Off, Slow, Med, Fast
- **AGC-T slider** — the AGC threshold
- **A** to the right of the RX applet's AGC slider, or **Auto** to the right of
  the slice flag's Audio-tab slider — requests radio Auto AGC-T. A lit button means
  the radio reports automatic threshold control; an unlit button means manual.
  This requires SmartSDR 4.3.16 or later and the radio's enabled **Auto AGC-T**
  entitlement (SmartSDR+ Early Access). Until support, entitlement and slice
  status are confirmed, the button is dimmed with the reason in its tooltip
  and screen-reader description. Other radio families keep it dimmed.

Auto AGC-T adjusts the threshold as band noise changes; the Off / Slow / Med /
Fast speed selector remains independent. The threshold slider stays enabled and
follows the radio. Moving it by hand requests a manual threshold; the **A** and
**Auto** indicators follow the mode the radio reports afterward. Click either
button again to request manual mode without changing
the current threshold. AetherSDR does not save or reapply the radio's automatic
or manual selection on reconnect.

**AGC-T calibration:** right-click the AGC-T slider, or use
**Tools → Calibrate AGC-T...**, to calibrate the threshold against the
measured noise floor on the active slice. It does not transmit.
Close the window with the corner ✕, Esc or Ctrl+W; closing it during
an Auto Sweep stops the sweep and puts back the AGC-T it started from.
Select manual AGC-T before calibrating. If radio Auto AGC-T or an external
receiver takes over, or the radio disconnects, calibration discards its curve
and recommendation without restoring its old threshold. Returning to manual
AGC-T does not resume the old sweep. After reconnecting, reopen
**Tools → Calibrate AGC-T...** to record a fresh manual curve.

On the Hermes-Lite 2, AGC runs on the computer: with AGC **Off**, the AGC-T
slider sets a fixed gain (4–64 dB, default 10 dB) remembered per receiver, and
**DIGU/DIGL open with AGC off**. See [Hermes-Lite 2](./hermes-lite-2.md).

On the ANAN-G2, AGC also runs on the computer. The AGC-T slider's 0–100
covers −20 to 120 dB of gain (default 71, about 79 dB), and with AGC **Off**
it sets the fixed gain instead. The slider always shows the setting the DSP is
using. See [ANAN-G2](./anan-g2.md).

### RIT / XIT

- **RIT** — Receiver Incremental Tuning (toggle, zero and step buttons)
- **XIT** — Transmitter Incremental Tuning (same layout)
- Step: 10 Hz per click

### FM controls (FM/NFM)

When FM or NFM is selected the applet shows repeater controls:

- Repeater **Offset** (MHz)
- Offset direction: **−**, **Simplex**, **+**, and **REV** (listen on the
  repeater input)

Tone mode and tone value (CTCSS) are on the VFO flag's FM tab. On networked
Icom radios the **REV** button becomes **XFC**, a momentary
transmit-frequency check.

## Using client-side noise reduction

AetherSDR's own noise-reduction methods run on your computer, one at a time:
**NR2**, **RN2**, **NR4**, **MNR** (macOS only), **BNR** (NVIDIA RTX/GeForce
GPU, Linux and Windows), **DFNR** and **NNR**. Choose a method and adjust it on
the **AetherNR** tab of **Settings → AetherRX...**, which also opens from the
**AetherRX** launcher on the VFO flag's DSP tab. The launcher lights while a
method is running. Right-clicking a DSP button gives a quick parameter popup.

- Each method processes the left and right channels separately, so pan and
  diversity are preserved.
- Client NR is for voice and turns itself off in CW, RTTY and digital modes.
- **BYPASS** at the foot of the AetherRX window turns off whichever method is
  running and restores it on the second click, for a quick A/B comparison.

See [DSP Noise Mitigation](./dsp-noise-mitigation.md) and [Aetherial Audio](./aetherial-audio.md).

## Using CW receive aids (APF and decoder)

- **APF** (audio peaking filter) toggle and level are in the **P/CW** applet in
  CW modes, as well as on the VFO flag. The level slider is inactive until APF
  is switched on.
- The CW decoder panel appears below the waterfall in CW modes. See
  [CW Decoder](./cw-decoder.md).

## Using WFM for satellite data

A software wide-FM demodulator for 9600-baud satellite data runs on the
computer from DAX IQ. It is in the panadapter's **DAX** flyout: set **IQ Ch**
first, then press **WFM** below it. The first time, choose a virtual audio
cable as its output. It needs a radio with DAX IQ; other radios refuse with a
notice. See [DAX IQ Streaming](./dax-iq-streaming.md).

## Using the controls on other radios

On the Hermes-Lite 2, ANAN-G2, RTL-SDR and networked Icom radios, every
control either works through the radio's own route, is **dimmed with its
reason** in the tooltip and screen-reader description, or **refuses with a
notice** (once per session) before anything is sent. Examples that refuse when
the radio cannot do them: antenna choices the radio has no port for, WFM,
CWL, and NR/ANF from MIDI or keyboard shortcuts. On these radios the **NR cycle** keyboard/controller action goes straight to
the client NR2 method, because the radio has no NR of its own. See [Supported Radios](./supported-radios.md).

## Known issues

- Programmatic/manual SQL admission across CAT, TCI, controllers, automation
  and memory recall remains under review
  ([#6276](https://github.com/aethersdr/AetherSDR/issues/6276)).
- Switching a slice from DIGU to USB can turn squelch on, silencing the audio until you switch it off ([#3505](https://github.com/aethersdr/AetherSDR/issues/3505)).

## Troubleshooting

### A slice is still muted after you reconnect

Mute belongs to the radio. AetherSDR does not change a slice's mute itself
when you reconnect, so the slice keeps the mute state the radio reports.

1. Single-click the slice's **Mute** (speaker icon) in the RX applet to unmute
   it.
2. To unmute all your slices at once, double-click **Mute**.

### Double-clicking Mute only mutes one slice

The single-click delay is set to 0, which disables double-click actions.

1. Open **Settings → Radio Setup... → Appearance & Behavior**.
2. Raise **Single-Click Delay** above 0, or press **Reset** for the platform
   default.

### Squelch switched off when you changed to RTTY

This is by design: squelch breaks the RTTY decode, so switching a slice to
RTTY turns it off.

1. When you leave RTTY, press **SQL** again if you want squelch back. Your
   manual level is remembered per slice.

### The APF level slider does nothing

The level slider is inactive until APF is on.

1. Switch on **APF** in the **P/CW** applet or on the VFO flag.
2. Adjust the level.

## See also

- [VFO Widget](./vfo-widget.md)
- [Slice Colors](./slice-colors.md)
- [DSP Noise Mitigation](./dsp-noise-mitigation.md)
- [Aetherial Audio](./aetherial-audio.md)
- [CW Decoder](./cw-decoder.md)
- [Supported Radios](./supported-radios.md)
