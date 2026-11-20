# FM squelch: width compensation and gate readback

Decided on RFC #6552 (options A and C). Applies to every receive channel whose FM
squelch is WDSP's `fmsq` through `WdspChannel::setSquelch()`. Today that is the
Hermes-Lite 2. The ANAN backend wires no squelch, and RTL-SDR uses its own
`RtlSquelchGate`, so neither is affected.

## What `fmsq` gates on

`fmsq` is a noise squelch, not a level gate. It averages the demodulator's output
above about 5 kHz over 1 ms (`avnoise`). The gate closes when that average is above
`tail_thresh` for the length of the tail, and opens when it falls below
`unmute_thresh = 0.9 · tail_thresh` (`third_party/wdsp/upstream/fmsq.c`). A
carrier that quiets the discriminator opens it.

The slider level L (0..100) maps to `tail_thresh = 10^(−2·L_eff/100)`, where
`L_eff` is L plus a width offset (below). Level 0 does not run the stage.

## A: width compensation

With noise alone at the input, `avnoise` grows as the receive filter narrows, so
without compensation one slider position closes on noise at ±8000 Hz and stays open
on noise at ±2500 Hz. The offset aims to move the level so that noise alone reaches its
50 % duty point (closed half the time) at the same slider position at every width.
It narrows the spread rather than removing it: `wdsp_channel_test` pins that at
level 15 the gate is open in at most half the blocks at every width from ±1500 to
±6000 Hz, and a narrower filter still needs a few more levels than a wide one.

`W` is the filter half-width, `(high − low) / 2`. With `u = log10(W / 3000 Hz)`, the
uncompensated level at which noise alone has 50 % duty is

    N(W) = 22.59 − 98.64·u + 75.02·u²

fitted offline over 1500..6000 Hz to the median of 8 noise seeds per width (max
residual 1.1 levels, rms 0.6). That residual is the fit against those medians, not
the alignment after compensation. Duty there is the full HL2 receive chain, 20 s of
noise per level after a 2 s settle; a 2 ms window counts as not muted when its
audio is at least 1 % (−40 dB) of the same noise with the squelch off. The offset is `N(W) − N(6000)`, so it is 0 at
±6000 Hz and grows as the filter narrows: +4.7 at ±5000, +31.2 at ±2500, +59.4 at
±1500. `L_eff = L + offset` is not clamped to 100, so the slider keeps its full
travel above the noise point at a narrow filter.

**Outside the fitted range the half-width is held at the nearer end, not
extrapolated.**

- **Above ±6000 Hz** (the HL2's FM default is ±8000) the offset is 0, which is the
  map without compensation. There, noise alone already holds the gate closed from
  level 1 (offline, 8 of 8 seeds at ±7000 and ±8000).
- **Below ±1500 Hz** the offset stays at +59.4. Noise alone then reaches 50 % duty
  above the common position, about 30 levels higher at ±1000 Hz (offline).

Constants: `WdspChannel::kFmSquelchFitMinHalfWidthHz`,
`kFmSquelchFitMaxHalfWidthHz`, `fmSquelchNoiseLevelForHalfWidth()`,
`fmSquelchWidthOffsetLevels()`, `fmSquelchThresholdFor()`. The threshold is
re-applied on every filter change in FM, on a mode change and on a channel rebuild.

The fit was made at the RX detector's 5000 Hz deviation (`Config::fmDeviationHz`),
which scales `fmsq`'s input (`fmd.c`: `audio = again · …`, `again = rate /
(deviation · 2π)`). The HL2 receive chain never changes it. A backend that does
would move the noise point down by `50 · log10(5000 / deviation)` levels (inferred
from the source, not measured). The FM limiter
(`SetRXAFMLimRun`) acts on `fmd`'s output after the de-emphasis and audio filters,
not on the `audio` buffer `fmsq` reads, so it does not change the gate.

**What it does not address.**

- **Modulation depth.** A modulated signal raises `avnoise` too, and at a filter
  that is narrow for the deviation it closes the gate at lower levels than an
  unmodulated carrier of the same strength. The compensation is fitted to noise
  alone and does not correct this.
- **The chatter band.** Between the level where noise alone always opens and the
  level where it always closes, the gate chatters. That band is wider at narrow
  filters (about 6 levels at ±8000, 14 at ±2500, 29 at ±1500, offline). One offset
  per width aligns the 50 % point only, not both edges.

## C: the gate on the slider

WDSP patch 24 (`third_party/wdsp/AETHERSDR-PATCHES.md`) publishes `fmsq`'s gate
once per block. `WdspChannel::fmSquelchGateOpen()` reads it while the stage runs.
The HL2 receive DSP passes each block's reading through `SquelchGateReporter`,
which publishes a change at most every 100 ms of processed audio, and the backend
sends it as `SliceDelta::squelchGate`. The backend lists the modes it reports in
`RadioCapabilities::squelchGateModes`. In those modes, with SQL not Off, the RX
applet's SQL slider fill dims while the gate is closed, and its tooltip and
accessible description say whether the gate is open or closed.
