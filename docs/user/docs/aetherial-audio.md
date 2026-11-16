---
title: "Aetherial Audio"
slug: "/aetherial-audio"
description: "Aetherial Audio is AetherSDR's client-side audio processing."
---

Aetherial Audio is AetherSDR's client-side audio processing. It runs on your
computer, independent of the radio's own DSP, and comes as two windows:

- **AetherTX** — the transmit voice chain: RN2 noise reduction, tube preamp,
  EQ, leveler, CFC multiband compressor, compressor, AetherVoice exciter, reverb
  and a brickwall limiter.
  Open it from **Tools → AetherTX...**
- **AetherRX** — the receive chain: noise reduction, gate, EQ, compressor,
  tube, AetherVoice and an output meter. Open it from
  **Settings → AetherRX...**

The two chains keep completely separate settings, so you can shape your
microphone for transmit and the band for listening without one disturbing the
other. A docked **Aetherial Audio** applet in the applet panel gives you a
compact view of both chains (see [Using the docked applet](#using-the-docked-applet)).

> **Upgrading from an older version:**
> - The old *Aetherial Audio Channel Strip* window is gone. Its transmit half
>   is now AetherTX and its receive half is AetherRX.
> - Channel-strip presets are retired. The first time you open the new
>   windows, each saved preset is split once into a TX profile and an RX
>   profile, so nothing is lost.
> - The **LIM** final limiter on the transmit Final Output stage is now a true
>   brickwall with look-ahead, and it is **on by default** at a −0.5 dB ceiling
>   with 1.5 ms of look-ahead. An installation that has saved a LIM setting
>   (by changing anything in Final Output or applying a TX profile) keeps it;
>   one that never did gets the new default. To get the new limiter on an
>   installation saved with LIM off, turn LIM on in Final Output.
> - **BYPASS** is remembered across restarts.
> - AetherTX's **Gate** and **De-esser** are gone. If you had the TX gate on,
>   **RN2** was switched on for you. An old profile that does not set RN2
>   itself sets it to match its gate, on or off. A chain order still at the
>   old default becomes the new one, with **Tube** first; an order you
>   changed keeps its order without either stage. Old gate and de-esser
>   settings are ignored.
> - The compressor's **Drive** and **Phase** are saved in AetherTX profiles.

<img src="/img/screens/aethertx.png" width="720" alt="AetherTX window. The left column lists the transmit chain stages Gate (selected), EQ, De-esser, Compressor, Tube, Exciter, Reverb and Final Output, with MIC and TX indicators, REC, PLAY, a settings gear and BYPASS at the bottom. The right side shows the gate's level graph with View (Level, Curve), Mode (Gate, Expander), RN2 and Peek controls, and six knobs: Thresh, Return, Hold, Release, Floor and Ratio." />

*The AetherTX window: the transmit processing chain on the left, the selected stage's controls on the right.*

## Requirements

AetherTX needs a computer microphone. AetherRX has no requirements: it
processes the receive audio the radio already sends you.

AetherTX processes a microphone connected to **this computer**. A mic plugged
into the radio bypasses it. The window shows a notice that names the fix
whenever the audio path is wrong:

- On a FlexRadio, set **Microphone source** to **PC** in the Phone/CW (P/CW)
  applet.
- On a radio that takes its transmit audio over the network (such as a
  networked Icom), turn on **PC Audio** in the title bar.
- On a radio that takes its transmit audio from this computer (such as a
  networked Icom or a Hermes-Lite 2), connect a microphone to the computer
  when the notice says no audio input device was found. Without one,
  AetherTX has no input, and a Hermes-Lite 2 transmits no voice at all.
  CW, TUNE and DAX/TCI are not affected. On a Hermes-Lite 2 (a radio this
  computer modulates), the first voice transmission in that state also shows
  the notice on the panadapter. It does not stop the transmission.

While the path is wrong, the docked Aetherial Audio applet replaces its TX
controls with the same guidance, and AetherTX and the TX stage editors refuse
to open. Receive views stay available. Fixing the path restores the controls.

DAX/TCI transmit audio (WSJT-X, fldigi and other digital-mode programs) and
RADE digital voice never pass through AetherTX, so their tones reach the radio
unshaped.

## Using the AetherTX and AetherRX windows

AetherTX and AetherRX share one layout:

- **The tab column on the left is the chain.** Each stage has a row with an
  enable checkbox and a drag grip. Tick the box to switch the stage on, and
  drag a row to change the order the audio meets the stages. The order is
  saved.
- Click a row to show that stage. Every stage page has the same shape:
  switches along the top, the display under them, and every knob in one row
  at the foot.
- Knobs accept typed values as well as dragging.
- The foot of the tab column holds the live controls: a **REC / PLAY** pair,
  **BYPASS**, and a **Settings** gear for the profile library.
- **The window itself.** Neither window has a title bar. Drag any empty
  part of it to move it, and drag an edge or corner to resize it. Close it
  with the round **✕** in the top-right corner, **Esc**, or **Ctrl+W**
  (**⌘W** on macOS). Each is a window of its own, with its own taskbar entry,
  so you can minimize it from the taskbar or park it on another monitor.
  **View → Frameless Window** does not apply to them. Pressing **Return** in
  a knob's value field only commits the value.
- Both windows open at the same size every time. Where you leave them is
  remembered.

Right-clicking a DSP button elsewhere in the app (for example on the VFO flag)
still opens a quick parameter popup without opening either window.

## Using AetherTX

### Signal path

The transmit voice chain runs at a fixed **48 kHz in floating point**. The
microphone is captured at whatever rate the device negotiates, converted to
float once and brought to 48 kHz. After the chain, a single high-quality
conversion takes it to the radio's 24 kHz transport rate, with dither, before
the one final conversion to 16-bit.

```
PC mic → (RN2) → Tube → EQ → LVLR → CFC → Compressor → Exciter → Reverb
       → MB limiter → Peak control → Final Output (Quindar, limiter, meters) → radio
```

The middle seven stages are the reorderable chain; the order above is the
default. **MB limiter** and **Peak control** are not part of it: they always
run last, in that order, after the chain, because they bound the peaks of
everything ahead of them. If you saved your own order before LVLR and CFC
existed, they are added
at the end of it; drag them ahead of the Compressor to match the default.
**RN2** is not a chain stage: when it is on it always runs first, ahead of
every stage, so the tube and everything after it work on denoised audio. Its
switch is on the **Tube** page.

AetherTX has no gate or de-esser. RN2 keeps room noise out of your pauses,
with no threshold to set. To tame sibilance, use a gentle EQ cut around
5–7 kHz. AetherRX has its own gate.

### Stages

| Tab | What it does |
|---|---|
| **Tube** | Dynamic tube preamp/saturator. Also holds the **RN2** microphone noise reducer, which runs before it. |
| **EQ** | Parametric EQ with an optional reference target curve. |
| **LVLR** | Leveler: a slow gain rider that brings a quiet or distant voice up to a steady level. Off by default. |
| **CFC** | Continuous Frequency Compressor: a multiband compressor whose compression and post-EQ are curves over frequency. Off by default. |
| **Compressor** | Compressor with Drive/Phase peak-to-average (PAPR) controls and its own brickwall limiter. |
| **Exciter** | AetherVoice™ harmonic exciter. |
| **Reverb** | Freeverb room reverb. Off by default. |
| **MB limiter** | Four-band look-ahead limiter: holds each band's peaks under its own threshold, so one band (a boomy low, sibilance) cannot dominate what follows. Fixed ahead of Peak control; off by default. |
| **Peak control** | Clipper and CESSB overshoot control that holds the SSB envelope under a ceiling, for more average power at the same peak. Always last; off by default. |
| **Final Output** | Final limiter (**LIM**), DC block, test tone, Quindar tones, output meters with **HOLD**, and a waveform scope. |

### REC / PLAY: the transmit monitor

**REC** records up to 30 seconds of processed transmit audio without keying
the radio. Click it again to stop early; playback then starts by itself.
**PLAY** plays the capture back; click it during playback to cancel. The
receive audio is muted while recording and playing back so you hear only your
own voice.

Use it to tune the chain without putting a signal on the air: record a test
phrase, listen, adjust a stage, record again. A phrase such as "The quick
brown fox jumps over the lazy dog. Five or six big jet planes flew over the
quiet countryside" covers most sounds.

Each recording is also written to `pudu_monitor.wav` in the system temporary
folder, overwritten every time, so you can inspect it in an audio editor.

### BYPASS

**BYPASS** switches every transmit stage off at once, including RN2, so the
microphone reaches the radio unprocessed. Click it again to restore exactly
the stages that were on. It is the quickest A/B test of what the chain is
doing.

### MIC and TX indicators

At the foot of the tab column, **MIC** lights when the PC microphone is
selected and DAX is off, which means the chain really is in the transmit
path. On a Hermes-Lite 2 (a radio this computer modulates), it also needs an
audio input device on this computer. **TX** lights while you are transmitting
on your own slice.

## Using AetherRX

AetherRX processes the audio the radio is already sending you, so there is no
routing to set up. The radio's own AGC, noise blanker and noise reduction run
first; AetherRX adds finishing on top.

### Stages

| Tab | What it does |
|---|---|
| **AetherNR** | The client-side noise-reduction methods (NR2, RN2, NR4, MNR, BNR, DFNR and NNR) with their full settings. Always first. See [DSP Noise Mitigation](./dsp-noise-mitigation.md). |
| **Gate** | Expander or gate, useful for knocking down hiss between overs. |
| **EQ** | Parametric EQ. The page also draws the slice's receive filter edges; dragging an edge changes the receive filter. |
| **Compressor** | Gentle levelling between loud and quiet stations. |
| **Tube** | Saturation for warmth, in small doses. |
| **Exciter** | AetherVoice presence and body. |
| **Final Output** | Output trim, **MUTE** and **BOOST**, output meter and waveform scope. Always last. |

Gate, EQ, Compressor, Tube and Exciter are the reorderable chain. There is no
reverb on receive.

### REC / PLAY and TX Playback

AetherRX's **REC / PLAY** pair records and plays back received audio through
the same path as the record and play buttons on the VFO flag.

Right-click **PLAY** for **TX Playback**, which transmits the last client-side
recording on the active slice. Select it again to stop. Playback is capped at
the idle timeout set in the recording settings (**Settings → Radio Setup... → Audio**), and the
transmitter is released if anything refuses or the radio disconnects.

### BYPASS

RX **BYPASS** suppresses every receive stage at once, AetherNR included, so you
hear the radio's audio unprocessed. Click it again to restore whatever was on,
including the noise-reduction method that was running.

### What is useful on receive

- **EQ:** pull harshness out of bright stations, add body to thin ones, tame a
  fatiguing midrange bump over a long contest. Boosting above your receive
  filter is wasted.
- **Gate in Expander mode:** knocks down inter-syllable hiss on a noisy band
  without the pumping of a hard gate.
- **Compressor at a low ratio** (1.5:1 to 2:1, 2–4 dB of reduction): evens out
  strength differences in a pile-up so you are not riding the AF gain.
- **Tube and Exciter:** a little warmth or presence on thin stations. Easy to
  overdo.

What to avoid: heavy compression lifts weak signals and noise along with
strong ones, which is the radio's AGC job; and high tube drive just adds
harmonic mud to a noisy band.

## Using profiles

The **Settings** gear at the foot of each window opens its profile library
(**AetherTX Settings** or **AetherRX Settings**). A profile is the whole chain:
every stage's settings, which stages are on, and their order. An AetherRX
profile also records the active noise-reduction method.

<img src="/img/screens/aetherrx-profiles.png" width="460" alt="AetherRX Profiles dialog. It explains that a profile is the receive chain, every stage's settings, which are on, their order and the noise reduction method, and that loading one leaves the transmit side alone. An empty profile list sits above Save..., Load, Delete, Import... and Export... buttons, with the note No profiles yet, Save... stores the chain you have set up now, and a Close button." />

*The AetherRX Profiles dialog, opened from the gear button at the foot of the stage column.*

| Button | Action |
|---|---|
| **Save…** | Store the chain as it is now under a name (asks before replacing an existing name) |
| **Load** | Apply the selected profile (double-click does the same) |
| **Delete** | Remove the selected profile |
| **Import…** | Read a profile from a JSON file and save it under a name you choose |
| **Export…** | Write the selected profile to a JSON file to share or back up |

Loading a TX profile leaves the receive side alone, and the other way round.

### Stock presets (AetherTX)

AetherTX Settings lists three stock presets above your own profiles, marked
**(stock)**. They are read-only: load one, then **Save…** it under a new name
if you want to change it. A stock preset's name can't be used for a profile
of your own.

Loading a stock preset also sets the radio's **TX filter** to the one it was
tuned for, so Peak control's passband matches. If you aren't connected, or the
radio's TX filter can't be set from AetherSDR, the status line tells you the
filter to set yourself. On a radio with fixed TX filter edges, the nearest
edges are used.

| Preset | TX filter | For |
|---|---|---|
| **SSB 3 kHz** | 100–3100 Hz | Everyday SSB: clean, full-sounding voice with dense average power |
| **DX Punch 2.7 kHz** | 200–2900 Hz | Weak-signal DX and pileups: narrower, with the lows trimmed for presence |
| **ESSB 6 kHz** | 50–6050 Hz | Wide, natural ESSB with extended lows and air, for rag-chews on open bands |

All three use the same core: the Compressor sets a hot, even level with its
phase rotator on, Peak control turns that into SSB envelope density, and LIM
holds the final ceiling. Tube, LVLR, CFC, Exciter, Reverb and the MB limiter
are off, and **RN2 stays as you set it**.

| Stage | SSB 3 kHz | DX Punch 2.7 kHz | ESSB 6 kHz |
|---|---|---|---|
| EQ | HPF 100, −2 dB @ 250, +3 dB @ 2.2k | HPF 200, +3 dB @ 2.2k | HPF 50, low shelf +2 dB @ 120, high shelf +3 dB @ 4k |
| Compressor | 4:1 at −24 dB, attack 20, release 150, makeup +16, Phase 6, limiter −1 | as SSB | 3:1 at −22 dB, attack 25, release 200, makeup +14, Phase 6, limiter −1 |
| Peak control | Clip 5 dB, CESSB on, Ceiling −1 | Clip 4 dB, CESSB on, Ceiling −1 | Clip 5 dB, CESSB on, Ceiling −1 |
| Final Output LIM | On, −0.5 dB, 1.5 ms | same | same |

Peak control is set to run in the radio's modulator; on a radio without one
(any FLEX), it runs in AetherTX's chain, as usual. Each preset adds about
41 ms of delay: Peak control (about 39 ms) and LIM's look-ahead (1.5 ms).

**How they were tuned.** On a FLEX-8600 at 25 W into a dummy load, with a
synthesized voice, each preset is the setting with the highest average
forward power whose received audio stayed clearly copyable. Copyability was
scored as intelligibility (STOI) of the transmitted signal received back
through a sampler, with 0.75 as the floor. Against the same chain bypassed:

| Preset | Average forward power | Envelope PAPR | Copyability (STOI) |
|---|---|---|---|
| Bypassed (each filter) | 2.2–3.7 W | 11.0–11.7 dB | 0.85–0.89 |
| **SSB 3 kHz** | 15.1 W | 5.4 dB | 0.77 |
| **DX Punch 2.7 kHz** | 13.4 W | 6.4 dB | 0.76 |
| **ESSB 6 kHz** | 15.3 W | 5.2 dB | 0.78 |

At the same 25 W setting, average forward power rose 6–8 dB. At an equal
peak, the gain is the drop in envelope PAPR: 4.6–6.4 dB. Your mic and voice
differ from the test voice, so check with **REC / PLAY**, and use **Trim**
or the radio's mic level so that peaks reach Peak control's ceiling.

### How the AetherTX stages fit together

| Stage | Where it runs | What it is for | Delay |
|---|---|---|---|
| RN2 | Ahead of everything | Room noise | About 10 ms when on |
| Tube | Chain | Color and density | – |
| EQ | Chain | Tone, high- and low-pass | – |
| LVLR | Chain | A slow level rider for quiet or distant voices | About 7 ms at 1 ms attack |
| CFC | Chain | Compression and EQ as curves over frequency | 21 ms at FFT 1024 |
| Compressor | Chain | Level and density; Drive and Phase | – |
| Exciter | Chain | Presence harmonics | – |
| Reverb | Chain | Room sound | – |
| MB limiter | Fixed, after the chain | Per-band peak control, for tone | 5 ms |
| Peak control | Fixed, after the MB limiter | SSB envelope density: clipper, CESSB, ALC | About 39 ms |
| Final Output LIM | Fixed, last | 48 kHz sample ceiling | The look-ahead, 1.5 ms by default |

Individual settings live in AetherSDR's settings database; you can inspect
them with **Settings → Settings Browser...** (see [Settings and Backups](./settings-and-backups.md)).

## Using the docked applet

The **Aetherial Audio** applet in the applet panel (button **VUDU**) is the
compact companion to the two windows:

<img src="/img/screens/txdsp-docked-applet.png" width="248" alt="Aetherial Audio applet. The top row has TX (selected), RX, a record button, a play button and BYPASS. Below, the transmit chain is drawn as connected boxes: an unlabeled green input stage, GATE, EQ, DESS, COMP, TUBE, EVO, VERB and TX, with the hint Click to bypass, Double click to edit, Drag to reorder. Under the strip is the Aetherial TX Gate tile with its transfer curve and knobs for Thresh, Ratio, Return, Release and Floor." />

*The docked Aetherial Audio applet (button VUDU) on the TX side: the chain strip, with the first stage tile below it.*

- **TX / RX** selects which chain is shown, and **BYPASS** bypasses that chain.
- The chain strip shows each stage as a box. Single-click a box to switch the
  stage on or off, double-click it to open that stage's editor, and drag it to
  reorder. The delay used to tell a single click from a double click is
  **Settings → Radio Setup... → Appearance & Behavior → Single-Click Delay**.
- On the TX side, ⏺ / ▶ drive the same transmit monitor as AetherTX's
  REC / PLAY.
- Compact tiles for individual stages sit below the strip.

## Reference

The ranges below are the knob ranges in the current windows.

### Gate (AetherRX)

Knocks down hiss between overs. AetherTX has no gate: use **RN2** on the Tube
page.

**Mode:** **Gate** (hard, snaps to 10:1 ratio and −40 dB floor) or
**Expander** (gentle, snaps to 2:1 and −15 dB). Switching mode changes only
ratio and floor; every other knob stays where you put it.

**View:** **Level** shows the live level history; **Curve** shows the static
transfer curve.

**Peek:** look-ahead, in steps of Off, 1, 1.5, 3 and 5 ms, so the gate opens
before a transient instead of clipping its start.

| Knob | Range | What it does |
|---|---|---|
| **Thresh** | −80 to 0 dB | Level below which the gate begins to close |
| **Return** | 0 to 20 dB | Hysteresis: opens above Thresh, closes below (Thresh − Return) |
| **Hold** | 0 to 500 ms | Time the gate stays open before release starts |
| **Release** | 5 to 2000 ms | How fast the gate closes after the signal drops |
| **Floor** | −80 to 0 dB | Maximum attenuation the gate applies |
| **Ratio** | 1 to 10 : 1 | Steepness of the expansion |

**Example: hiss between overs.** Start in **Expander**. Set **Thresh** just
above the band noise and below the quietest speech you want to hear, **Release**
about 80 ms, **Floor** −15 dB so the band never goes completely dead, and
**Return** about 3 dB so the gate doesn't flutter near the threshold.

### EQ

A parametric EQ with ten bands by default (high-pass, low shelf, peaking
bands, high shelf and low-pass), each with frequency, gain and Q. Drag band
handles on the response curve, or select a band and type values.

In AetherRX, the **OUT** strip below the EQ curve displays the post-EQ level,
with its numeric readout at the end of the strip.

Toolbar controls:

<img src="/img/screens/aethertx-eq.png" width="720" alt="AetherTX window with EQ selected in the stage column. Ref:, Smoothing, Peak Hold and Reset controls run along the top, above a row of filter-shape buttons. The main area plots the live microphone spectrum with ten band handles on a flat line, over a frequency scale from 20 Hz to 20 kHz with the E-SSB, SSB and AM / FM ranges marked, and the band frequencies and gains listed beneath. An output level meter runs along the bottom." />

*AetherTX with the EQ stage selected.*

- **Ref:** overlays an amber reference target curve: Off, AT&T 1959, Heil DX,
  Astatic D-104, Shure 444 or Heil HC-5. It is a visual guide only; it does
  not change the audio.
- **Smoothing:** fractional-octave smoothing of the analyzer trace (display
  only).
- **Peak Hold:** freezes the analyzer's peak trace.
- Filter family for the high-pass and low-pass slopes: **Butterworth**,
  **Chebyshev**, **Bessel** or **Elliptic**.
- **Reset** returns every band to its default.

When AetherTX's EQ is in use, leave the radio's own TX EQ flat.

**Example: male voice, dynamic mic.** High-pass at 80 Hz to remove handling
noise; −3 dB around 200 Hz to cut mud; −2 dB around 500 Hz to scoop
boxiness; +3 dB at 2 kHz and +2 dB at 3.5 kHz for presence; low-pass at your
SSB filter width (about 3 kHz). For a bright condenser mic, do the opposite:
a little low-mid warmth, pull back 2–4 kHz, roll off above 4 kHz.

### LVLR (AetherTX)

The transmit leveler from WDSP. It rides your level slowly, the way you would
on a fader: when you lean back from the mic or speak softly it brings you up,
by at most **Max gain**, so the fast stages after it see a steady level. Loud
speech is brought toward full scale rather than raised further. LVLR ships
**off**.

The page traces the gain LVLR applied over the last five seconds, against the
**Max gain** ceiling (dashed).

| Knob | Range | What it does |
|---|---|---|
| **Max gain** | 0 to 20 dB | The most LVLR will raise you. Default 5 dB |
| **Attack** | 1 to 10 ms | How quickly it pulls the gain back on a louder sound. It is also a look-ahead, so it adds delay (below). Default 1 ms |
| **Decay** | 1 to 5000 ms | How quickly the gain climbs back after you get louder and then softer. Default 500 ms |
| **Hang** | Off, or 10 to 5000 ms | Holds the gain steady this long before it starts to climb, so pauses between words don't pump. Default Off |

**Delay.** LVLR adds about 7 ms at 1 ms attack, rising to about 61 ms at 10 ms.

**Example: a distant or soft talker.** Max gain 12 dB, Attack 1 ms, Decay 500 ms,
Hang 300 ms. Record a phrase leaning back from the mic and again close in; the
two should come out at nearly the same level.

**Pitfalls:**
- LVLR raises room noise in your pauses as well as your voice. Use the Gate
  ahead of it, and Hang to stop the gain climbing between words.
- LVLR's target sits just above full scale, so keep the Compressor's
  **Limiter** or the Final Output **LIM** on after it.

### CFC (AetherTX)

The Continuous Frequency Compressor from WDSP, the DSP library behind
OpenHPSDR radios. Instead of splitting your voice into a few fixed bands, it
compresses every frequency by its own amount, set by a curve: you can drive the
low end harder without the whole voice ducking, or add presence-band density
without making the lows boom. A second curve, the post-EQ, reshapes the result.
CFC ships **off**.

The page shows both curves over frequency: **compression** (filled points,
0 to 20 dB) and **post-EQ** (hollow points, −20 to +20 dB, dashed while Post-EQ is
off). While you talk, the compression actually being applied is shaded below
the 0 dB line.

- **Drag a point** to move its band in frequency, and that curve up or down.
  A band can't pass its neighbours.
- **Double-click** empty space to add a band (up to ten); **right-click** a point
  and choose **Remove band** to remove one (at least two stay).
- The **Freq**, **Comp** and **EQ** fields under the curves type the selected
  band's values exactly.

How compression works here: a band's level is gain applied before a ceiling.
Quiet sound at that frequency comes up by the full amount; loud sound is held
at the ceiling, so the louder the input the less it is raised. Between bands,
the curves are straight lines in frequency.

| Control | Range | What it does |
|---|---|---|
| **Comp** (per band) | 0 to 20 dB | Compression at that frequency |
| **EQ** (per band) | −20 to +20 dB | Post-EQ at that frequency, applied after compression when **Post-EQ** is on |
| **Freq** (per band) | 10 to 9990 Hz | Where the band sits |
| **Pre-comp** | 0 to 20 dB | Compression added at every frequency alike |
| **Post-EQ** | On / Off | Applies the post-EQ curve and **Post** gain. Default off |
| **Post** | −20 to +20 dB | Gain after the post-EQ. You can set it any time; it takes effect when Post-EQ is on |
| **FFT size** | 1024 to 16384 | Frequency resolution, and the delay CFC adds (below) |

**FFT size and delay.** CFC analyses your voice in blocks, and every block costs
delay. The slider shows the delay it adds:

| FFT size | Adds | Resolves |
|---|---|---|
| 1024 (default) | 21 ms | 47 Hz |
| 2048 | 43 ms | 23 Hz |
| 4096 | 85 ms | 12 Hz |
| 8192 | 171 ms | 6 Hz |
| 16384 | 341 ms | 3 Hz |

That delay sits between your voice and your RF, so at large sizes you hear it
on monitor playback and feel it on quick turnarounds. 1024 resolves voice bands well;
go larger only for very narrow, low-frequency shaping. Changing the size while
you talk crossfades to the new size with no gap.

The first time you turn CFC on, it starts from WDSP's own transmit profile:
compression 0, 5, 10, 10 and 5 dB at 200 Hz, 1 kHz, 2 kHz, 3 kHz and 4 kHz, with
a +7 dB post-EQ (Post-EQ itself off).

**Example: more density without boom.** Pre-comp 0. Compression 2 dB at 200 Hz,
6 dB at 1 kHz, 10 dB at 2 kHz and 3 kHz, 6 dB at 4 kHz. Leave Post-EQ off. Record
a phrase with **REC** and compare against **BYPASS**. If the voice gets gritty,
lower the 2–3 kHz points before touching anything else.

**Pitfalls:**
- CFC raises quiet sound, so room noise between words rises with it. Turn on
  RN2 (on the Tube page), and keep the Compressor after it gentle.
- CFC's ceiling holds each frequency, not the waveform's peaks: with heavy
  Pre-comp the output can exceed full scale. Keep the Compressor's
  **Limiter** or the Final Output **LIM** on after it.
- CFC evens out your voice's spectrum and density; on its own it adds little
  average power at the same peak (measured, it costs up to 0.7 dB). For more
  average power, use **Peak control**.

### Compressor

Levels the signal: brings quiet syllables up and holds loud ones back.

<img src="/img/screens/aethertx-compressor.png" width="720" alt="AetherTX window with Compressor selected in the stage column. A Limiter button and a Ceiling slider sit at the top. The compressor's transfer curve fills the center, between an input meter with a threshold marker on the left and gain-reduction and output meters on the right. Six knobs along the bottom read Ratio, Attack, Release, Knee, Drive and Phase." />

*AetherTX with the Compressor stage selected.*

- **Threshold:** the vertical fader on the left, which doubles as the input
  meter, so you can set the threshold against your own voice peaks.
- **Makeup:** drag the handle on the **Out** meter (−12 to +24 dB). It can
  also be adjusted from the keyboard.
- **Limiter / Ceiling:** the compressor's own brickwall peak limiter and its
  ceiling (−24 to 0 dB, default −1 dB). The **Limiter** button glows red while
  it is clamping. This is separate from the **LIM** limiter on Final Output.

| Knob | Range | What it does |
|---|---|---|
| **Ratio** | 1:1 to 20:1 | How hard to compress above the threshold |
| **Attack** | 0.1 to 300 ms | Transient response |
| **Release** | 5 to 2000 ms | Recovery speed |
| **Knee** | 0 to 24 dB | Soft-knee width (0 = hard knee) |
| **Drive** | 0 to +18 dB | AetherTX only. Gain before the threshold so the compressor works harder; makeup follows it, so average power rises with the peaks. Default 0 |
| **Phase** | Off, 1–6 stages | AetherTX only. All-pass phase rotator that evens out lopsided voice peaks before compression, lowering peak-to-average ratio. 4 is the broadcast default |

Drive and Phase together are the PAPR controls: they raise your average
transmitted power without pushing peaks past the ceiling. They are hidden on
receive.

**Example: broadcast-style SSB.** Threshold about −20 dB, Ratio 4:1, Attack
20 ms (keeps consonants crisp), Release 150 ms, Knee 6 dB, makeup until peaks
sit around −3 dB on the Out meter, Limiter on with the ceiling at −1 dB. Aim
for 3–6 dB of gain reduction on peaks; more than 10 dB is obvious compression
that suits contests better than rag-chews.

### Tube

Analog-style saturation for warmth and harmonic color: the mic preamp, first
in AetherTX's default chain.

**RN2** (AetherTX only): the RNNoise microphone denoiser, at the left of the
Tube page's toolbar. It runs ahead of the tube and every other stage, so noise
is removed before anything can amplify it, and it replaces the gate AetherTX
used to have. Voice modes only, saved in the profile, and switched off by
BYPASS.

**Model:** **A** (symmetric, neutral warmth), **B** (hard clip into soft
saturation, edgier) or **C** (asymmetric, even harmonics, warmest).

| Control | Range | What it does |
|---|---|---|
| **Drive** | 0 to 24 dB | How hard the signal hits the saturator |
| **Tone** | −1 to +1 | Tilt filter: negative darkens, positive brightens |
| **Bias** | 0 to 100 % | Asymmetry of the curve (more even harmonics) |
| **Envelope** | −100 to +100 % | Dynamic drive: negative eases drive on peaks, positive adds to it |
| **Release** | 10 to 500 ms | Envelope-follower release |
| **Output** | −24 to +12 dB | Output gain |
| **Dry/Wet** | 0 to 100 % | Blend of dry and saturated signal (parallel saturation) |

**Example: subtle warmth.** Model A, Drive 6 dB, Tone +0.2, Bias 15 %,
Dry/Wet 40 %, Output about −1 dB to match unity gain.

### Exciter (AetherVoice™)

Adds sparkle at the top and body at the bottom. **Even** generates even
harmonics with asymmetric shaping (warmer); **Odd** generates odd harmonics
with symmetric shaping (edgier, cuts through noise).

| Section | Knob | Range | What it does |
|---|---|---|---|
| Low | **Drive** | 0 to 24 dB | Low-frequency enhancement drive |
| Low | **Tune** | 50 to 160 Hz | Low-frequency corner |
| Low | **Mix** | 0 to 100 % | How much low-frequency enhancement is added |
| High | **Tune** | 1 to 10 kHz | High-frequency corner |
| High | **Air** | 0 to 24 dB | Amount of high-frequency harmonic generation |
| High | **Mix** | 0 to 100 % | How much high-frequency enhancement is added |

The AetherVoice wordmark glows with the wet signal level, confirming the
exciter is actually adding energy.

**Pitfall:** stacking the exciter on top of a bright EQ shelf and heavy tube
drive gives a harsh, hyped sound. Pick one place to add brightness.

### Reverb (AetherTX)

Freeverb room reverb. Reverb trades intelligibility for size, so it ships
**off** and suits rag-chew or AM more than DX or contesting.

| Knob | Range | What it does |
|---|---|---|
| **Size** | 0 to 100 % | Room size |
| **Decay** | 0.3 to 5 s | Tail length |
| **Damp** | 0 to 100 % | High-frequency absorption in the tail (higher = warmer) |
| **PreDly** | 0 to 100 ms | Gap before the tail starts |
| **Mix** | 0 to 100 % | Dry/wet blend |

**Example: subtle broadcast space.** Size 35 %, Decay 0.8 s, Damp 70 %,
PreDly 15 ms, Mix 8 %. Keep the reverb last in the chain; a compressor after
it squashes the tail. Pre-delay adds directly to the delay of the wet signal,
which you will notice on quick overs.

### MB limiter (AetherTX)

A four-band limiter with 5 ms of look-ahead. The voice is split into four
bands at **X1**, **X2** and **X3** (Linkwitz-Riley crossovers that sum flat
when nothing is limited), each band is held under its own **Threshold**, and
the bands are added back together. Because it looks ahead, a band never passes
a peak above its threshold, even on a sudden onset. It always runs after the
chain and just ahead of Peak control, and ships **off**; tick its tab to turn
it on.

Each band's column shows its gain reduction.

| Control | Range | What it does |
|---|---|---|
| **X1 / X2 / X3** | 80–500, 500–2000, 2000–6000 Hz | The crossovers, kept at least 1.5× apart. Defaults 250, 1000, 3000 Hz |
| **Threshold** (per band) | −24 to 0 dBFS | The band's peak ceiling. Default −6 dBFS |
| **Release** (per band) | 10 to 1000 ms | How fast the band recovers after a peak. Default 100 ms |

**Delay.** The MB limiter adds 5 ms.

**What it is for.** Tonal control under load: keeping a boomy low end or
sibilant highs from taking over the clipper. It does not raise talk power; that
is Peak control's **Clip**. Measured on a multitone ahead of Peak control, the
limiter adds its own distortion at the same output level, so set thresholds
only as low as the problem band needs.

**Example: a boomy dynamic mic.** Low band Threshold −12 dBFS, Release 200 ms;
the other bands at 0 dBFS so they pass untouched.

### Peak control (AetherTX)

WDSP's SSB speech processor: a bandpass, an envelope clipper, a second
bandpass, CESSB (controlled-envelope SSB) overshoot control, a last bandpass
and WDSP's transmit ALC as a look-ahead backstop. It holds the envelope of the
SSB signal your radio builds from this audio at a ceiling. That is not the same as limiting the audio's sample
peaks: band-limited audio can have an envelope several dB above its highest
sample, which is why a limiter alone cannot cap the peak RF power. With the
envelope held, you can run more average power at the same peak. Peak control
always runs after the chain, and it ships **off**; tick its tab to turn it on.

The page traces the envelope peak over the last five seconds against the
**Ceiling** (dashed).

| Control | Range | What it does |
|---|---|---|
| **Clip** | 0 to 20 dB | How hard the envelope is driven into the clipper. More clip, more average power and more distortion. Default 6 dB |
| **CESSB** | On / off | Removes the overshoot the bandpass after the clipper puts back. Without it the ALC backstop still holds the ceiling, but by turning you down, which costs up to about 1 dB of average power. Default on |
| **Ceiling** | −12 to 0 dBFS | The envelope level it holds to. Default −1 dBFS |

**Passband.** The stage's bandpass follows your radio's TX filter, 50 Hz inside
each edge, so the radio's filter does not reshape the envelope after it. The
readout under the knobs shows the passband in force. Changing the TX filter
while you talk is seamless. ESSB filters are followed up to the radio's
10 kHz maximum. The passband is never narrower than 500 Hz, so with a TX
filter narrower than 600 Hz it no longer matches the radio's.

**Delay.** Peak control adds about 39 ms.

**On a Hermes-Lite 2: run it in the modulator.** With an HL2 connected, the
page offers **Run in: AetherTX chain / Radio modulator**. In the modulator,
WDSP's own clipper and CESSB run inside the HL2's transmit channel, where they
see the final envelope, and the stage in the AetherTX chain stands aside: the
two never run at once. **Clip** and **CESSB** apply as usual; there is no
**Ceiling**, because the HL2's ALC and drive set the peak. While it is on, the
HL2's SSB TX filter runs linear phase instead of minimum phase, which adds
about 19 ms. Measured with the Contest profile through the HL2 modulator
(300–2700 Hz filter): the peak-to-average ratio is 5.2 dB with no peak
control, 4.9 dB with it in the chain (the HL2's minimum-phase filter undoes
most of it), and 2.5 dB with it in the modulator, about 2.7 dB more talk
power. Off an HL2 the choice is not shown, and a saved **Radio modulator**
choice runs in the chain.

**Example: SSB contest or DX punch.** Clip 6 dB, CESSB on, Ceiling −1 dBFS. On
a FLEX-8600 at 25 W into a dummy load, with the Contest profile ahead of it
and the passband following the radio's 50–4000 Hz TX filter, this lowered the
peak-to-average ratio from about 6.1 dB to about 3.3 dB and raised average
power by about 3 dB at the same peak.

**Pitfalls:**
- Clip is distortion by design. Beyond about 10 dB, voices get rough. Record
  and listen with **REC / PLAY** before you go on the air.
- Set the radio's drive so the envelope peaks reach full power, not beyond.
  Peak control bounds the envelope it sends; the radio's own ALC still sees
  anything you add after it.
- Final Output comes after the ceiling. **Trim** above 0 dB raises the
  envelope past it, and the **LIM** ceiling, if set lower, cuts into it.

### Final Output (AetherTX)

The last stage before the radio.

| Control | What it does |
|---|---|
| **LIM** | Final brickwall limiter at the end of the TX chain: no 48 kHz sample leaves the limiter above the ceiling. **On by default**, ceiling −0.5 dB |
| **DC** | 25 Hz high-pass that strips DC offset |
| **TONE** | 1 kHz test tone injected at the head of the chain; right-click for frequency and level |
| **QUIN** | Quindar tones on PTT engage and release; right-click for the editor (below) |
| **Trim** | Output gain, −12 to +12 dB |
| **Look-ahead** | 0 to 5 ms (default 1.5 ms). How far ahead LIM sees a peak coming, so it ramps down in time instead of clamping. It is also the delay LIM adds while it is on; 0 ms is an instant hard limit |
| **HOLD** | When lit, the **PK / RMS / GR / CRST** readouts latch the worst value since you engaged it. Click again to release |

The horizontal meter shows the input peak, the output peak, gain reduction and
the limiter ceiling as a draggable handle (−12 to 0 dB, default −0.5 dB).
**OVR** latches red on a clip and clears itself; **LIMIT** lights while the
limiter is clamping. The waveform scope below the meter shows what you are
sending.

LIM holds its own 48 kHz output samples under the ceiling. The conversion to
the radio's 24 kHz rate after it can overshoot that slightly between samples,
and LIM cannot hold the SSB RF envelope, which can rise a few dB above the
highest audio sample. For that, use **Peak control**.

#### Quindar tones

NASA's Apollo-era voice-loop chirp: a 2525 Hz tone when you key and 2475 Hz
when you unkey. Right-click **QUIN** to set:

- **Style:** Tone (the classic chirp) or Morse (sends "K" and "BK").
- **Level:** −20 to 0 dBFS.
- **Tone:** intro and outro frequency (2400–2700 Hz) and duration
  (100–500 ms).
- **Morse:** speed (20–60 WPM) and pitch (400–1200 Hz).
- **Test intro / Test outro** audition the tone locally without keying.

### Final Output (AetherRX)

Output **Trim** (−12 to +12 dB), **MUTE** and **BOOST**, with a level meter
and a waveform scope, so you can match the processed receive path against the
dry one.

### Starting points by mic and voice

Tune from here; don't just load and transmit. Record yourself with REC/PLAY
after every change.

#### Dynamic broadcast mic (e.g. SM7B), male voice

| Stage | Setting |
|---|---|
| RN2 | On if the room is noisy |
| EQ | HPF 80, −3 @ 200, +3 @ 2k, +1 @ 3.5k, LPF 3.1k |
| Compressor | Ratio 4:1, Attack 25, Release 150, makeup +5, Limiter on at −1 |
| Tube | Model A, Drive 6, Tone +0.2, Dry/Wet 40 % |
| Exciter | Even, low Mix 20 %, high Air 6, high Mix 20 % |
| Reverb | Off, or Size 30 %, Decay 0.7 s, Damp 70 %, PreDly 15 ms, Mix 6 % |

#### Condenser mic, female voice

| Stage | Setting |
|---|---|
| RN2 | On if the room is noisy |
| EQ | HPF 100, −2 @ 400, −2 @ 3k, +2 @ 5k, LPF 3.1k |
| Compressor | Ratio 3:1, Attack 20, Release 120, makeup +4 |
| Tube | Model C, Drive 9, Bias 20 %, Dry/Wet 45 % |
| Exciter | Even, low Mix 15 %, high Mix 10 % |
| Reverb | Off |

#### Contest / DX punch

Load the **DX Punch 2.7 kHz** stock preset (see
[Stock presets](#stock-presets-aethertx)) and save your own copy to adjust.

## Known issues

- With the default AetherTX chain, transmit output can be well below the radio's rated power, and reaching full PEP needs a high **Trim** ([#2876](https://github.com/aethersdr/AetherSDR/issues/2876)).
- Turning on **RN2** in AetherTX can sharply reduce your transmit audio level and RF output ([#4859](https://github.com/aethersdr/AetherSDR/issues/4859)).
- With **Mic source** set to **PC**, transmit audio can be silent after startup until you switch the source to **BAL** and back to **PC** ([#5442](https://github.com/aethersdr/AetherSDR/issues/5442)).

## Troubleshooting

### You changed a knob and nothing happened

The stage is switched off, the chain is bypassed, or (on AetherTX) the chain
is not in the transmit path.

1. Check that the stage's checkbox is ticked in the tab column.
2. Check that **BYPASS** is not lit.
3. On AetherTX, check that **MIC** is lit. If it is not, fix the audio path as
   described in [Requirements](#requirements).

### AetherTX won't open

The audio path is wrong. The message names the fix.

1. On a FlexRadio, set **Microphone source** to **PC** in the **P/CW** applet.
2. On a radio that takes its transmit audio over the network, turn on
   **PC Audio** in the title bar.
3. If the message says no audio input device was found, connect a
   microphone to this computer.

### The monitor plays silence

**REC** only captures when the PC microphone is selected and DAX is off.

1. Set **Microphone source** to **PC**.
2. Switch DAX off.
3. Check that **MIC** is lit, then press **REC** again.

### Your audio sounds worse, not better

You are probably over-processing.

1. Press **BYPASS** and listen to the dry signal.
2. If the dry signal is fine, switch stages off.
3. Start again with EQ and gentle compression, and add stages one at a time,
   recording with **REC / PLAY** after each change.

### Reordering the chain did nothing audible

Order matters most between dynamics stages: compressor before tube
compresses the dry signal, compressor after tube compresses the saturation
harmonics, and EQ before or after the tube changes what it saturates.

1. Move the compressor, tube or EQ relative to each other.
2. Record with **REC / PLAY** before and after the change, and compare.

### BYPASS didn't restore your previous state

BYPASS restores the stages that were on when you pressed it. A stage you
switched by hand while bypassed counts as a manual change.

1. Release **BYPASS**.
2. Switch the stages you want back on by hand.

## See also

- [TX Controls](./tx-controls.md) — the TX, Phone and P/CW applets around the chain.
- [RX Controls](./rx-controls.md) — receive controls, squelch and AGC.
- [DSP Noise Mitigation](./dsp-noise-mitigation.md) — the noise-reduction methods on AetherRX's
  AetherNR tab.
- [Audio Settings](./audio-settings.md) — PC audio devices and recording.
- [Settings and Backups](./settings-and-backups.md) — where profiles and settings are stored.
