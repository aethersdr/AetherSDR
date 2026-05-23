# Understanding Noise Cancellation

## What Is Noise Cancellation and Why Does It Matter?

If you have ever tuned across the HF bands on SSB and heard a constant hiss,
buzz, or crackling underneath the voice you are trying to hear, you have met
radio noise. Some of it comes from your own house — power supplies, LED lights,
appliances. Some of it travels thousands of miles along with the signal you
want. Noise cancellation (often called "noise reduction" or NR) is software
that listens to the audio coming out of the radio, figures out which parts are
noise and which parts are the voice you care about, and turns down the noise
while keeping the voice intact.

Think of it like noise-cancelling headphones, but instead of blocking airplane
cabin rumble, it is cleaning up a weak signal from the other side of the world.

You do not need to understand the math behind any of this. The short version:
turn on one of the NR options in MasterSDR, adjust the strength until the
background noise drops without making the voice sound robotic, and enjoy
cleaner audio. The sections below explain what each option does and when to
reach for it.

---

## How Noise Reduction Has Evolved

### The Early Days — Classical Filtering

The first noise reduction systems in amateur radio appeared in the 1980s and
worked with simple analog filters. When DSP (Digital Signal Processing) radios
arrived in the 1990s, manufacturers built noise reduction directly into the
radio hardware. These early DSP methods — sometimes called "NR" or "NR1" —
used techniques called spectral subtraction and autoregressive modeling. In
plain language, the radio looked at the frequency pattern of the incoming audio,
estimated which frequencies were "just noise," and turned those frequencies
down.

These classical methods work, but they have limitations. They can create
artifacts — short chirps or a watery, "musical" sound — especially when the
noise changes quickly. They also struggle with noise that sits right on top of
the voice frequencies.

### The Statistical Era — Smarter Math

Starting in the late 1990s and into the 2000s, researchers developed more
sophisticated approaches. Instead of simply subtracting noise, these methods
use probability and statistics to estimate what the clean speech "should"
sound like. Algorithms like the Ephraim-Malah estimator (the foundation of
what MasterSDR calls NR2) track the noise floor in real time, adapt to
changing conditions, and apply just enough suppression to reduce noise without
damaging the voice. The result is cleaner audio with fewer artifacts, though
you still have knobs to turn if the conditions are unusual.

### The AI Revolution — Neural Networks Enter the Shack

Around 2018, a breakthrough arrived: neural networks trained on thousands of
hours of noisy and clean speech. Instead of following hand-written rules about
"what noise looks like," these networks learned on their own. Mozilla's
RNNoise project (2018) was one of the first open-source examples, and it
works remarkably well with zero tuning required.

Since then, AI-based noise reduction has advanced rapidly. DeepFilterNet
(2022) uses deep learning with a specialized filter structure that preserves
voice quality even in very noisy environments. NVIDIA's Maxine platform brings
GPU-accelerated AI denoising that can run in real time alongside your radio
software.

The trend is clear: each generation gets better at separating voice from noise,
needs less manual tuning, and handles a wider range of noise types. MasterSDR
gives you access to options from every era so you can pick what works best for
your station and your computer.

---

## Noise Cancellation Options at a Glance

Your FlexRadio and MasterSDR each provide their own noise reduction tools.
They run in different places — the radio-side options run inside your
FlexRadio's own DSP hardware, while MasterSDR's options run on your computer
after the audio arrives from the radio. Understanding which is which will help
you get the most out of both.

You can use one radio-side option and one MasterSDR option at the same time —
they stack. Many operators use light radio-side NR plus one of MasterSDR's
modes for the best result.

### Radio-Side (FlexRadio Hardware)

These are built into your FlexRadio and processed on the radio itself.
MasterSDR gives you access to them through the DSP buttons on the VFO bar
and the spectrum overlay panel, but the processing happens on the radio —
not on your PC.

| Mode | Full Name | Tunables | Best For |
|------|-----------|----------|----------|
| NB | Noise Blanker | NB Level slider | Impulse noise — clicks, pops, ignition noise, lightning crashes |
| NR | Noise Reduction | NR Level slider | General-purpose noise reduction for SSB and CW |
| ANF | Automatic Notch Filter | ANF Level slider | Removing carriers, heterodynes, and steady tones |

If you are wondering "where are NB, NR, and ANF?" — they are right here,
controlled through MasterSDR but running on your FlexRadio hardware.

### Client-Side (MasterSDR Software)

These are unique to MasterSDR and run on your computer's CPU (or GPU in the
case of BNR). They process the audio after it leaves the radio, giving you an
additional layer of noise reduction beyond what the radio provides. Only one
client-side mode can be active at a time.

| Mode | Full Name | Type | Tunables | CPU/GPU | Best For |
|------|-----------|------|----------|---------|----------|
| NR2 | Spectral NR | Statistical DSP | 6 parameters | CPU | Fine-tuned control over SSB noise |
| RN2 | RNNoise | Neural network | None | CPU | Quick cleanup, no fuss |
| NR4 | SpecBleach | Advanced spectral | 7 parameters | CPU | Stubborn broadband noise |
| MNR | Apple MMSE-Wiener | Statistical DSP | 1 slider | CPU (macOS only) | macOS users wanting a native option |
| BNR | NVIDIA Maxine | AI (cloud/GPU) | 1 slider | RTX GPU | Maximum quality with NVIDIA hardware |
| DFNR | DeepFilterNet3 | AI (local) | 2 parameters | CPU | Best all-around AI denoising |

**Note:** Not every client-side mode may be visible on your system. Some
options (such as BNR or DFNR) require additional hardware (an NVIDIA RTX GPU
for BNR) or optional libraries that may not be compiled into your build. If
you do not see a particular button in the VFO bar or DSP panel, it is not
available in your installation — this is normal and nothing is broken.

### A Note About CW and Digital Modes

MasterSDR's client-side noise reduction modes are designed for voice (SSB)
operation. They work by identifying and preserving human speech while removing
everything else — which is exactly the wrong thing to do when the signal you
want is a CW tone or a digital mode like FT8, JS8Call, or RTTY.

For this reason, the client-side modes (NR2, RN2, NR4, MNR, BNR, and DFNR)
automatically disable themselves when you switch to CW, CWL, or a digital
mode. This is intentional — do not try to force them on. Running voice-optimized
noise reduction on non-voice signals can distort the audio, confuse decoding
software, and waste CPU cycles for no benefit.

If you need noise help in CW mode, your radio's built-in NR and ANF are
better choices — they operate on the raw signal before mode-specific
processing and are designed to work across all modes.

---

## A Closer Look at Each Mode

### NR2 — Spectral Noise Reduction

NR2 is based on the Ephraim-Malah Minimum Mean Square Error Log-Spectral
Amplitude (MMSE-LSA) estimator, a well-regarded algorithm originally published
by Yariv Ephraim and David Malah in 1985. The implementation in MasterSDR is
derived from the open-source WDSP library written by Warren Pratt, NR0V, which
has been a staple in FlexRadio and other SDR platforms for over a decade.

NR2 excels at steady-state noise — the kind of constant hiss or hum that
does not change much from moment to moment. It is less effective against
impulsive noise (clicks, pops, lightning crashes) because its noise-floor
tracker needs time to adapt. If you are on a relatively quiet band with a
steady background hiss, NR2 is an excellent choice because you can dial in
exactly the right amount of suppression.

**Available tunables:**
- **Gain Max (Reduction Depth)** — How aggressively noise is suppressed (0.50–2.00). Start around 1.50 and increase if noise persists.
- **Gain Smoothing** — Smooths the suppression over time to reduce "musical" artifacts (0.50–0.98). Higher values sound smoother but react slower.
- **Voice Threshold** — How sensitive the speech detector is (0.05–0.50). Lower values preserve quiet voices but may let more noise through.
- **Gain Method** — The mathematical model used (Linear, Log, Gamma, or Trained). Gamma is the default and works well for most voice signals.
- **Noise Estimation Method** — How the algorithm tracks the noise floor (OSMS, MMSE, or NSTAT). OSMS is the default; try NSTAT if noise changes rapidly.
- **Artifact Elimination Filter** — A post-processing step that smooths out ringing artifacts. Leave this on unless you want maximum aggressiveness.

**Recommendation:** Start with the defaults. If you hear musical warbling, increase Gain Smoothing. If weak voices disappear, lower the Voice Threshold.

---

### RN2 — RNNoise Neural Suppression

RN2 uses Mozilla's RNNoise, an open-source recurrent neural network released
in 2018 by Jean-Marc Valin (the creator of the Opus audio codec and Speex).
The network was trained on thousands of hours of speech mixed with real-world
noise, and it runs so efficiently that it adds virtually no CPU load.

RNNoise is a "set it and forget it" option. Because the neural network learned
what speech sounds like during training, it does not need you to adjust
parameters. It handles a wide variety of noise types — hiss, hum, computer
fan noise, and moderate impulse noise — with good results. Its main limitation
is that it was trained on general-purpose noise rather than HF radio noise
specifically, so it can sometimes struggle with the unique characteristics of
weak, fading shortwave signals. Very weak signals may be suppressed along with
the noise.

**Available tunables:** None — the trained model handles everything
automatically.

**Recommendation:** RN2 is a great starting point if you are new to noise reduction or just want cleaner audio without learning what all the knobs do. If it is clipping weak signals or the audio sounds slightly muffled on very noisy bands, try DFNR or NR2 instead.

---

### MNR — Apple MMSE-Wiener (macOS only)

MNR is a macOS-only spectral noise reducer that uses Apple's vDSP framework
to run an MMSE (Minimum Mean-Square Error) Wiener filter natively on the
operating system's optimised math libraries. It is mathematically related
to NR2 — both descend from the Ephraim-Malah family — but takes advantage
of macOS's hand-tuned implementation so it costs almost nothing on Apple
Silicon.

MNR exists for Mac users who want a no-fuss native option without
installing GPU drivers (BNR) or compiling extra libraries (DFNR / NR4).
It handles general HF SSB noise well; for tougher conditions or weak-DX
work, DFNR remains the best all-around choice on macOS too.

**Available tunables:**
- **Strength** — A single slider (0–100%) controlling how aggressively
  the Wiener filter cuts noise. Default 100%; lower for lighter touch.

**Recommendation:** macOS users start here for the simplest setup; switch
to DFNR for tougher noise.

---

### NR4 — SpecBleach Spectral Denoiser

NR4 is powered by libspecbleach, an open-source spectral noise reduction
library created by Luciano Dato in 2021. It uses advanced spectral masking
techniques that go beyond traditional spectral subtraction, applying
psychoacoustic models to decide which parts of the audio to suppress.

NR4 shines against broadband noise — the kind of steady, wide-frequency
"wall of noise" that comes from switch-mode power supplies, plasma TVs, or
a generally high noise floor. It has a learning period during the first second
of operation where it builds a profile of the noise in your environment, then
applies targeted suppression. This makes it particularly effective when the
noise is consistent, and its seven tunables give you fine-grained control.

**Available tunables:**
- **Reduction Amount** — Maximum suppression depth in dB (0–40 dB). Start at 10 dB and increase as needed.
- **Smoothing Factor** — Stabilizes the noise estimate over time (0–100%). Higher values are steadier but slower to adapt.
- **Whitening Factor** — Flattens the spectral shape of residual noise (0–100%). Useful if remaining noise sounds "colored" or uneven.
- **Noise Estimation Method** — How the noise profile is calculated (SPP-MMSE, Brandt, or Martin). SPP-MMSE is the default and works well in most situations.
- **Adaptive Noise** — Continuously updates the noise profile vs. using the initial measurement. Leave on for changing conditions; turn off if the noise is perfectly steady.
- **Masking Depth** — How deep the spectral mask cuts (0.0–1.0). Higher values suppress more aggressively in masked regions.
- **Suppression Strength** — Overall aggressiveness (0.0–1.0). The master volume knob for noise removal.

**Recommendation:** NR4 works best when you can let it "listen" to pure noise for a moment before a signal appears. If your noise floor is high and constant, NR4 with a moderate Reduction Amount (15–20 dB) often sounds more natural than NR2 at the same suppression level.

---

### BNR — NVIDIA Maxine Background Noise Removal

BNR uses NVIDIA's Maxine AI platform, originally developed for video
conferencing and released as part of the NVIDIA Broadcast suite in 2020.
It runs as a local service on your machine and uses your NVIDIA RTX graphics
card's AI tensor cores to perform real-time noise removal.

BNR delivers some of the highest-quality noise removal available because it
has access to massive GPU computing power. It handles virtually every type of
noise — broadband hiss, impulse noise, hum, and even complex non-stationary
interference — with minimal impact on voice quality. The trade-off is that it
requires an NVIDIA RTX GPU and a running Docker container to operate.

**Available tunables:**
- **Intensity** — A single slider (0–100%) controlling how aggressively noise is removed. 100% is the default and works well for most HF conditions.

**Recommendation:** If you have an NVIDIA RTX GPU, BNR is worth trying for the
best possible noise removal with the simplest controls. It pairs especially
well with weak-signal DX work where every decibel of cleanup matters. If you
do not have an RTX GPU, DFNR provides similar AI-quality results on CPU.

---

### DFNR — DeepFilterNet3 AI Noise Reduction

DFNR uses DeepFilterNet3, an AI speech enhancement model created by Hendrik
Schröter and published in 2022 through his work at the Erlangen-Nürnberg
university in Germany. It was designed specifically for real-time speech
enhancement and uses a deep filtering approach that processes audio in both
the time and frequency domains simultaneously.

DFNR represents the best balance of quality, efficiency, and control among
the AI-based options. It runs entirely on your CPU with only about 10
milliseconds of latency — fast enough that you will never notice the delay.
Because DeepFilterNet was designed for speech enhancement rather than general
denoising, it is particularly good at preserving the natural sound of human
voices while removing background noise. It handles HF radio conditions well,
including fading signals and varying noise floors.

**Available tunables:**
- **Attenuation Limit** — Maximum noise suppression in dB (0–100 dB). A value of 0 means no processing (passthrough). For weak signals, try 20–30 dB. For casual listening, 40–60 dB. For strong signals with heavy noise, 80–100 dB.
- **Post-Filter Beta** — An additional suppression stage after the main AI processing (0.0–0.30). Leave at 0 for the cleanest sound; increase to 0.05–0.15 for a subtle extra cleanup, or 0.15–0.30 if noise persists after the main filter.

**Recommendation:** DFNR is the best all-around choice for most operators. Start with an Attenuation Limit of 40 dB and Post-Filter Beta of 0, then adjust the attenuation up or down based on conditions. It works on any computer — no special GPU needed.

---

## Quick-Start Guide

Not sure where to begin? Here is a simple decision tree:

1. **Just want it to work?** Turn on **DFNR** with default settings. Done.
2. **Have an NVIDIA RTX GPU?** Try **BNR** — one slider, excellent results.
3. **Want zero controls?** Use **RN2** — no knobs at all.
4. **Fighting steady hiss on a quiet band?** Use **NR2** — dial in exactly the suppression you need.
5. **High, constant noise floor (power line noise, RFI)?** Try **NR4** — its noise profiling handles broadband interference well.

Remember, you can combine any one of these with your radio's built-in NR for
even more noise reduction. Start with light settings on both and increase
gradually — over-processing can make voices sound robotic or hollow.

---

## Where to Find the Controls

- **Quick toggle:** The DSP buttons on the VFO bar (NR2, RN2, NR4, MNR, BNR, DFNR)
- **Overlay panel:** Right-click the spectrum display, open the DSP panel
- **Full settings:** Settings menu → MasterDsp Settings (or right-click any DSP applet)
- **Right-click shortcut:** Right-click the NR2 button on the VFO bar for a quick parameter popup

---

## Troubleshooting

**The voice sounds robotic, hollow, or "underwater"**
You are over-processing. Reduce the aggressiveness — lower the Gain Max (NR2),
Reduction Amount (NR4), Attenuation Limit (DFNR), or Intensity (BNR). If you
are stacking radio-side NR with a client-side mode, try turning one of them
down. Less is often more.

**A weak signal disappears when I turn on noise reduction**
The algorithm is treating the weak voice as noise. For NR2, lower the Voice
Threshold so the speech detector is more sensitive. For DFNR, reduce the
Attenuation Limit to 20–30 dB so it applies lighter suppression. RN2 has no
tunables for this — switch to DFNR or NR2 where you have more control over
weak signals.

**My NR button is grayed out or turned itself off**
This is normal. Most client-side modes automatically disable when you switch
to CW, CWL, or a digital mode because they are designed for voice signals
only. Switch back to an SSB mode and the button will become active again. See
the "CW and Digital Modes" section above.

**I hear a "musical" warbling or chirping sound**
This is a common artifact of spectral noise reduction. In NR2, increase the
Gain Smoothing parameter to stabilize the suppression over time. In NR4, try
increasing the Smoothing Factor. If the artifacts persist, switch to an
AI-based mode (DFNR or RN2) — neural networks rarely produce this type of
artifact.

**NR4 does not seem to do anything for the first second**
This is by design. NR4 needs a brief learning period (about one second) to
build a profile of the noise in your environment. During this time it passes
audio through unprocessed. After the learning period, suppression kicks in
automatically.

**I do not see BNR or DFNR in my DSP buttons**
These modes require optional components that may not be included in every
build. BNR requires an NVIDIA RTX GPU and a running Maxine Docker container.
DFNR requires the DeepFilterNet3 library to be compiled into your build. If
they are not available on your platform, they will not appear — this is
expected. The other modes (NR2, RN2, NR4) are always available.

**Audio sounds worse with two noise reduction modes stacked**
Stacking radio-side NR with a client-side mode can help, but too much of both
creates diminishing returns and can introduce new artifacts. Start with one
mode at moderate settings. If you want to stack, keep the radio-side NR light
(level 3–4 out of 10) and let the client-side mode do the heavy lifting.
