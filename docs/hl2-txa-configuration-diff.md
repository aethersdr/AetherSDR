# The WDSP TXA configuration diff: `runVector(Transmit)` against the live HL2 path

**Question.** `src/core/backends/hl2/Hl2TxDsp.cpp`'s note says a WDSP TXA channel,
"driven from this backend's configuration", "returned Underrun on most blocks and zeros on
the rest", and names the way back in: *"with `wdsp_channel_test`'s working configuration as
the starting point rather than a guess."* This document is that comparison, call by call,
and the measurement that settles what the Underruns and the zeros were.

**Tree read and measured:** `origin/main` at `85e4a1ff` — the WDSP **2.10** vendor snapshot,
not the 2.00 snapshot the `S6-wdsp-txa-migration` study was written against. Every source
claim below was re-checked against 2.10; where 2.10 differs it is said so.

**What was measured.** Everything in §3 and §4 is a run of
`tests/wdsp_channel_test.cpp`'s `runTransmitLiveGeometryTest` and
`tests/hl2_txdsp_test.cpp`'s DIGU low-edge diagnostic, on this machine, macOS/arm64,
RelWithDebInfo. Both are in the default `ctest` graph. **Nothing here touches hardware and
nothing keys a transmitter** — it is buffers and tests only. What was *not* measured is
listed in §7 and is listed there in full.

> **Status, added when TXA landed.** This document was written while TXA was a
> *candidate*. It is now a BUILD OPTION, `AETHER_HL2_TX_TXA`, and it is
> **ON by default since #5779**: a stock build now opens a TXA channel at the
> geometry derived here and compiles the phasing modulator out, and
> `-DAETHER_HL2_TX_TXA=OFF` is the way back to it. The default WAS OFF because
> #5678's approval asked for hardware time and §7 recorded that there had been
> none; §7 now records the runs that supplied it, **and the limits of what they
> observed** — read that section rather than this sentence before relying on
> the change. The configuration in
> §1 and §2 is what `Hl2TxDsp::buildModulator` and `applyModeAndFilter` now do;
> the measurements in §3 and §4 stand as written. Two details have moved on:
> `hl2_txdsp_test`'s DIGU low-edge block is an **assertion** rather than a
> diagnostic, and both harnesses now retry a starved run, because a machine
> under load starves a `blockForOutput = false` channel and a short capture
> reads as a suppression failure. Left in place rather than rewritten — the
> reasoning is the record.

---

## 1. The two configurations, call by call

### 1.1 `runVector(WdspChannel::Direction::Transmit)` — the working one

`tests/wdsp_channel_test.cpp`, `runVector`. `WdspChannel::Config` fields it sets, and the
defaults it leaves:

| field | value | source |
|---|---|---|
| `direction` | `Transmit` | set |
| `inputBlockSize` | 256 | set |
| `dspBlockSize` | 256 | set |
| `mode` | `Usb` | set |
| `blockForOutput` | **`true`** | set |
| `inputSampleRate` / `dspSampleRate` / `outputSampleRate` | 48000 / 48000 / 48000 | `Config` defaults |
| `filterLowHz` / `filterHighHz` | 150.0 / 3000.0 | `Config` defaults |
| `filterTaps`, `minimumPhase` | 2048, false | `Config` defaults; **never read on a transmit channel** (`WdspChannel::open` calls `RXASetNC`/`RXASetMP` only on the receive branch) |
| `agcMode`, `maximumAgcGainDb`, `agcSlopeDb`, `agcFixedGainDb` | 3, 120, 35, 10 | `Config` defaults; `applyRxAgc` is receive-only |
| `muteDelayUpSec` / `muteSlewUpSec` / `muteDelayDownSec` / `muteSlewDownSec` | 0.010 / 0.025 / 0.000 / 0.010 | `Config` defaults |
| `noiseBlankerEnabled`, `noiseBlankerLevel` | false, 50 | `Config` defaults; `WdspChannel::openNoiseBlanker` returns immediately on a transmit channel |

The WDSP calls `WdspChannel::open()` then makes, in order:

```
loadWisdomOnce()
OpenChannel(id, 256, 256, 48000, 48000, 48000, 1 /*kTxChannelType*/, 0 /*stopped*/,
            0.010, 0.025, 0.000, 0.010, 1 /*bfo*/)
SetTXAMode(id, 1 /*TXA_USB*/)
SetTXABandpassFreqs(id, 150.0, 3000.0)
exportWisdomNow() / armWisdomExportOnce()      // if the planner is unbounded
openNoiseBlanker()                             // returns immediately, receive-only
SetChannelState(id, 1, 0)
```

**That is the entire transmit-side configuration in the tree — two WDSP calls plus the
lifecycle.** Everything else is `create_txa`'s construction defaults, and for SSB they are
already correct: `panel.run = 1` with `inselect = 2`, `bp0.run = 1` at
`max(2048, dsp_size)` taps with `mp = 0`, `alc.run = 1` with `max_gain = 1.0` and
`out_targ = 1.0`, and `gen0`, `gen1`, `phrot`, `amsq`, `eqp`, `preemph`, `leveler`,
`cfcomp`, `compressor`, `bp1`, `bp2`, `osctrl`, `ammod`, `fmmod`, `iqc` and `cfir` all
`run = 0`. Verified in 2.10's `third_party/wdsp/upstream/TXA.c`, `create_txa`. **The "long,
largely undocumented initialisation sequence" the note describes is, for SSB, empty.**

Then per block, 24 times with no pacing: `fexchange2` through
`WdspChannel::processIq`, with `fillAudioTone` writing **the same** 1 kHz value at amplitude
0.1 into `inputI` *and* `inputQ`.

Derived WDSP state (`channel.c` `pre_main_build`, `iobuffs.c` `create_iobuffs`,
`TXA.c` `TXAResCheck`):

- `dsp_insize = 256`, `dsp_outsize = 256`, `out_size = 256`
- `r1_size = 256`, `r2_size = 256`, `r2_havesamps = (DSP_MULT-1) * r2_size = 256`,
  `Sem_OutReady` initial count `n = 1`
- `rsmpin` **off**, `rsmpout` **off** — all three rates equal
- `bp0` nc = `max(2048, 256)` = 2048
- mute ramp `slew.ndelup + slew.ntup = 0.010*48000 + 0.025*48000 = 1680` input samples
  = **35 ms = 6.56 blocks**, and the test's own energy assertion starts at block 8

### 1.2 The live path

`Hl2Backend` opens **zero** transmit WDSP channels today — `beginDspSetup`'s comment says
so. The live configuration below is therefore what the equivalent channel *must* be given,
read off `Hl2TxDsp::Config` as `Hl2Backend::beginDspSetup` fills it, `MetisClient`, and
`AudioEngine`:

| field | value | where it comes from |
|---|---|---|
| `direction` | `Transmit` | — |
| `inputSampleRate` | **24000** | `Hl2TxDsp::Config::inputSampleRateHz`, assigned explicitly in `Hl2Backend::beginDspSetup` ("AudioEngine's rate; `submitTxAudio` re-checks", and `submitTxAudio` refuses anything else) |
| `dspSampleRate` | 48000 | `Hl2RxDsp`'s `kWdspDspSampleRateHz`; WDSP's design rate, and both reference clients hold it there |
| `outputSampleRate` | 48000 | `MetisClient::kEp2AudioRateHz`, fixed |
| `inputBlockSize` | **512** | `Hl2TxDsp::Config::dspBlockSize`, in *input* samples |
| `dspBlockSize` | **1024** | the mirror of `Hl2RxDsp::configure`'s `dspBlockSize * kWdspDspSampleRateHz / inputSampleRateHz`. **Nothing in the tree computes this for transmit today.** |
| `mode` | the TX receiver's mode | `Hl2Backend::beginDspSetup` via `modeFromString`; changes later through `setSliceMode` → `Hl2TxDsp::setMode` |
| `filterLowHz` / `filterHighHz` | `effectiveTxPassband(mode)` → `defaultTxPassbandForMode` | `{300,2700}` voice, `{150,3000}` DIGU/DIGL, `{300,900}` CW, `{100,3000}` AM/FM — **positive for every mode** |
| `blockForOutput` | **`false`** | must be. `Hl2Backend`'s constructor moves both `m_metis` and `m_txDsp` to `m_ioThread`, and `MetisClient`'s 2 ms `m_ep2Timer` and its `QUdpSocket` live on that same thread. `fexchange2`'s `bfo` branch is `WaitForSingleObject(Sem_OutReady, INFINITE)`; an unbounded wait there stops EP2 and the HL2's gateware watchdog is on the other end. `Hl2RxDsp::Config::blockForOutput` is already `false` for the same reason. |

Derived WDSP state at that configuration:

- `dsp_insize = 512`, `dsp_outsize = 1024`, `out_size = 1024`
- `r1_size = 512`, `r2_size = 1024`, `r2_havesamps = 1024`, `Sem_OutReady` initial `n = 1`
- `rsmpin` **ON** (`in_rate != dsp_rate`) — a `calc_resample` polyphase FIR the test never runs
- `rsmpout` off
- `bp0` nc = `max(2048, 1024)` = 2048
- mute ramp `0.010*24000 + 0.025*24000 = 840` input samples = **35 ms = 1.64 blocks**
- **caller cadence**: `AudioEngine::m_txPollTimer` at 5 ms → `Hl2Backend::submitTxAudio` →
  `Qt::QueuedConnection` onto `m_ioThread` → `Hl2TxDsp::processAudioBlock` accumulates into
  `m_inBuffer` and processes whole `dspBlockSize` multiples → **one `processIq` per 512
  input samples = one call per 21.33 ms**

---

## 2. The diff

Every row is a real difference between the two, with its consequence and whether that
consequence was measured or read.

| # | what differs | `runVector` | live | consequence | status |
|---|---|---|---|---|---|
| 1 | **caller pacing** | tight loop, 24 calls back to back | one call per **21.33 ms**, clocked by the audio device | **This is the entire Underrun story.** See §3. | **measured** |
| 2 | `blockForOutput` | `true` | **must be `false`** | With `bfo = 1`, `fexchange2`'s `*error += -2` branch is structurally unreachable (`if (a->bfo) WaitForSingleObject(..., INFINITE); if (a->bfo || doit)`). **The existing TX vector is incapable of reporting an underrun.** Verified unchanged in 2.10. | source |
| 3 | `inputSampleRate` | 48000 | **24000** | `TXAResCheck` turns `rsmpin` on. A stage the test never executes, in the signal path, ahead of `bp0`. Ran clean in every measurement below. | source + **measured to run** |
| 4 | buffer geometry | `dsp_insize == dsp_size == out_size == 256` | `dsp_insize 512`, `dsp_size 1024`, `dsp_outsize 1024`, `out_size 1024` | **Every buffer relationship `pre_main_build` computes differs.** Both geometries run clean once paced. | **measured** |
| 5 | `dspBlockSize` derivation | hand-set equal to `inputBlockSize` | must be `2 × inputBlockSize` | `Hl2RxDsp::configure` computes this for receive; **no transmit counterpart exists**. Getting it wrong is the one configuration error that produces a *permanent* rate imbalance rather than a transient. | source |
| 6 | **which input plane carries the audio** | **both**, identically (`fillAudioTone`) | mono audio — one plane | `xpanel` runs with `inselect = 2`: `I = in[2i] * (inselect >> 1)`, `Q = in[2i+1] * (inselect & 1)`. **Q is multiplied by zero.** A caller that fills `inputQ` and leaves `inputI` empty gets **exact zeros, forever, with no error**. `runVector` fills both, so it cannot distinguish them. | **measured** — §4.1 |
| 7 | passband sign | `{+150, +3000}` | positive for every mode today; **must become signed** | The mode does not select the sideband in TXA — `TXASetupBPFilters` handles `TXA_LSB` and `TXA_USB` with the identical `CalcBandpassFilter(bp0, f_low, f_high, 2.0)` call. The sign does. | **measured** — §4.2 |
| 8 | output conjugation | n/a (output discarded) | `Hl2TxDsp` emits `(bi, -q)` | **The conjugation must be DROPPED, not kept.** See §4.2 — this is the correction that matters most, and it is the opposite of what the S6 study concluded. | **measured** |
| 9 | mute-ramp length in blocks | 1680 input samples = 6.56 blocks | 840 input samples = 1.64 blocks | The test's `block >= 8` energy window clears its ramp by one and a half blocks; the live geometry clears its ramp in two. First non-zero output measured at **block 2**. | **measured** |
| 10 | mode coverage | `Usb` only | any of 13 | `WdspChannel::Mode` is index-identical to `rxaMode` *and* to `txaMode` for 0..11, but **`txaMode` has no `TXA_WBFM`**: 12 is `TXA_AM_LSB` and 13 is `TXA_AM_USB`. `validateConfig`'s refusal of `Mode::Wbfm` on transmit is load-bearing, not cosmetic — without it a WBFM transmit channel would silently become AM-lower-sideband. | source |
| 11 | control calls while running | none | `setMode` / `setFilter` arrive queued, possibly **while keyed** | Both take `beginControlOperation()`, which makes concurrent `processIq` return `ProcessResult::Busy`. `Hl2TxDsp` has no such state today — a mode change mid-over is free. A TXA path drops a transmit block per control call unless the caller re-feeds. | source |
| 12 | consumer | discarded | `MetisClient::queueTxIq`, a deque drained 126 samples per 2.625 ms EP2 frame, `kTxQueueMax = 12000` | Underflow is silence, overflow drops the oldest. Unchanged by the modulator. | source |
| 13 | unkey | none | `Hl2Backend::setKeying(false)` → `Hl2TxDsp::reset()` + `MetisClient::flushTxIq()` | TXA uses `WdspChannel::discardTransmitData()` to synchronously discard rings and filter history. A fade-only `setRunning(false)` requires continued clocking, which HL2 does not supply while receiving. | source + head-inclusive reset regression |

---

## 3. The Underruns

### 3.1 What was run

`runTransmitLiveGeometryTest` opens a transmit `WdspChannel` at the live geometry
(`inputBlockSize 512`, `dspBlockSize 1024`, 24 000 → 48 000 → 48 000, `blockForOutput =
false`, `Mode::Usb`, `{300, 2700}`), feeds a 1 kHz tone at amplitude 0.1 into `inputI` with
`inputQ` zeroed, and censuses `ProcessResult` against one variable: **how fast the caller
calls.**

```
TX underrun census at the live geometry (512 in / 1024 dsp, 24k->48k, bfo=0):
  pace     0 us over 256 blocks: ok=16  underrun=240  settledBlocks=15  maxPhaseSpread=164.0 deg
  pace  1000 us over  64 blocks: ok=64  underrun=0    settledBlocks=60  maxPhaseSpread=7.2e-06 deg
  pace  5000 us over  64 blocks: ok=64  underrun=0    settledBlocks=60  maxPhaseSpread=7.2e-06 deg
  pace 21333 us over  64 blocks: ok=64  underrun=0    settledBlocks=60  maxPhaseSpread=7.2e-06 deg
```

Across five consecutive runs of the same binary the unpaced leg gave `ok` counts of
16, 7, 5, 1 and 9 out of 256, with 0–15 of those blocks carrying any signal. The 5 ms and
21.33 ms legs gave 64 out of 64 every time. The 1 ms leg gave 64/64 three times and 63/64
twice — **it is marginal, and the two 63/64 runs are what produced §3.3's measurement.**

### 3.2 What that says

**The Underruns were a caller-cadence artefact, not a configuration defect.** At the live
block period a transmit channel at the live geometry underruns **zero** blocks out of 64,
including the first. In a tight loop it underruns 251–255 out of 256 and never recovers.

The mechanism is arithmetic in `iobuffs.c`. A non-blocking channel has exactly one DSP
buffer of slack (`create_iobuffs`: `r2_havesamps = (DSP_MULT - 1) * r2_size`, and
`DSP_MULT` is 2 in `comm.h`). `fexchange2` pushes the input block, releases
`Sem_BuffReady`, and then **immediately** tests `r2_havesamps >= out_size` — so on every
call after the first it is racing a worker it has only just woken. The clamp
`if ((a->r2_havesamps -= a->out_size) < 0) a->r2_havesamps = 0;` means surplus is never
banked: the hit rate is simply the ratio of worker passes to caller calls. A caller running
~20× real time hits ~1 call in 20; a caller running at real time hits every one.

`Hl2TxDsp::processAudioBlock` already has the accumulator that produces this cadence — it
buffers into `m_inBuffer` and processes whole `dspBlockSize` multiples — so **the live
producer already paces correctly**. What must not happen is a test, a bulk re-render, or a
catch-up loop calling `processIq` faster than the audio clock.

### 3.3 An underrun is not a dropped block. It is a permanent slip.

`fexchange2` advances `r2_outidx` on a **miss** as well as on a hit, while `r2_inidx`
advances only when the worker deposits. The `r2` ring holds `DSP_MULT = 2` buffers and the
reader starts one buffer behind the writer; a miss closes that gap and **the reader is
thereafter reading the buffer the writer has just filled instead of the one before it.**
The output stream jumps forward by one whole `out_size` buffer and stays there.

The census measures each settled `Ok` block's correlation phase at the tone frequency
against that block's **absolute** position in the output stream, gated to blocks at full
amplitude so a priming transient cannot be mistaken for a pointer fault. A run where blocks
were merely *dropped* — and the index already accounts for a dropped block — would show a
spread of zero.

| leg | underruns | max phase spread over settled blocks |
|---|---|---|
| paced, 0 underruns (every 5 ms and 21.33 ms run) | 0 | **7.2 × 10⁻⁶ deg** |
| paced 1 ms, **one** underrun at block 12 | 1 | **120.0 deg** |
| unpaced | 240–255 | **164.0–164.3 deg** |

**120.000 degrees at 1 kHz sampled at 48 kHz is exactly 1024 samples — one `out_size`
buffer.** A *single* underrun slipped the output by one entire DSP block, for the rest of
the channel's life, with `ProcessResult::Ok` returned on every block afterwards.

So the cost of one underrun at the live geometry is **two** blocks of audio: 21.33 ms of
silence on the wire, plus 21.33 ms of real audio discarded and never sent. Nothing reports
the second half.

Three consequences:

1. **A TXA transmit path must treat `ProcessResult::Underrun` as a fault to report, not a
   condition to skip past.** S6 §5's "Underrun is the normal start-up state of a WDSP
   channel" does not survive this: at the live cadence there is no start-up Underrun at all
   (zero out of 64, including block 0), and when one does occur it is not benign.
2. **This is a property of `fexchange2`, not of TXA.** The function is direction-agnostic,
   so a receive channel running `blockForOutput = false` slips the same way — and
   `Hl2RxDsp::processIqBlock` currently does `continue; // Underrun while the pipeline
   fills, etc.` **Not measured on a receive channel, and out of scope here**, but it is the
   same code path and it should be checked.
3. `wdsp_channel_test` now asserts the slip is absent, on the live-cadence leg, whenever
   that leg underran nothing — so a loaded machine reports the slip instead of failing
   twice for one cause.

## 4. The zeros

### 4.1 Both causes, and the note reproduced literally

**Cause one — the pipeline never primes.** The `pace 0` leg above produced, in 256 blocks,
**1–5 `Ok` results and 0–2 blocks carrying any signal at all**; the rest were `Underrun`,
and `fexchange2`'s underrun branch is `memset(Iout, 0, ...); memset(Qout, 0, ...)`. That is
the note's sentence, word for word: *Underrun on most blocks and zeros on the rest.* The
`Ok` blocks that do get through are early ones, still inside the 840-sample mute ramp and
`bp0`'s fill, and are therefore zero themselves. **This is a reproduction of the original
failure, at the live geometry, in a test binary, with no hardware.**

**Cause two — the Q plane is discarded.** The S6 study's ranked candidate is correct as a
mechanism, and it is now demonstrated rather than argued:

```
TX live geometry (I only): ok=96 underrun=0 firstNonZero=2 peak=0.100003
TX live geometry (Q only): ok=24                            peak=0
```

A transmit channel fed an identical tone in `inputQ` with `inputI` zeroed produces
**exactly zero** — every sample of every block, `ProcessResult::Ok` throughout, no error
anywhere. `xpanel`'s `inselect = 2` multiplies Q by `(inselect & 1)` = 0. A caller that put
mono audio in the wrong plane would see precisely "zeros", indefinitely, with nothing
diagnostic to go on.

Both causes are now asserted in `wdsp_channel_test`.

### 4.2 The sideband — and the correction that matters

This is where this document disagrees with the S6 study, and the disagreement is the kind
that puts a transmitter on the wrong sideband.

S6 §7 reasons that `xbandpass(bp0)` "given positive edges keeps the positive-frequency
analytic half", concludes that TXA emits the standard analytic convention, and recommends
**keeping `Hl2TxDsp`'s conjugation** and mirroring the SSB passband signs. Its alternative
(b) — no conjugation, `{-2700,-300}` for USB — is the same claim with the sign moved.

**Both are wrong, and the source says so before the measurement does.** `fir_bandpass`
(`third_party/wdsp/upstream/fir.c`) builds the complex impulse for `rtype = 1` as

```
c_impulse[2*i + 0] = + coef * cos (posi * w_osc);
c_impulse[2*i + 1] = - coef * sin (posi * w_osc);
```

i.e. `lowpass · exp(−j·w_osc·pos)` with `w_osc = π(f_high + f_low)/samplerate`. Under
convolution that shifts the lowpass to **−fc**, so a *positive* signed band selects the
**negative** baseband half. (Byte-identical in the 2.00 snapshot at `85e4a1ff^`, so this is
not a 2.10 change — the study's derivation simply went the other way.)

Measured, on the wire-facing plane pair, against an audio tone, with nothing downstream —
the same `binPower` correlation `hl2_txdsp_test` runs on the phasing modulator:

```
TX USB {+300,+2700} 1 kHz:  +1 kHz 1.37e-16   -1 kHz 0.100
TX USB {-2700,-300} 1 kHz:  +1 kHz 0.100      -1 kHz 1.37e-16
```

`hl2_txdsp_test` asserts of `Hl2TxDsp` that *"USB: wire-facing IQ puts energy on the LOWER
side (conjugated)"*. **A TXA channel with positive edges and no conjugation lands on that
same lower bin.** So:

> **Drop the conjugation. Keep `defaultTxPassbandForMode` positive for the USB family, and
> give the LSB family negative edges — the same signed convention
> `defaultPassbandForMode` already uses for receive (`{100,2900}` USB, `{-2900,-100}` LSB).**

Adding the conjugation *and* signing the table, as S6 §7.2 recommends, transmits every SSB
mode on the wrong sideband. So does leaving the table positive and keeping the conjugation.
`docs/HERMES.md` §5's "`SetTXABandpassFreqs` wants positive edges for every mode" is a true
statement about `Hl2TxDsp` and a false one about the WDSP call it names, and should be split
— that part of S6 §9 stands.

**Not measured:** AM, DSB, SAM and FM. `xammod` writes `I == Q` and is handedness-immune,
but `xfmmod` emits `exp(+jφ)` and `TXASetupBPFilters` gives the AM/FM family
`CalcBandpassFilter(bp0, f_low, f_high, 1.0)` with the same signed edges — so the
interaction between the bandpass sign and the modulators that run *after* it is an open
question, and nothing here answers it. No mode other than the SSB family was run.

### 4.3 Opposite-sideband suppression, both chains, same instrument

`tests/hl2_txdsp_test.cpp` now prints the phasing modulator's figure on the `{150, 3000}`
passband `Hl2Backend::defaultTxPassbandForMode` pushes for DIGU and DIGL — the mode WSJT-X
transmits in, and the one no bench leg in this lab has ever run. `wdsp_channel_test` prints
the identical quantity, from the identical correlation, on the identical wire convention,
for a TXA channel at the live geometry.

| tone | `Hl2TxDsp` (255 Blackman taps) | TXA `bp0` (2048 taps) | delta |
|---|---|---|---|
| 150 Hz | **22.0 dB** | **67.8 dB** | +45.8 dB |
| 200 Hz | **30.6 dB** | **66.1 dB** | +35.5 dB |
| 300 Hz | **53.2 dB** | **69.7 dB** | +16.5 dB |
| 1000 Hz | **76.9 dB** | 287.8 dB — see below | — |

S6's 22 dB and 30.6 dB at 150 and 200 Hz were *derived from the filter design*. They are
now **measured**, and they agree to 0.0 dB. The derivation was right.

Two further figures, on the `{300, 2700}` voice passband:

- **1 kHz opposite-sideband image: 1.37 × 10⁻¹⁶ against a wanted bin of 0.100.** That is
  −297 dB, which is the arithmetic floor of the correlation and **not a physical
  suppression claim.** What it establishes is that TXA's complex bandpass introduces no
  measurable opposite-sideband image at 1 kHz — the limit is set by everything downstream
  (the EP2 seam, the DAC, the PA), not by the modulator. `Hl2TxDsp` measures 85.4 dB on the
  same passband in the same binary, and this lab's bench figure is 87.15–87.19 dB.
- **Out-of-passband: a 5 kHz tone against a 2700 Hz edge lands 204.9 dB below an in-band
  tone.** This is the assertion that caught the wideband-Hilbert bug on the phasing
  modulator.

Conversion gain is unity and matches: audio peak 0.1 in, `|IQ|` peak 0.100003 out —
`CalcBandpassFilter(bp0, …, 2.0)`'s gain of 2.0 is exactly what turns a real cosine of
amplitude A into an analytic signal of magnitude A.

---

## 5. What this changes in the S6 sequencing

| S6 item | status after this |
|---|---|
| §1 "there is no missing initialisation sequence for SSB" | **confirmed** on 2.10, and confirmed by a channel that runs |
| §5 "`blockForOutput = true` makes the underrun branch unreachable" | **confirmed** on 2.10 |
| §5 "with `bfo = 0`, Underrun is the normal start-up state" | **wrong, and the correction is better news.** At the live cadence there is no start-up Underrun at all — zero out of 64, including block 0. Underrun is the normal state of an *unpaced* caller, which the live path is not. A TXA transmit path should treat Underrun as a **fault to be reported**, not a condition to `continue` past as `Hl2RxDsp::processIqBlock` does. |
| §3.5 "a miss discards ring position permanently" | **right for the wrong reason, and worse than it says.** The `r2_havesamps` clamp destroys *credit*, not position, and the hit rate is simply the producer/consumer rate ratio — there is no latched state there. But the `r2_outidx` advance on a miss *is* permanent, and it is now measured: one underrun slips the output by exactly one `out_size` buffer, for good. See §3.3. |
| §3.6 candidate 1 "the audio was put in the wrong plane" | **demonstrated as a mechanism** (Q-only gives exact zeros) — but it is *not needed* to explain the note, because §4.1 reproduces "Underrun on most blocks and zeros on the rest" from the cadence alone |
| §7.2 "(a) keep the conjugation, mirror the passband signs" | **wrong. Both (a) and (b) transmit on the wrong sideband.** See §4.2 |
| §4 item 2 "TXA should beat 22 dB at 150 Hz by a wide margin" | **measured: 67.8 dB vs 22.0 dB.** The prediction holds, by 45.8 dB |
| §9 "the gate costs about twenty lines" | it cost about 260, mostly because pacing, absolute sample indices and the plane asymmetry all had to be in it |

The lifecycle prerequisite is implemented by `WdspChannel::discardTransmitData()`.
It retains the channel and FFTW plans, clears TX rings and DSP history under the
existing control fence and channel locks, and leaves the channel stopped. The
next authorized audio block starts it. A pending asynchronous fade/flush is
refused; HL2 disables the modulator on refusal until reconfigured.

---

## 6. Where the new code is

| thing | file | symbol |
|---|---|---|
| live-geometry transmit config | `tests/wdsp_channel_test.cpp` | `liveTransmitConfig` |
| the census, the plane test, the sideband and low-edge measurements | `tests/wdsp_channel_test.cpp` | `runTransmitLiveGeometryTest`, `runTransmitChannel` |
| the correlation, extended to carry absolute sample index | `tests/wdsp_channel_test.cpp` | `binPower` |
| the phasing modulator's low-edge figure, diagnostic only | `tests/hl2_txdsp_test.cpp` | the `{150, 3000}` block before the DIGU/DIGL sideband cases |

Both targets are registered unconditionally in `tests/tests.cmake` and run in the default
`ctest` graph. The new case adds ≈10 s of wall clock to `wdsp_channel_test`, almost all of
it deliberate sleeping.

---

## 7. What was NOT measured

**2026-09-17 — hardware time now exists, and what it did NOT see matters as much
as what it did.** Three runs on ON8ST's Hermes-Lite 2, board id 6, gateware 74:

| run | what it was | what it establishes |
|---|---|---|
| 2026-09-16 22:36, antenna | operator's own over, **heard correct on an external SDR receiver** | the emission was right — on a build whose `resetModulatorState()` is EMPTY, i.e. NOT the merged code |
| d104, dummy load | 2 × 5 s, 1 kHz tone, 40 % drive, merged code | power up within ~270 ms, steady 0.47–0.50 W, SWR 1.00–1.13, ALC 0 dB / −19.99 dBFS, clean unkey, second over indistinguishable from the first |
| d105, antenna | 2 × 5 s, 3.695 MHz USB, merged code, ID first | steady 0.49–0.52 W, **SWR 1.57–1.69** against the load's 1.07 — a real mismatch where one belongs, which is what shows the RF reached the antenna |

**WHAT NONE OF THEM OBSERVED IS THE EMISSION ITSELF, ON THE MERGED CODE.**
Forward power, SWR, ALC gain and ALC dBFS are readings of the load and the level
chain; every one is unchanged by an inverted or spurious emission. This file's
own §1 records the precedent — the phasing modulator once transmitted every
signal on the wrong sideband and it took *an operator with a second receiver* to
catch it, because it is "invisible from inside this application". The d105 over
was **USB on 80 m**, where an inverted emission is LSB, the band's conventional
sideband: it would have sounded entirely ordinary, and nobody was listening.

So one run observed the emission on the wrong code, and one ran the right code
without observing the emission. **No single run covers both.** Raised by
aethersdr-agent on #5779.

**AND NONE OF THEM EXERCISED THE PAYOFF.** §1 says the migration's return is the
LOW EDGE of the `{150, 3000}` DIGU/DIGL passband, and that at 1 kHz the
incumbent already reads 87 dB against EP2's ~96 dB wire so TXA's advantage there
is *below the wire and unusable*. The dummy over was a 1 kHz tone and the
antenna over was voice. There is no DIGU/DIGL over and no WSJT-X transmission —
the hardware time on record is at the one frequency this document says is
uninformative.

- **The figures below predate all of that.** No radio was keyed for them and no
  antenna port was observed. Every figure here is
  from a test binary.
- **One machine, one architecture, one build type.** macOS/arm64, RelWithDebInfo. Not run
  on x86-64, not run under ASan or TSan, not run on Linux or Windows.
- **Group delay.** S6 derives +21.6 ms for TXA's `rsmpin` + `bp0` over `Hl2TxDsp`'s 255
  taps, plus one `dsp_size` block because `wdspmain` calls `dexchange` before `xtxa`. The
  per-block phase measurement shows the delay is *constant* to 7.2 × 10⁻⁶ degrees but does
  not measure its *value* — a single frequency fixes delay only modulo one period.
- **Any mode but SSB.** No AM, DSB, SAM, FM or CW channel was opened. §4.2 flags the
  specific open question for the AM/FM family.
- **Unkey is now covered offline.** A tone followed by reset, no clocking for
  0 or 500 ms, and silence must emit silence from the first output sample.
  Repeated reset and subsequent tone recovery are covered. This replaces the
  earlier settled-only measurement, which discarded the defect's first 85 ms.

- **Control calls during transmit.** The later running-mode-change regression
  exercises USB → LSB after feeding audio. Concurrent control admission
  remains governed by the WdspChannel operation fence.
- **Memory and FFTW planning cost.** Not measured. S6 §12's ≈23 MB of minimum-phase
  workspace is arithmetic from 2.00 and has **not** been re-derived against 2.10, which
  introduced an impulse cache in `fir_bandpass` that 2.00 did not have. The NNR stages 2.10
  adds are `create_rxa`-only and cost a transmit channel nothing.
- **Cold-connect cost.** Not measured; S6 §13's argument is untouched.
- **IMD.** No two-tone measurement exists for either chain.
- **The EP2 seam.** Unchanged by any of this, and a TXA migration must not be proposed as
  fixing it.

---

## 8. Contributor ctest results before review repairs

Rebased onto `origin/main` at `8f4b4dc1`, which is the tip carrying #5646 and
#5647 — the two @ten9876 asked to land before TXA work touched `Hl2TxDsp`. That
precondition is now met rather than worked around.

**Both builds, full suite:**

| build | result | failures |
|---|---|---|
| `AETHER_HL2_TX_TXA=ON` (opt-in) | **461 / 463** | `tgxl_docked_parity_test`, `vkamp_connection_test` |
| `AETHER_HL2_TX_TXA=OFF` (phasing) | **462 / 463** | `tgxl_docked_parity_test` |

`wdsp_channel_test` and `hl2_txdsp_test` pass in both.

- `tgxl_docked_parity_test` — **deterministic, not ours.** Fails on the
  `QFontMetrics(drawn).horizontalAdvance(btn->text()) <= btn->width() - 6`
  assertion, preceded by `ThemeManager: saved theme "Default Dark" is
  unavailable` and `qt.qpa.fonts: ... missing font family "Sans Serif"`. A
  font/theme-resource environment failure; #5676. Its `tests.cmake` block
  compiles the TunerApplet/TGXL/ThemeManager sources and links Qt6
  Core/Gui/Widgets/Network — it shares no translation unit with anything changed
  here.
- `vkamp_connection_test` — **flaky, not ours, and flaky in isolation too.** It
  failed the full TXA run; re-run alone it failed once and then passed. So it is
  not merely parallel-execution contention — it is intermittent on its own,
  which matches the socket-binding behaviour @ten9876 reported. It does not
  appear in the phasing run at all. Its block compiles `tests/vkamp_connection_test.cpp`,
  `src/core/VkampConnection.cpp`, `src/core/VkampProtocol.cpp`, the settings sources and
  the log writer, and links Qt6 Core/Network/Test — again no shared translation unit.

These historical full-suite results are contributor evidence from that revision,
not a full-suite run of the final review repairs. The PR records final focused
validation separately.

## Reset correction in PR #5747

A nonblocking `setRunning(false)` only schedules WDSP's fade/flush. Restarting
without clocking that fade cancels it, retaining the previous transmission's
queued audio. Independent review measured a 0.5335 peak over 85.4 ms after reset
and a 500 ms receive interval, with no underruns; the phasing control emitted
exact silence. Calling this only pipeline latency did not satisfy the reset
contract. The former assertion skipped 256 ms and passed with reset deleted.

The explicit TX-only discard is documented as WDSP patch 11. It uses existing
locks, retains DSP allocation/plans, and refreshes the output semaphore through
WDSP's existing ring flush. It is a control-path operation, not an audio callback
or a fade to be emitted on air. RX and a pending asynchronous flush are refused.
The regression measures all post-reset samples and keeps real-time pacing so
starvation cannot masquerade as success. No hardware or RF behavior is claimed.
