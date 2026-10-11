---
title: "NR2 Noise Reduction"
slug: "/nr2-noise-reduction"
description: "NR2 is a client-side spectral noise reduction method that complements the radio's built-in NR."
---

NR2 is a client-side spectral noise reduction method that complements the radio's built-in NR. It uses the Ephraim-Malah MMSE Log-Spectral Amplitude estimator, derived from Warren Pratt NR0V's WDSP library, and runs on your computer's CPU — not on the radio.

*Contributed by @EI6JGB*

For how NR2 compares with the other six client-side methods, see [DSP Noise Mitigation](./dsp-noise-mitigation.md).

## Requirements

FFTW3 is required to build AetherSDR (the bundled WDSP library depends on it). Release builds include it. When building from source:

- Arch: `sudo pacman -S fftw`
- Ubuntu/Debian: `sudo apt install libfftw3-dev`
- macOS: `brew install fftw`
- Windows: run `scripts/setup/setup-fftw.ps1`

See [Building from Source](./building-from-source.md).

## How it works

NR2 processes the demodulated receive audio on your CPU. It estimates the noise floor in each frequency bin and applies a spectral gain that suppresses noise while preserving speech. Like every client-side method, it processes the left and right channels independently, and it keeps its noise estimate across a transmission so it does not have to re-converge every time you unkey.

## Using NR2

### Turning NR2 on

- **AetherRX:** open **Settings → AetherRX...** (or the **AetherRX** button on the VFO flag's DSP tab), go to the **AetherNR** tab and select **NR2** in the method row. NR2's settings appear below it.
- **Shortcut:** bind **NR Cycle (Off/NR/NR2/NR4/DFNR)** under **Settings → Configure Shortcuts...**, or to a MIDI control.

NR2 is a voice method: it switches itself off in CW, CWL, RTTY and DIGU/DIGL.

### When to use NR2

- **Steady noise** — constant hiss or hum on a relatively quiet band, where you want to dial in exactly the right amount of suppression
- **Weak signals** where the radio's NR isn't enough
- **Stacking with radio NR** — NR and NR2 use different algorithms and can complement each other (enable NR on the radio, NR2 in AetherSDR)

NR2 is less effective against impulse noise (clicks, pops, lightning crashes); use the radio's NB or WNB for those.

### FFTW wisdom (first use)

The first time you enable NR2, AetherSDR optimizes the FFT plans it uses (FFTW "wisdom"). An **AetherSDR — FFTW Wisdom** window appears while this runs:

<img src="/img/screens/fftw-wisdom-window.png" width="500" alt="AetherSDR FFTW Wisdom window. It reads Computing COMPLEX FORWARD FFT size 2048... and This window will automatically close when wisdom generation is complete, above a progress bar at 38%, Working..., an elapsed time of 0:09 and a Cancel button." />

*The FFTW Wisdom window, shown the first time NR2 starts on a computer.*

- It shows progress, a **Working** activity indicator and an **Elapsed** time counter. Planning can take several minutes on some systems; if the progress pauses, the window says it is still working.
- The window is modeless — keep operating while it runs. On macOS it stays visible when AetherSDR is not the frontmost app.
- Clicking NR2 again while it runs brings the same window forward instead of starting a second run.
- **Cancel** stops after the FFT plan currently being computed. NR2 is then not enabled, and your audio is left unchanged.
- NR2 switches on when planning finishes, and the window closes itself.

The result is saved **as soon as planning finishes**, so a crash or force-quit afterwards does not lose it, and later launches skip the wait. The same cache also speeds up the WDSP receive setup on the [Hermes-Lite 2](./hermes-lite-2.md).

The wisdom file is stored at:

| Platform | Path |
|---|---|
| Linux / macOS | `~/.config/AetherSDR/aethersdr_fftw_wisdom` |
| Windows | `%APPDATA%\AetherSDR\aethersdr_fftw_wisdom` |

To force a fresh optimization (for example after upgrading FFTW), quit AetherSDR, delete that file, and enable NR2 again.

> **Upgrading from an older version:** older builds kept the file in a double-nested `~/.config/AetherSDR/AetherSDR/` folder. Current builds use the path above, so NR2 optimizes its plans once more after upgrading. You can delete the old file.

## Reference

### Settings

| Setting | Range / options | What it does |
|---|---|---|
| **Gain method** | Linear, Log, **Gamma**, Trained | The speech model used to compute the suppression gain. Gamma is the default. |
| **Noise estimation** | **OSMS**, MMSE, NSTAT | How the noise floor is tracked. Try NSTAT if the noise changes quickly. |
| **Reduction** | 0.50–2.00 | How aggressively noise is suppressed. |
| **Naturalness** | 0–0.15 | Leaves a small residual noise floor, trading depth for a more natural sound. |
| **Smoothing** | 0.50–0.98 | Smooths the gain over time to reduce musical artifacts. Higher is smoother but slower. |
| **Threshold** | 0.05–0.50 | Speech-detector sensitivity. Lower keeps quiet voices but lets more noise through. |
| **AE Filter (artifact elimination)** | On / off | Reduces ringing and musical artifacts. |
| **Noise fill (psychoacoustic)** | Off by default | Mixes a little shaped noise back into the gaps between syllables. **Fill level**, **Fill character** and **Fill bandwidth** shape it. |

Start with the defaults. See [Troubleshooting](#troubleshooting) if you hear musical warbling or weak voices disappear.

## Known issues

- NR2 can sound scratchy or produce clicks and artefacts on voice ([#632](https://github.com/aethersdr/AetherSDR/issues/632)).

## Troubleshooting

### Musical warbling in the background

The gain is changing too quickly from frame to frame.

1. Raise **Smoothing**.
2. Turn on **AE Filter (artifact elimination)** if it is off.

### Weak voices disappear

The speech detector is missing quiet speech and treating it as noise.

1. Lower **Threshold**.
2. If that is not enough, lower **Reduction** or raise **Naturalness**.

### The FFTW Wisdom window takes a long time

AetherSDR is optimizing its FFT plans the first time you enable NR2. This can take several minutes on some systems.

1. Keep operating; the window is modeless.
2. Wait for it to finish. NR2 switches on and the window closes itself, and later launches skip the wait.
3. If you click **Cancel**, NR2 stays off; enable it again to restart planning.

### NR2 turns off when you change mode

This is expected. NR2 is a voice method and switches off in CW, CWL, RTTY and DIGU/DIGL.

1. Switch back to a voice mode.
2. In CW, use the radio's NR and ANF instead.

### NR2 does nothing against clicks and crashes

NR2 targets steady noise, not impulse noise.

1. Use the radio's **NB** or **WNB** for clicks, pops and lightning crashes.

## See also

- [DSP Noise Mitigation](./dsp-noise-mitigation.md) — how NR2 compares with the other client-side methods
- [Aetherial Audio](./aetherial-audio.md)
- [Building from Source](./building-from-source.md)
- [Hermes-Lite 2](./hermes-lite-2.md)
