# Hermes-Lite 2 Bring-Up — Field Notes

Working notes from the HL2 receive bring-up on `feat/hl2-backend` (2026-07-24,
macOS 26.5.2 / arm64). Written to be *studied*, not just read: the last section
turns what happened into a proposed automated bring-up sequence.

Status: HL2 receives, transmits, and runs WSJT-X over TCI on live hardware —
confirmed by 63 PSK Reporter spots on 14.074 DIGU. The slice is decoupled from
the DDC so the panadapter holds still while tuning.

Section 11 audits the receive bring-up against the independent correctness
oracles at `/Users/patj/oracles/hl2/`.

**Start here for a new backend:** §15 (receive handedness and tuning) and §5's
sideband-selection rules. Those two describe the most expensive bug of the
project — one that survived a full session of correct-looking measurements —
and §15.6 is the checklist that would have caught it on day one.

---

## 1. What makes HL2 different, and why it broke things

Flex hardware demodulates and ships **cooked audio + a hardware spectrum**.
HL2 ships **raw IQ and nothing else**, so the backend owns an engine-side WDSP
chain. It is the first backend to exercise that branch of the seam.

Almost every defect found in this session traces to one of two root shapes:

| Shape | Consequence |
|---|---|
| Code assumes a Flex-only object exists | Null deref, or a silently dropped intent |
| Code assumes Flex firmware will interpret a value | We hand the raw value to WDSP, which has different conventions |

That is the lens to bring to the *next* non-Flex backend. Neither shape is
visible from the interface; both are only visible at runtime.

---

## 2. The single most important lesson

**The decisive bug was found by reading reference implementations, not by
measuring.**

`Hl2RxDsp` opened its WDSP channel with `dsp_rate` = the 24 kHz audio rate.
WDSP's RXA stages are built around a 48 kHz internal rate. Both reference
clients hold it there unconditionally:

```c
// Thetis — Project Files/Source/ChannelMaster/cmaster.c, create_rcvr()
OpenChannel(chid, xcm_insize, 4096, xcm_inrate,
            48000,             // dsp rate — literal
            rcvr.ch_outrate,   // output rate — independent
            ...);

// pihpsdr — receiver.c
OpenChannel(rx->id, rx->buffer_size, rx->fft_size, rx->sample_rate,
            48000,             // dsp rate
            48000,             // output rate
            ...);
```

Neither derives `dsp_rate` from input or output rate. We did.

Why measurement never found it: `dsp_rate = 24000` is **not wrong in
isolation**. It passes `validateConfig()`, it is internally consistent, the
frame arithmetic balances (1024 in @48k → 512 out @24k), and the delivered
frame rate measured 23,936/s against 24,000 nominal — correct. It is only
wrong against a convention that exists solely in the reference clients.

Effect of the fix, identical capture conditions:

| | Before | After |
|---|---|---|
| Peak sample | 1.779 (5 dB over FS) | **0.1433** |
| RMS | 0.1209 | 0.0353 |
| 93.75 Hz comb + harmonics | strong | **gone** |

Time cost of not doing this first: roughly four rounds of measurement and two
wrong hypotheses (below).

---

## 3. Wrong turns, and what each one cost

Recording these because an automated process should be designed to make them
cheap or impossible.

| Hypothesis | Why it looked right | How it died |
|---|---|---|
| macOS broadcast discovery is broken | Python `sendto` to `255.255.255.255` → `OSError 65` with two interfaces up | Qt's in-app sweep works fine. **Tested before "fixing".** |
| `dsp_size` mismatch causes the warble | Autocorrelation showed peaks at every multiple of 1024 | Peaks were local maxima on a smoothly decaying autocorrelation — any continuous audio does that. r was 0.610 before, 0.700 after. |
| Spectrum is I/Q-inverted | Sim tones landed at negative offsets | Sim builds `I=sin, Q=cos`; `sin θ + j cos θ = j·e^(−jθ)` is negative-frequency *by construction*. Our decode was right. |
| Clipping masks the tones | Peak 2.64, 10% of samples at FS | Comb survived with AGC fully off. |
| Half of each 512-frame block is stale | Would explain both comb and 2× stretch | Correlation between block halves = 0.048. Not a repeat. |
| AM filter is the pitch bug | AM really does get an SSB passband (real bug!) | Operator reported USB *also* low-pitched. |

**Pattern:** four of six died on a cheap measurement that took minutes. The
expensive part was never the test — it was choosing which test to run. A
reference-comparison step up front would have skipped all of them.

---

## 4. Protocol facts (HPSDR Protocol 1 / Metis)

### The C&C bank we were missing

`MetisClient` sent three banks: config `0x00`, RX1 frequency `0x04`, LNA gain
`0x14`. Protocol 1 also defines **`C0=0x1C`** (address `0x0e`) — per-receiver
ADC assignment: C1 holds RX1–4 (2 bits each, LSB first), C2 holds RX5–7, C3
bits[4:0] TX attenuation.

The HL2 has one ADC and works without it. A conforming multi-ADC device leaves
every receiver **unassigned** and emits:

> correctly framed, correctly sequenced, correctly paced, **all-zero** IQ

This is the nastiest failure mode encountered all session, because every health
signal reads nominal — packet count, sequence continuity, sample rate, 0.00%
loss — and only the sample *values* give it away. Both AetherSDR and the
Phase-0 Python spike had this bug; neither could have found it on HL2 hardware.

**Automation requirement:** a data-plane health check must assert on sample
statistics (RMS, peak, non-zero fraction), never only on packet counts.

### Measured wire behaviour (48 kHz, against hpsdrsim)

| Quantity | Measured | Expected |
|---|---|---|
| IQ sample rate | 47,974/s | 48,000 |
| EP6 payload | 126 samples/packet | 126 |
| Inter-arrival mean | 2.625 ms | 2.625 ms |
| Inter-arrival p50 / p99 / max | 2.615 / 3.25 / 6.08 ms | — |

The p99/max figures are the real input for sizing the SPSC queue between the
UDP thread and DSP: it needs ≥3 packets of slack to absorb observed jitter.

### Ordering

A stream started before any C&C frame has landed emits ADC-idle samples. Prime
with C&C **before** `metis-start`. (The earlier `CONFIG_MERCURY` diagnosis was
wrong — HL2 gateware never decodes that bit; ordering was the real cause. Both
the design note and `docs/archive/hl2-phase0-spike.md` carry the correction.)

---

## 5. WDSP configuration facts

```
in_size   = 1024                    complex samples per fexchange2 call, at in_rate
dsp_size  = in_size * dsp_rate / in_rate     → 1024 @48k, 512/256/128 @96/192/384k
in_rate   = HL2 IQ rate             48/96/192/384 kHz
dsp_rate  = 48000                   CONSTANT. Not the input rate. Not the audio rate.
out_rate  = 24000                   AudioEngine::DEFAULT_SAMPLE_RATE
out_size  = in_size / (in_rate/out_rate)  → 512 frames
```

From WDSP's own `channel.c:40-52`:

```c
dsp_insize  = dsp_size * (in_rate  / dsp_rate);
dsp_outsize = dsp_size * (out_rate / dsp_rate);
out_size    = in_size  / (in_rate  / out_rate);
```

Note `out_size` depends **only** on `in_size` and the input/output rates. It is
independent of `dsp_size`, so `dsp_size` can never affect pitch — useful for
ruling things out quickly.

`validateConfig()` checks rate divisibility and the output-block arithmetic but
**not** the `dsp_size`/`dsp_rate` relationship, which is how a bad value passed.

### Sideband selection — the mode does NOT choose it

Two facts that took a full session to establish, and that no amount of reading
WDSP's headers would have given us. Both measured against WWV on live hardware.

- **RX: the passband edges select the sideband, not the mode.** `SetRXAMode`
  rebuilds the NBP stage from its own per-mode notion of the passband, so any
  filter applied *before* the mode call is discarded by it. **Order is
  load-bearing: mode first, then passband, and re-push the passband on every
  mode set** — not only when its value changed.
- **RX: WDSP's RXA selects the OPPOSITE sign to its passband bounds.** USB
  configured `[+150, +3000]` passes *negative* analytic frequencies. Confirmed
  independently by `hl2_rxdsp_test` and `hl2_shift_test`. This is the single
  least intuitive fact in the whole backend and everything in §15 follows from
  it.
- **TX is the mirror image: the MODE selects the sideband and the bandpass is an
  audio-domain magnitude.** `SetTXABandpassFreqs` wants **positive** edges for
  every mode. Handing TX the RX table's signed pairs put LSB and DIGL on the
  upper sideband — caught by `hl2_txdsp_test` before it shipped, which is why
  `Hl2Backend` keeps two separate tables (`defaultPassbandForMode` signed for RX,
  `defaultTxPassbandForMode` positive for TX).

The trap: RX and TX use **opposite conventions**, and both look plausible. A
table written for one and reused for the other is silently wrong on exactly half
the modes.

### CW has no BFO unless you build one

`SetRXAMode(CWU)` does **not** insert a beat oscillator. In this chain the NBP
edges select the sideband (above) and the detector is direct-conversion for
every mode, so a CW channel configured like an SSB one puts the tuned frequency
at **DC** — the signal under the marker is silent, and the operator has to tune
a whole pitch off it to hear anything.

That is not the convention any other client uses. SmartSDR, Thetis and this
app's own `VfoWidget::applyFilterPreset` all put the marker **on the signal**
and produce the pitch from a BFO; FlexLib clamps CW filter cuts to
`±12000 - CWPitch`, which only makes sense for cuts measured from the carrier.
It matters more on an HL2 than on most radios because the **gateware generates
the shaped CW carrier at the TX NCO** (wiki `Protocol.md`: "a shaped CW signal
is generated by the Hermes-Lite2"), i.e. exactly at the marker — so a receiver
offset from the marker is a receiver offset from where the radio transmits.

`Hl2Backend` therefore keeps CW in **two domains**, and `cwBfoHz` /
`dspFilterHz` / `rxShiftHz` are the only translation between them:

| | measured from | 500 Hz filter at a 600 Hz pitch |
|---|---|---|
| operator-facing (`Receiver::filterLowHz/HighHz`, the seam, the panadapter) | the marker | `{-250, +250}` |
| demodulator (`Hl2RxDsp::Config`, `setFilter`) | DC | `{350, 850}` |

The BFO is `+pitch` for CWU/CW, `-pitch` for CWL, and **zero for every other
mode** — which is why all receivers route through those helpers rather than only
the CW ones. The shift carries it as `dspShiftHz(slice, nco) - bfo`: **minus**,
because the shift names the RF frequency the detector treats as zero, so pushing
that zero *down* a pitch is what lifts the marker *up* onto it. Both CW modes
share one `defaultPassbandForMode` entry now; the sideband lives in the BFO's
sign, not the filter's.

Two consequences worth remembering:

- The pitch is **not a transmit setting** for a backend that demodulates. It
  reaches the seam as `IRadioBackend::setCwPitch()` (Flex ignores it — it has
  `cw pitch` as text and its own on-radio BFO), and a pitch change re-pushes
  both the shift and the passband. Re-pushing only one leaves the detector and
  the filter disagreeing: the tone moves and the signal fades.
- Entering *or leaving* CW has to re-push the shift, not just the filter.
  Otherwise leaving CW strands every other mode a pitch high, which reads as
  "the radio is off frequency" long after the operator left CW behind.

Pinned by `hl2_cw_bfo_test` (audio actually lands on the pitch, and a signal a
pitch away from the marker is rejected) and by `hl2_backend_test` (the seam
reports symmetric cuts, and a pitch change does not move them).

### AGC

- `SetRXAAGCTop` is the **maximum gain in dB**, and 120 dB is the top of WDSP's
  range. Inheriting that default ran the HL2 wide open: peak 3.186, **10.31% of
  samples at or beyond full scale**. At a 65 dB ceiling: peak 2.664, 0.27%.
- Mode vocabulary: `off/slow/med/fast` → WDSP RXA 0/2/3/4. WDSP's "long" (1)
  has no representation in the four-way UI control.
- **The AGC is client-owned state and nothing on the radio can be asked for
  it.** There is no AGC register in the HPSDR map — the whole loop is WDSP on
  this host — so the operator's mode and threshold live in the client's
  operating-state document or nowhere. They ride the RFC #4603 `Agc` domain as
  typed universal fields (`RestoredRadioState::agcMode` /
  `agcThreshold` — 0..100 operator units, NOT dB; the backend multiplies by
  `kAgcCeilingDbPerUnit` to reach real dB), captured on every `setSliceAgc` and
  seeded back onto **every** receiver by `Hl2Backend::seedReceiverAgc()`.
- **"Flat" means ONE remembered pair, not one AGC.** The runtime control is
  per-receiver — `setSliceAgc(sliceId, …)` resolves `ddcForSlice()` and writes a
  single `Receiver`, and `emitSliceState()` publishes per-DDC — so an operator
  running two receivers really can have RX1 on `slow`/40 and RX2 on `fast`/30
  within a session. What is flat is the *memory*: one pair is captured and it is
  seeded onto every receiver at the next connect, so that divergence does not
  survive a restart. This is a deliberate product call (an operator's AGC is a
  property of how they like to listen, like the TX cut points) rather than an
  oversight, and it is worth stating plainly because "flat" on its own reads as
  "there is only one AGC" and sends the next person debugging RX2 looking for a
  bug. The pair that gets remembered is **the last one the operator set**, on
  whichever receiver — `setSliceAgc()` records it, and `currentOperatingState()`
  reads that rather than the transmit receiver. Reading the TX receiver instead
  meant a change on RX2 fired the capture and then persisted RX1's untouched
  value: the change that triggered the write was not the change that got
  written.
- **"At the next connect" means a NEW radio, not a returning one.** The seeding
  runs from `connectRadio()` when the connect request's serial differs from the
  last one seeded, or when `buildReceivers()` had no previous state to carry —
  never on a plain auto-reconnect to the same radio. `buildReceivers()`
  deliberately preserves receiver state across a rebuild and
  `RadioModel::handRestoredStateToBackend()` re-hands the document before EVERY
  connect, so seeding from `applyRestoredState()` meant a dropped link flattened
  RX2's live AGC back onto the remembered pair while its mode and passband
  survived — the sibling restore engineers around exactly that. Flat memory
  across a restart is the design; flattening live receivers mid-session is a
  loss. `applyRestoredState()` still resets the CAPTURE side (the remembered
  pair belongs to the radio whose document it is), which is what keeps radio A's
  AGC from being written back under radio B's identity.
- The threshold's "not restored" sentinel is **-1, not 0**: 0 is a threshold the
  operator can select, so a zero-means-absent encoding would quietly reset
  anyone running the AGC-T at the bottom of the slider.
- Restore is **flat**, not per band or per mode, unlike the drive and LNA maps.
  Same reasoning as the TX cut points in §14.9: the control is one pair, and
  making it jump on a band change would be a surprise, not a memory.

### AM/SAM hand back a DC pedestal, and nothing upstream removes it

WDSP's AM/SAM detector is an **envelope** detector — `amd.c` emits
`sqrt(I² + Q²)`, which is strictly non-negative — so the carrier arrives in the
demodulated audio as DC. Three things that each look like they would remove it,
and do not:

- **`levelfade` is not a DC blocker.** On by default (`RXA.c`). It computes
  `audio += dc_insert - dc` from an 8 Hz-corner average and a 0.11 Hz-corner
  average. Those cancel *fading*; they deliberately **hold** the pedestal at the
  long-term carrier level. That is the entire point of the stage.
- **The AM/SAM passband cannot strip it.** `defaultPassbandForMode` gives AM and
  SAM `{-4000, +4000}` — symmetric about the carrier, because both detectors
  need it that way — which puts 0 Hz mid-band.
- **AetherSDR had no DC blocker on the RX demodulated-audio path.** The only
  `setDcBlockEnabled` in the tree is on the **TX** final limiter
  (`ClientFinalLimiter`). `ClientPudu` has a one-pole DC block too, but it is
  internal to the opt-in Aphex HF path and exists to remove the offset that
  path's own one-sided clipping introduces — it is not in the chain unless the
  operator switched that effect on, and it is downstream of the applet anyway.

Measured on a 50%-modulated carrier, unfixed: settled audio mean **1.79** on a
±1.0 float scale, AC RMS 0.70 — measured at the `WdspChannel` output, so that is
*after* AGC. The pedestal alone is 79% past full scale, so AM audio clipped hard
against the rails everywhere downstream.

Every zero-referenced consumer downstream inherits the offset. The visible
symptom was the **WAVE applet drawing two waveforms, the lower one inverted**:
it renders `peak` and `rms` — both magnitudes — mirrored about a hard
centreline, so a DC-shifted trace draws a phantom upside-down copy of itself in
the bottom half. SSB looked fine throughout because it is already zero-mean, and
that asymmetry is the tell.

`Hl2RxDsp` now applies a 20 Hz one-pole DC blocker per channel to the
`WdspChannel` output, **unconditionally for every mode** — SSB/CW are already
zero-mean so it is a no-op there, FM wants it for the same reason AM does, and
an unconditional filter has no mode-change state to get wrong. Guarded by
`hl2_am_dcblock_test`, which measures the *settled tail* (`dc_insert` is a 1.4 s
pole, so a short burst shows almost no DC even unfixed) and asserts a DC/AC
**ratio** rather than a level, since AGC scales both equally.

That test also pins the corner, which is the half of the property that is easy
to satisfy by accident: a blocker whose corner has crept up into the audio band
removes the pedestal just as thoroughly while eating the bass out of every mode,
and every DC measurement stays green through it. So it checks a 60 Hz vs 400 Hz
modulation ratio through the real chain, the closed-form `|H(f)|` at three audio
rates, and the unconfigured bypass. Six 4 s bursts through the real chain cost
2.5 s, so it is a cheap test — but only with a warm FFTW wisdom cache. Cold, the
same binary takes 178 s, because WDSP's first `OpenChannel` measures FFTW
`PATIENT` plans (see `WdspChannel::open()`, cached at
`$XDG_CACHE_HOME/aethersdr/wdsp-fftw-wisdom`). A CI container starts cold every
run, which is why this sat at 188-190 s on the per-PR gate and now runs weekly
under the sanitizers job instead. Cache that file in CI and it belongs back on
the gate.

**Why on `Hl2RxDsp` and not on `WdspChannel`.** The root cause is `amd`'s
envelope detector, which belongs to WDSP, so a blocker on `WdspChannel`'s own RX
output would fix it once for every future consumer rather than per caller.
`Hl2RxDsp` is the only WDSP **receive** consumer in the tree today — `Hl2TxDsp`
is the one other user and is transmit-only — so per-caller costs nothing yet.
The next WDSP RX path added will not inherit it: push the blocker down into
`WdspChannel` at that point rather than repeating it.

**What this does not fix.** The blocker is downstream of the entire RXA chain,
so `wcpAGC` — which sits after `amd` *inside* RXA — still rides the pedestal.
Its gain decisions on AM/SAM remain biased by the carrier. Correcting that needs
DC removal between `amd` and the AGC, and WDSP exposes no hook there;
`SetRXAAMDFadeLevel(0)` is not one, since it only drops the fade correction and
leaves `sqrt(I² + Q²)` just as non-negative. Left as a known residual.

---

## 6. Seam gaps found (the reusable checklist)

Each of these is "a Flex assumption that a DSP-owning backend violates".

**Gaps 16–19 are in §18.6**, kept there because they share one root cause (RX
audio features bind to a transport rather than to the radio) and reading them
apart from that audit loses the point.

| # | Gap | Symptom | Fix |
|---|---|---|---|
| 1 | `RadioModel::m_panStream` only assigned in the Flex `dynamic_cast` branch (`RadioModel.cpp:443`) | `startDax()` deref'd null → **SIGSEGV 3 s after every connect** | Guard at `startDax()` entry (`e556ad01`) |
| 2 | Missing ADC-assign C&C bank | All-zero IQ on conforming devices | `5c6c2fdd` |
| 3 | AGC never reached the backend | **Dead slider** — UI moved, DSP unchanged | `4d2bc494` |
| 4 | `dsp_rate` derived from audio rate | Low-pitched, warbling audio | `74f10f53` |
| 5 | Mode change mirrors the passband in the model **without** emitting operator intent | Model and DSP silently diverge | *Open* — `slice filter` verb works around it |
| 6 | AM is in neither filter-polarity family (`SliceModel.cpp:47-57`) | AM gets an SSB passband that excludes the carrier | *Open* |
| 7 | No pan-geometry down-verb on `IRadioBackend` | Zoom/pan can't reach the backend; waterfall and pan disagree | *Open* — structural |
| 8 | Slice frequency **is** pan center (`Hl2Backend.cpp:165`) | Click-to-tune recenters the world instead of landing | *Open* — needs slice-offset-within-passband |
| 9 | Same null-deref shape in the RADE path (`MainWindow_DigitalModes.cpp:461`) | Will crash HL2 whenever RADE starts | *Open* |
| 10 | ~~`AETHER_AUTOMATION_NO_AUTOCONNECT` appears not to suppress autoconnect~~ | Test instance grabs a radio | **Not a bug — the variable does not exist.** Removed application-wide by #4421/#4401; autoconnect is `AutoConnectToLastRadio` alone (`MainWindow.cpp`). Use the isolated profile in §10 |
| 11 | `SpectrumWidget` **drops** inbound pan geometry during a gesture, assuming another status is coming | View parks at the old centre while slice/pan/waterfall move — measured **permanently 6.3 kHz** out after one drag-tune | `3d52d07d` |

| 12 | Slice frequency WAS the DDC NCO, so the pan centre tracked every tune | Display re-centred on every click; a slice offset from centre was unrepresentable | `a1cbe154` |
| 13 | RX filter set via `SetRXABandpassFreqs` alone, leaving the NBP stage — the filter actually in circuit — untouched | No sideband selection and no filtering AT ALL; 0 dB rejection of a tone outside the passband | `86a3d27b` |
| 14 | HPSDR wire IQ handedness is opposite to WDSP's | USB demodulated the lower sideband and LSB the upper — audibly swapped, while the panadapter looked correct | `79c54266` |
| 15 | AM in neither filter-polarity family | Switching to AM kept an SSB passband that filters the carrier OUT, so the envelope detector distorts rather than going quiet | `2996f0eb` |
| 16 | AGC reached the backend but **nothing remembered or echoed it** — `setSliceAgc` never called `notifyOperatingStateChanged()`, no `Agc` domain existed, and `emitSliceState` never published the pair | Every launch reopened the WDSP channel on `med`/65; the operator's AGC was gone, and a restored value would have been invisible anyway because the applet kept showing `SliceModel`'s own defaults | #4909 |

**Gap 13 is the second instance of the §2 lesson** — a plausible low-level API
used where both reference clients use the canonical composite one
(`RXASetPassband`). Neither call is wrong in isolation. Add to the Phase-0
reference diff: *for every vendor call we make, check whether the references use
a higher-level wrapper instead* — a wrapper usually exists because it sets more
than one stage.

**Gap 14 hid behind gap 13.** Until something actually selected a sideband, USB
and LSB sounded equally wrong and the swap was indistinguishable from general
breakage. Fixing the filter is what made it measurable. Expect this ordering:
some defects are only observable once a more basic one is repaired.

**Gap 11 is the most transferable lesson in this file.** The suppression is
correct — an echo arriving mid-drag is stale. It was *safe* only because Flex
re-echoes pan status continuously, so a dropped value is replaced within
milliseconds. That assumption is nowhere in the code. A backend that publishes
geometry only when it **changes** (the HL2 emits its pan centre from the RX NCO,
once, on tune) loses it forever.

Generalised rule, worth applying to every inbound path when adding a backend:

> **Ask whether each producer is level-triggered (re-asserts state) or
> edge-triggered (announces changes). Any code that drops an update "because
> another will arrive" is only correct for the first kind.**

The fix is the inbound half of #4142's "defer, never drop" — but re-read the
*model* on release rather than replaying the suppressed value, or you resurrect
the stale echo the suppression existed to reject.

**Principle II trap (hit twice):** `agcModeChanged`/`agcThresholdChanged` and
`filterChanged` are emitted from *both* operator setters and status
application. Driving a backend command off them echoes the radio's own state
back at it as a request. Operator-only intent signals are required —
`frequencyCommandIssued`, `filterCommandIssued`, and now `agcCommandIssued`.

---

## 7. The test fixture: hpsdrsim

Built from `g0orx/pihpsdr` and kept **outside** the AetherSDR tree at
`/Users/patj/aether/tools-external/pihpsdr` (GPL-3; behavioural reference only,
no code incorporated).

```bash
make hpsdrsim
./hpsdrsim -hermeslite2 -P1
```

Appears as serial `AA:BB:CC:DD:88:FF` (the `88` is its `-hermeslite2` MAC
byte), distinguishable from the real HL2 (`00:1C:C0:A2:13:DD`, gateware 7.4,
192.168.1.21).

### What it gives you

- Broadband ADC noise (amplitude 0.00003) plus two tones at **800 Hz and
  4000 Hz**, both at **−73 dBm** (= S9).
- Convention: **0 dBFS ≡ 0 dBm**. This is what let us confirm the dBFS→dBm
  constant, which the design note lists as an open question.

### Fixture gotchas — all cost time

1. Its header comment says "5000 Hz"; the actual phase increment
   (`0.016362461737… × 1536000 / 2π`) is **4000 Hz**. Trust the code.
2. Its tones are **negative-frequency by construction** (`I=sin, Q=cos`), so
   they only appear in **LSB**.
3. It **never models the receiver NCO** — tones sit at fixed baseband offsets
   regardless of tuning, so it cannot test frequency-offset behaviour.
4. `rx_adc[]` defaults to `-1` → all-zero IQ until `C0=0x1C` arrives.
5. Its C&C logging is **change-only**, so a reconnect can look silent. Restart
   the sim between test runs rather than trusting a quiet log.
6. It carries a strong **DC offset on I**. Any stage that translates frequency
   moves that spur too, where it impersonates the signal. Use a synthetic tone
   for sign/scale questions, not the simulator.
7. Stale instances hold UDP 1024. `pkill -f hpsdrsim` — note a `./hpsdrsim`
   invocation won't match a full-path pattern.

**Open question:** with everything correct, the sim's tones still don't resolve
in demodulated audio while the panadapter shows them ~55 dB above the floor.
Live audio is correct, so this is a fixture artifact — but understand it before
leaning on the sim for audio-path assertions.

---

## 8. Automation: what existed, what was added, what's still missing

### Added this session

| Verb | Why |
|---|---|
| `slice filter <lowHz> <highHz>` | Passband was unassertable, making every audio measurement untrustworthy |
| `slice agc <mode> [threshold]` | A control that can't be driven headlessly can't be regression-tested |
| `wheel <target> <x> <y> <steps>` | Of the four ways to move the VFO, the wheel was the only one with no verb — so the only one that could not be regression-tested |
| `wfRowLowMhz`/`wfRowHighMhz` + `wfCenterErrorHz` (state, not a verb) | Pan/waterfall alignment was eyeball-only; now it is a number |

**Reusable artifact:** `tools/tune_conformance.py` drives all four tuning modes
and asserts `slice == pan model == view == waterfall row` to 1 Hz after each.
Run it against any new backend before calling receive "done" — it is precisely
the check a new backend is most likely to fail, for the reason in gap 11.

Gotcha found while writing it: `SpectrumWidget` clamps the wheel to ±1 step per
event and debounces within 50 ms (#504/#556, inflated deltas on some desktops).
One synthetic event carrying five detents is **one** step, by design. Space
notches >50 ms apart or the test silently under-drives the control.

### Documentation drift cost real time

`slice mode` **already existed** but was absent from both the verb's own error
text and the docs table. Two separate detours into `dump_tree` and UI-clicking
resulted, on the belief that mode was undrivable.

**Requirement:** the verb's error text and the docs table must be generated
from one source. `gen_bridge_docs.py` tracks top-level verbs (53) but not
sub-actions, so action-level drift is invisible to CI.

### Still missing

1. **Read back what the DSP was actually configured with.** The recurring
   failure is model/DSP divergence (gaps 3, 5). `get_state` reports the *model*.
   An agent needs `get_state model=dsp backend=...` exposing the live WDSP
   config: in/dsp/out rates, block sizes, AGC mode + ceiling, filter edges.
   **This one verb would have caught gaps 3, 4 and 5 immediately.**
2. **A pitch/tone assertion primitive.** Every audio measurement this session
   was hand-rolled numpy over `capture_audio` JSON. A `capture_audio` mode
   returning dominant frequencies, peak/RMS, clipped-sample fraction and
   detected comb spacing would make audio regressions one call.
3. **Backend-vs-reference config diff.** See §9.
4. **Non-zero-sample assertion** in any data-plane health check.

---

## 9. Proposed automated bring-up sequence

Ordered by cost-to-run ascending, and deliberately front-loaded with the checks
that would have found this session's real bugs.

**Phase 0 — static, no hardware (seconds)**

1. **Reference-parameter diff.** For every vendor library we drive (WDSP
   first), diff our construction parameters against the reference clients'.
   Flag any parameter we *derive* that a reference *hardcodes* — that single
   rule catches `dsp_rate` (§2) and would have saved most of the session.
2. Assert `validateConfig()` covers every documented relationship, including
   `dsp_size`/`dsp_rate`.
3. Grep the new backend's call graph for Flex-only objects (`panStream()`,
   `connection()`, `m_flexBackend`) reachable without a null guard — catches
   gaps 1 and 9 statically.

**Phase 1 — against the simulator (a minute)**

4. Discovery → connect → assert `connected`.
5. Data-plane health: packet count, sequence continuity, **sample RMS/peak and
   non-zero fraction**, inter-arrival p50/p99/max.
6. Assert the DSP config read-back (§8.1) against expected values.
7. Drive every operator control through the bridge — mode, filter, AGC, tune —
   and after each, assert the **backend/DSP** state changed, not just the model.
   This is the dead-slider test, and it generalises to every future control.
7b. Run `tools/tune_conformance.py`: all four tuning modes, asserting
   `pan model == view == waterfall row` and that the slice lands where asked
   and stays inside the displayed span. Catches gaps 11 and 12, which are
   invisible to unit tests and nearly invisible by eye.
7c. Sweep any DSP stage whose SIGN or SCALE you are about to assume, against a
   SYNTHETIC source. `tests/hl2_shift_test.cpp` is the model: the same question
   measured against hpsdrsim was inconclusive because the simulator's DC offset
   translates with the shift and impersonates the signal. Reasoning about the
   direction got it backwards; one sweep settled it in seconds.
8. Audio assertions: inject a known tone, assert dominant frequency within
   tolerance, peak below full scale, no comb.

**Phase 2 — against hardware (minutes)**

9. Repeat 4–8 on the real radio.
10. Soak: run 10+ minutes, assert no drops, no growth in gap p99, no crash.
11. Operator sign-off on anything only ears or eyes can judge — audio quality,
    waterfall behaviour. Everything else should be machine-assertable.

**What must stay human:** whether audio *sounds* right. The pitch bug was
confirmed fixed by the operator's ears, and the AM filter bug surfaced from
"the audio sounds off". Step 8 narrows what needs listening; it does not
replace it.

---

## 10. Environment quick reference

```bash
# Build (8 cores)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build -j8

# Simulator
cd /Users/patj/aether/tools-external/pihpsdr && ./hpsdrsim -hermeslite2 -P1

# App with bridge, without grabbing a live radio
# App with bridge, on an ISOLATED settings profile so it cannot grab a radio
# and cannot touch the operator's real configuration.
#
# AETHER_AUTOMATION_NO_AUTOCONNECT DOES NOT EXIST -- it was removed
# application-wide (#4421/#4401) and nothing reads it. Autoconnect is governed
# by AutoConnectToLastRadio alone, so the way to not grab a radio is a profile
# that has never connected to one.
export T=/tmp/aether-hl2-test
mkdir -p $T/Library/Preferences/AetherSDR
cat > $T/Library/Preferences/AetherSDR/AetherSDR.settings <<'XML'
<?xml version="1.0" encoding="UTF-8"?>
<Settings>
  <AutoConnectToLastRadio>False</AutoConnectToLastRadio>
  <Hl2>{&quot;receiverCount&quot;:4,&quot;spanMhz&quot;:0.192}</Hl2>
</Settings>
XML
HOME=$T CFFIXED_USER_HOME=$T XDG_CONFIG_HOME=$T/.config \
QT_LOGGING_RULES="aether.hl2*=true" \
AETHER_AUTOMATION=1 AETHER_AUTOMATION_SOCKET=aethersdr-hl2 \
./build/AetherSDR.app/Contents/MacOS/AetherSDR
```

- The settings store is **XML**, not JSON, and a malformed file is refused with
  `AppSettings: cannot load ... Start tag expected.` followed by
  `refusing to save before a successful load` -- the app then runs on defaults,
  which looks like the settings simply not taking effect.
- `QT_LOGGING_RULES="aether.hl2*=true"` is needed to see the band-filter and
  receiver-count lines; they are `qCInfo` on a category that is off by default.
- Connecting by IP needs the radio TYPE picked as well as the address:
  `connectionManualModeButton` -> `connectionManualRadioType` ->
  `connectionManualIp` -> `connectionManualConnectButton`. The bridge's
  `connect ip` verb alone leaves the dialog waiting.
- `hpsdrsim` serves ONE client. A leftover app instance still holds it and the
  next connect times out with nothing in the log to say why -- check
  `pgrep -f MacOS/AetherSDR` before blaming the change.

- Launch the app as the **foreground process of a backgrounded shell**;
  launching it with `&` inside a foreground command gets it killed with the
  shell's process group.
- First WDSP channel open costs **~19 s** generating FFTW wisdom; every later
  open — any receiver, any sample rate — is **40–175 ms**. The planning cost is
  not a bug and cannot be optimised away, but it is now paid **off the GUI
  thread** and reported in the connect animation; see §22.
- The `tools/hl2/` Python spike defaults to broadcasting
  `255.255.255.255`, which fails on macOS with `OSError 65` when multiple
  interfaces are up. Use `--bcast <subnet>.255`. The in-app Qt sweep is fine.


---

## 11. Audit against the HL2 correctness oracles

Three oracles live at `/Users/patj/oracles/hl2/` — `hl2-oracle.md` plus addenda
on spectrum/audio and on AGC/filtering/multi-stream. They are independent of
this bring-up and worth reading before touching the backend again.

Their §0 precedence ladder is the discipline this session lacked:

> gateware Verilog > HL2 wiki > Quisk > openHPSDR protocol docs > anything else

and their central claim — *"many address bits have two meanings depending on a
mode flag; those dual-meaning fields are where implementations break"* — is
confirmed below, by us, exactly.

### 11.1 The one live defect: register `0x1C` is mislabeled

`MetisProtocol.h` defines `kC0AdcAssign = 0x1C` and `5c6c2fdd` documents it as
the receiver-to-ADC assignment bank. Since `C0 = ADDR << 1`, that is
**address `0x0e`**, and on the HL2 the oracle's §4 map gives it a completely
different meaning:

| Bits | HL2 meaning |
|---|---|
| `0x0e[15]` | Enable hardware-managed LNA gain for TX |
| `0x0e[14]` | LNA mode select for the TX value |
| `0x0e[13:8]` | LNA gain during TX |

ADC assignment at `0x0e` is the **generic openHPSDR** meaning. That is why
hpsdrsim needs the bank and why sending it was genuinely correct — but the
name and the commit message assert HL2 semantics that are wrong.

No live impact today: we send all zeros, so bit 15 stays 0 and hardware-managed
TX gain stays disabled, which is already the default. The hazard is latent and
specific — addendum 2 §A2 makes `0x0e` the register behind the T/R gain switch,
the mechanism Quisk (the designer's own client) uses, and the one PureSignal
needs for an unclipped feedback path. The moment TX work starts, this round
robin would be zeroing it every other frame.

**Do not delete the write.** Rename it, record the dual meaning in a comment,
and gate it before TX lands.

### 11.2 Pipeline reset — a gap the decoupling created

Addendum 2 §B2: the CIC/FIR decimation chain carries state, and a large
frequency jump smears a transient across the change. `0x39[7:4] = 0x8` resets
the pipeline; `0x9` also phase-aligns the NCOs.

We never issue it — and `a1cbe154` made this newly relevant, because
`setSliceFrequency` and `setPanCenter` now move the NCO on band-scale jumps,
which is precisely the case named. Small fix, directly on the path just
touched. Use `0x9` if coherent multi-RX ever lands.

### 11.3 Watchdog versus our threading model

We default the watchdog ENABLED, which the oracle recommends for anything that
can transmit. But §2 also requires the command cadence to live on a thread that
cannot be starved by rendering — and `Hl2Backend.h` states plainly that Phase 1b
runs the wire AND the DSP on the backend's own (GUI) thread.

A GUI stall therefore stops EP2 and the radio stops streaming on its own. We
measured a 21–82 second main-thread stall on first connect (FFTW wisdom) — see
§22, which fixed that one. The wire and the DSP now live on their own I/O
thread, so the class this warned about is narrower than it was. Two paths still
block the GUI thread, both in §22.4: the **span change**, which rebuilds every
receiver, and **backend teardown**, which waits out an in-flight DSP build.

### 11.4 Absent subsystems, in rough value order

| Missing | Why it matters |
|---|---|
| RQST/ACK state machine (§5) | Gate for everything below it. Single outstanding request, no transaction id, echo-matched. Do NOT model as RPC |
| ADC overload bit + clip counter (§6) | Addendum 2 §A3: the CORRECT driver for any gain decision. Audio level in one slice says nothing about what saturates a converter seeing 0–38.4 MHz |
| Discovery telemetry (§1) | Temperature, power, clip count, PTT are pollable WITHOUT a stream — cheapest possible first increment, and a diagnostic when the stream itself is broken |
| ~~Receiver count at discovery `0x13`~~ | **DONE — §19.** Read and clamped against; skimmer variants 9–12 with NO transmit are still untested |
| TX FIFO depth (§6) | "The most important number in the protocol." TX pacing must servo against it, not a host timer — clock domains drift |
| Wideband bandscope (§7) | Unimplemented by piHPSDR (dead code) and declined by SDR Console. A differentiation opportunity, with the 4-vs-32 packets-per-block trap already documented |

### 11.5 Smaller corrections

- **Normalization**: we use `1 << 23` (8388608); the oracle specifies
  **8388607** (2²³−1) for dBFS parity with piHPSDR. Numerically irrelevant,
  but parity is the whole point of matching a reference.
- **LNA ↔ dB reference** (addendum 2 §A3): every LNA change shifts the absolute
  reference, so the panadapter trace jumps and the waterfall shows a band users
  read as a real event. Keep LNA value, calibration offset and AGC threshold in
  ONE per-slice object. Worth doing before an RF AGC exists — manual gain
  changes have the same problem.

### 11.6 What the oracles did not cover — now addendum 3 (see §12)

The three defects that cost the most this session were all WDSP *channel
geometry*, and none appear in the oracles (addendum 2 §A4 covers AGC internals
only):

1. `dsp_rate` is **always 48000**, independent of input and output rate —
   Thetis `cmaster.c`, pihpsdr `receiver.c`. See §2.
2. `RXASetPassband` vs `SetRXABandpassFreqs`: the latter leaves the NBP stage
   untouched, so NOTHING selects a sideband. Gap 13.
3. HPSDR wire IQ handedness is **opposite** to WDSP's, so USB and LSB come out
   swapped. Gap 14 — and it hid behind gap 13.

All three are only visible by reading the reference clients, which is exactly
the oracles' own §0 discipline. **Addendum 3 now covers this ground** and
independently confirms items 2 and 3 — see §12.

### 11.7 Open items (superseded by §13)

1. Rename `kC0AdcAssign`, document the `0x0e` dual meaning (11.1).
2. Issue a pipeline reset after an NCO move (11.2).
3. Read receiver count from discovery `0x13`; stop hardcoding `maxSlices`.
4. RQST/ACK + the ADC overload/clip telemetry it unlocks.
5. Move the HL2 DSP off the GUI thread (11.3).
6. AM passband still inherits SSB width on Flex-shaped mode changes elsewhere —
   see gap 15's fix for the pattern.


---

## 12. Audit against addendum 3 (WDSP channel setup)

`hl2-oracle-addendum-wdsp-channel-setup.md`. This is the chapter that covers
what §11.6 said was missing, and it independently confirms two of the three
defects that cost this session the most.

### 12.1 Confirmed by the oracle

- **`RXASetPassband` supersedes `SetRXABandpassFreqs`** — §7 states the latter
  is *deprecated* in favour of the former. Independent confirmation of
  `86a3d27b`, which we arrived at by reading RXA.c.
- **`dsp_rate` is 48000, fixed** — §2 and the §10 reference table. Confirms
  `74f10f53`.
- **First-run FFTW planning is slow BY DESIGN** (§9). Our measured ~19-second
  first connect is expected behaviour, not a performance bug. The oracle's
  prescription is a progress indicator, not optimisation — which is what §22
  built. What the oracle does NOT excuse, and what was the actual defect, is
  spending those 19 seconds with the GUI thread blocked and then discarding the
  result at exit.

### 12.2 Licensing — resolved, we are fine

§0 flags WDSP as GPL-2.0 and says to settle this *before* building the DSP
layer. Checked: the WDSP sources carry **"either version 2 of the License, or
(at your option) any later version"** — 70 of 74 `.c` files. GPL-2-or-later
upgrades cleanly into AetherSDR's GPL-3, so linking is fine. The four files
without the boilerplate are worth a spot check before any redistribution
question, but the headline is settled.

### 12.3 New defects found

**Mute ramps are all zero.** `WdspChannel::open()` passes
`0.0, 0.0, 0.0, 0.0` for `tdelayup / tslewup / tdelaydown / tslewdown`. Both
references use `0.010, 0.025, 0.000, 0.010`. §2 calls these the anti-click
mechanism and "the difference between clean and clicky T/R... easy to leave at
defaults and never discover" — we did exactly that. Trivial fix, and it matters
the moment anything mutes or starts a channel.

**The S-meter measures the wrong thing.** `Hl2RxDsp::processIqBlock` computes
`20*log10(rms)` of `m_left` — the *post-AGC* audio. Holding that level constant
is precisely what AGC is for, so with AGC engaged our S-meter barely moves
regardless of signal strength. WDSP already provides the real thing:

```c
double GetRXAMeter(int channel, int mt);   // RXA_S_PK, RXA_S_AV
```

This is a defect that looks like it works — the meter deflects, just not in
proportion to anything. Worth fixing before anyone calibrates against it.

**`RXASetNC` and `RXASetMP` are never called.** Filter tap count and
minimum-phase mode — the selectivity-versus-latency controls. piHPSDR sets both
right after `OpenChannel` (`RXASetNC(id, fft_size)`, `RXASetMP(id,
low_latency)`); we take WDSP's defaults silently. §7 notes these matter a lot
to CW operators.

**`SetChannelState` is never used.** We pass `state = 1` at open and never stop
the channel. §2 is explicit that `SetChannelState` is the T/R call (it applies
the ramps) and `CloseChannel` is for teardown only — "conflating them means
either clicks (closing) or leaks (never closing)."

### 12.4 Divergences that are defensible, but should be deliberate

**Output rate.** piHPSDR fixes `dsp_rate` AND `output_rate` at 48000 and varies
only the input rate; §2 calls that "the simple, correct default." We use
`output_samplerate = 24000` (AudioEngine's native rate) to avoid a resample.
That is legitimate — the parameter exists to be set — but it IS a divergence
from the reference, in exactly the area that produced our worst bug. Keep it
labelled as a deliberate choice, not an accident.

**Rate changes.** §2 says to use `SetAllRates`, never the individual setters,
because stepping through them leaves the channel in intermediate inconsistent
states that WDSP will happily process audio in. We use neither: `configure()`
rebuilds the channel outright. That dodges the hazard completely but re-plans
FFTW and discards channel state, so `SetAllRates` is the lighter correct path
if rate changes ever become frequent.

**Analyzer.** We run our own `Hl2Spectrum` FFT rather than WDSP's analyzer.
§4's recommendation for our architecture is exactly this (its "option 2"), so
the choice is right — but note WDSP's analyzer returns **pixels, not bins**, and
carries detector and averaging modes that §4 says are "why WDSP panadapters look
smooth." If ours ever looks noisy by comparison, the lever is a detector /
averaging mode, **not a bigger FFT**.

### 12.5 Design constraints to absorb before multi-slice

- **Three index spaces** (§3): hardware DDC index, WDSP channel index, UI
  receiver number — plus analyzer IDs in a fourth. Keep
  `{ ddcIndex, dspChannel, analyzerId, uiNumber }` per slice and never derive
  one from another arithmetically; PureSignal and diversity break the
  arithmetic. Trivial today at one slice, which is exactly when to put it in.
- **Diversity is a PRE-channel combiner** (§6). `divEXT` takes two DDC streams
  and produces one, which then feeds a single WDSP channel — that is why
  piHPSDR passes four sample arrays into what looks like one receiver. Modelling
  diversity as "a slice with two inputs" fights the DSP layer.
- **Noise blankers are also outside the channel** (§6): `xanbEXT` / `nobEXT`
  operate on raw IQ before `fexchange`, not as RXA blocks.
- **Two ADC level readings that disagree by design** (§7): WDSP's
  `RXA_ADC_PK`/`RXA_ADC_AV` measure the post-DDC *slice*; the HL2's clip counter
  and overload bit measure the pre-DDC *full spectrum*. You can be far from
  clipping in a 48 kHz slice while a broadcast station saturates the converter.
  Show both, labelled distinctly — §7 calls this the single most useful
  diagnostic pairing on the HL2, and it ties §11.4's missing telemetry to the
  bandscope.

### 12.6 Revised next-session list (superseded by §13)

Cheap and high-value first:

1. Mute ramps → `0.010, 0.025, 0.000, 0.010` (12.3). One line.
2. S-meter → `GetRXAMeter(RXA_S_PK)` instead of post-AGC audio RMS (12.3).
3. Rename `kC0AdcAssign`, document the `0x0e` dual meaning (11.1).
4. Pipeline reset after an NCO move (11.2).
5. `RXASetNC` / `RXASetMP` (12.3).
6. Receiver count from discovery `0x13`; stop hardcoding `maxSlices` (11.4).
7. RQST/ACK, then ADC overload + clip telemetry, paired with WDSP's own ADC
   meter (11.4, 12.5).
8. Move the HL2 DSP off the GUI thread — watchdog correctness (11.3).


---

## 13. Consolidated backlog

Everything still open, across all four oracles and our own gap list. This is the
canonical to-do table; §11.7 and §12.6 are partial views kept for provenance.

Effort is rough: **XS** under an hour, **S** a session, **M** a few sessions,
**L** a design conversation first.

### Tier 1 — cheap, high value, do first

| # | Item | Source | Why it matters | Effort |
|---|---|---|---|---|
| 1 | Mute ramps `0.010/0.025/0.000/0.010` instead of all zeros | A3 §2 | The anti-click mechanism; invisible until you are debugging clicks | XS |
| 2 | S-meter from `GetRXAMeter(RXA_S_PK)`, not post-AGC audio RMS | A3 §7 | Current meter is held flat by the AGC — it deflects but tracks nothing | XS |
| 3 | Rename `kC0AdcAssign`; document the `0x0e` dual meaning | O §4 | It is TX LNA gain on HL2. Latent TX/PureSignal hazard | XS |
| 4 | Pipeline reset `0x39[7:4]=0x8` after an NCO move | A2 §B2 | Decimation state smears a transient across band-scale jumps — which `a1cbe154` made routine | XS |
| 5 | Normalize by `2^23-1`, not `2^23` | A1 §A2 | dBFS parity with piHPSDR. Numerically trivial, but parity is the point | XS |
| 6 | `RXASetNC` / `RXASetMP` after `OpenChannel` | A3 §7 | Selectivity vs latency; matters to CW operators. We silently take defaults | XS |
| 6a | Rate-limit the ADC-overload warning | §15.7 | Edge-gated, but the value chatters: **~133 warnings/second** on MW, which flushes the log ring and hides everything else | XS |

### Tier 2 — correctness gaps

| # | Item | Source | Why it matters | Effort |
|---|---|---|---|---|
| 7 | ~~Read receiver count from discovery `0x13`~~ | O §1 | **DONE — §19.** `maxSlices`/`maxPanadapters` report the RUNNING count: requested, clamped by discovery `0x13`, clamped again by the link budget | S |
| ~~8~~ | ~~Move HL2 wire + DSP off the GUI thread~~ **DONE** | O §2 | `Hl2Backend` runs `MetisClient` and both DSP chains on a dedicated `hl2-io` thread. Note the consequence: EP2 pacing, EP6 ingest, WDSP and the panadapter FFT now share ONE thread, so per-sample cost there scales with the span (§15.2) | — |
| 9 | `SetChannelState` for start/stop; `CloseChannel` only for teardown | A3 §2 | Conflating them gives clicks or leaks. Needed before T/R | S |
| 10 | RADE null-deref at `MainWindow_DigitalModes.cpp:461` | ours, gap 9 | Same shape as the DAX crash; will kill HL2 the moment RADE starts | XS |
| 11 | ~~`AETHER_AUTOMATION_NO_AUTOCONNECT` not honoured~~ | ours, gap 10 | **Withdrawn.** The variable was removed application-wide; nothing reads it. See gap 10 and the §10 recipe | — |
| 12 | One dB-reference object per slice (LNA + calibration + AGC threshold) | A2 §A3 | Every LNA change shifts the absolute reference; the trace jumps and users read it as a real event | S |
| ~~12a~~ | ~~Seam verb for RF/LNA gain~~ **DONE** | §15.7 | `IRadioBackend::setPanRfGain` carries the ANT panel's RF Gain slider to the AD9866. Measured on hardware: a commanded 20 dB step moved the wire noise floor 19.8 dB | — |
| 12b | Automation verbs `pan span`, `pan rate`, `perf` | §15.7 | Proving §15 needed span driven by repeated `pan_zoom_in`, the FPS slider reached through a menu, and frame rates scraped from a log file the chatter in 6a nearly buried | S |

### Tier 3 — absent subsystems, in dependency order

| # | Item | Source | Why it matters | Effort |
|---|---|---|---|---|
| 13 | RQST/ACK state machine | O §5 | Gate for everything below. Single outstanding request, echo-matched, no transaction id. **Do not model as RPC** | M |
| 14 | ADC overload bit + clip counter | O §6, A2 §A3 | The *correct* driver for gain decisions — audio level in one slice says nothing about what saturates a converter seeing 0–38.4 MHz | S |
| 15 | Discovery-reply telemetry (temp, power, PTT, clip) | O §1 | Pollable **without a stream** — cheapest first increment, and a diagnostic when the stream is broken | S |
| 16 | Pair WDSP `RXA_ADC_PK` with the hardware clip indicator | A3 §7 | Post-DDC slice vs pre-DDC full spectrum. They disagree by design; A3 calls this the most useful diagnostic pairing on the HL2 | S |
| 17 | TX IQ FIFO depth + servo | O §6, A1 §B3 | "The most important number in the protocol." TX pacing must servo against it, not a host timer — clock domains drift | M |
| 18 | Wideband bandscope (endpoint `0x04`) | O §7, A1 §A1 | Unimplemented by piHPSDR (dead code) and declined by SDR Console — a differentiation opportunity. **4 packets/block on HL2, not 32** | M |
| ~~19~~ | ~~Filter board band switching (J16 / I2C `0x20`)~~ **DONE** — PA bias + config EEPROM still open | O §8 | Band filters auto-select from the slice frequency (`Hl2Backend::applyBandFilter`). PA bias and the config EEPROM are untouched and still want the RQST/ACK path | — |
| 20 | ~~Multi-slice: index-space mapping object~~ | A3 §3 | **DONE — §19.** `Hl2Receivers.h`. The WDSP channel really is not the DDC index: ids come from a shared 32-slot pool, so after a TX channel has come and gone receiver 0 is routinely not channel 0 | S |
| 21 | Diversity as a **pre-channel combiner** | A3 §6 | `divEXT` takes two DDC streams and yields one. Modelling it as a two-input slice fights the DSP layer | M |
| 22 | Hardware-managed T/R LNA gain (`0x0e[15]`) | A2 §A2 | Quisk uses it; lower latency than any host round trip; PureSignal needs an unclipped feedback path | S |
| 23 | PureSignal | O §11, A1 §B6 | Needs everything above. Consumes 4 RX (2 feedback), halving the slice budget | L |

### Tier 5 — the RX audio bus (added after the §18 audit)

Everything here is one root cause: features bind to a *transport*, not to the
radio. See §18 for the full audit and the proposed seam.

| # | Item | Source | Why it matters | Effort |
|---|---|---|---|---|
| 24 | RADE / DAX-bridge bare `panStream()` deref | §18.3, gap 18 | **SIGSEGV on mode change**, same shape as gap 1. Do this before any of the below | XS |
| ~~25~~ | ~~WSPR beacon on a host-modulating backend~~ **DONE** | §18.4 | The audio route already existed (#4471); only the DAX-borrow guard was in the way. First external-oracle TX instrument we have | — |
| ~~26~~ | ~~Unified RX-audio seam~~ **PARTLY DONE** | §18.5, §18.8 | `rxDemodAudioReady` landed with CW, RTTY and the QSO recorder RX tap as its consumers. The `sliceId` argument and a `Wideband` tap are still open — nothing needs them yet | S |
| 27 | AetherClock off DAX-channel identity onto slice identity | §18.6, gap 17 | WWV/WWVB decode. Depends on 26 | S |
| 28 | `hasDaxAudio` / `hasDaxIq` / tap kinds / `rxAudioSampleRateHz` capabilities | §18.5 | Lets features decline honestly instead of binding to nothing. Depends on 26 | S |
| 29 | Retire the `kiwi : "flex"` source-tag ternary | §18.2, gap 19 | Blocks a third concurrent family; `AsrTapPolicy` cannot disambiguate. Fold into 26 | XS |
| 30 | Measure whether TCI's post-AGC feed costs WSJT-X decodes | §18.5 | Decides whether a `Wideband` tap is worth building at all. **Measure before building** | S |

### Tier 4 — deliberate divergences, do NOT "fix" by reflex

| Divergence | Reference does | We do | Why ours is defensible |
|---|---|---|---|
| `output_samplerate` | 48000 | 24000 | AudioEngine's native rate; avoids a resample. Legitimate, but it IS a divergence in the area that produced our worst bug — keep it labelled |
| Rate change | `SetAllRates` | Rebuild the channel | Dodges the intermediate-inconsistent-state hazard entirely. Heavier, but NOT because of FFTW — a rebuild at a new rate re-plans almost nothing (§22.4). It is heavier because it is a close+open per receiver, and it still blocks the GUI thread |
| Spectrum | WDSP analyzer (returns pixels) | Own `Hl2Spectrum` FFT | A3 §4 recommends exactly this for our architecture. **If it ever looks noisy, the lever is a detector/averaging mode, not a bigger FFT** |
| FFTW wisdom | `WDSPwisdom(dir)` | Own `fftw_import_wisdom_from_filename` + eager export | `WDSPwisdom` is Windows-console-only. First-run slowness is expected; the fix was a progress indicator plus getting the wisdom to actually persist (§22) |

### Settled — no action

- **WDSP licensing.** GPL-2-**or-later** in 70 of 74 `.c` files, so it upgrades
  into our GPL-3. Linking is fine. (Spot-check the four before any
  redistribution question.)
- **~19-second first connect.** Expected FFTW planning, per A3 §9. The
  planning itself stays; what was fixed is that it froze the whole UI and was
  then thrown away on exit. §22.
- **Alex manual mode** (`0x09[22]`). Not implemented in gateware — do not build
  UI for it.

Legend: **O** = `hl2-oracle.md`, **A1** = spectrum/audio addendum,
**A2** = AGC/filtering addendum, **A3** = WDSP channel setup addendum.

---

## 14. Transmit bring-up

RX bring-up was mostly "the audio sounds wrong, find out why". TX was different
in kind: **every failure was silent**. A transmitter that is misconfigured emits
nothing, or emits something wrong, and neither announces itself. Nothing in the
app said "you are not transmitting" — the UI keyed, the meters sat still, and
the only evidence was the radio's own forward-power counter reading zero.

### 14.1 Four defects between "correct IQ on the wire" and "RF out of the socket"

Each of these, on its own, produced a perfectly correct-looking keyed
transmission with **zero** forward power. They had to be found in series.

| # | Defect | Why it was invisible |
|---|---|---|
| 1 | `onTxAudioReady` returns early without a Flex TX stream id | For Flex that id *is* the destination. Killed the mic **and** the TONE button, because the tone is injected *inside* that callback |
| 2 | Mic capture never started — `startTxStream()` is called only from Flex DAX signals gated on `mic_selection=PC` | No HL2 session emits those, so `QAudioSource` never opened |
| 3 | Onboard PA never enabled (`0x09[19]`, C2 bit 3) | Without it the only output is the AD9866's DAC level — milliwatts |
| 4 | RF power never applied on connect | `rfPowerChanged` is edge-triggered; an untouched control left drive 0, which also leaves the PA off |

**The reusable lesson:** on a transmit path, "the command was accepted" proves
nothing. The only trustworthy signals are the radio's own telemetry (forward
power) and physics (PA temperature rising). Both were needed here.

### 14.2 The modulator bug the test caught

The first SSB modulator used a textbook Hilbert transformer, `2/(pi*k)` on odd
taps. **That filter is all-pass in magnitude.** It passed out-of-band audio at
full amplitude with a 90-degree shift while the I path correctly rejected it, so
energy above the passband arrived in Q *alone* — a real signal — and came out
**double sideband**.

Measured: a 5 kHz tone against a 2700 Hz filter appeared at both +5 kHz and
−5 kHz, only 6 dB down. Splatter outside our own passband, radiated, and
**invisible to any loopback that only checks the wanted sideband**.

The fix derives both filters from one analytic prototype,
`ha[k] = (exp(j·2π·hi·k) − exp(j·2π·lo·k)) / (j·2π·k)`, so I and Q share a
passband by construction and their group delay matches for free. Rejection went
from 6 dB to 100 dB; opposite-sideband suppression is 85 dB.

### 14.3 Protocol facts established

| Fact | Detail |
|---|---|
| PA enable | `0x09[19]` = C2 bit 3. **Mandatory** for useful output |
| TX NCO | `0x01`, a **separate oscillator** from the RX DDC — it does not follow the receiver. Unset, a key transmits at DC |
| Host→radio samples | **16-bit** I + 16-bit Q, unlike EP6's 24-bit |
| EADDR trap | The first 32-bit word after each frame's C&C is the extended-address register, **not** headphone audio. A memcpy'd Hermes TX layout corrupts it |
| MOX | C0 bit 0 of **every** frame, not a register. Both sub-frames must carry it or keying is cadence-dependent |
| EP6 response C0 | `ACK` (bit 7) **changes how the rest of C0 decodes**: ACK=0 → RADDR in `[6:3]` (4 bits) + Dot/Dash/PTT; ACK=1 → RADDR in `[6:1]` (6 bits) |
| TX inhibit | **Active low** — the bit is SET when transmit is permitted |
| SWR | Counts are **voltage**-proportional → `(Vf+Vr)/(Vf−Vr)`, **no square root**. Validated by reading 1.0:1 into a dummy load |
| **Wire handedness** | The wire is the **conjugate** of the standard analytic convention. RX compensates with `-imag()` before WDSP; **TX must conjugate too**. Omitting it transmits every signal on the wrong sideband — see §14.6 |
| PA enable vs handedness | A tune carrier sits at **zero offset**, where handedness has no effect. TUNE therefore works even when the sideband convention is wrong, and is useless as evidence for it |

### 14.4 Seam gaps this phase exposed

Two verbs existed and were wired to nothing at all:

- **`IRadioBackend::meterUpdate`** — `meterDefined`/`meterRemoved` were connected
  in `RadioModel`; values were not, because Flex streams them over VITA-49. Every
  meter reading this backend computed was discarded. The S-meter had been correct
  for days and had never once been visible.
- **`IRadioBackend::setKeying`** — no callers anywhere. `RadioModel::setTransmit`
  ended in `sendCmd("xmit N")`, a raw Flex text command, so **no non-Flex backend
  could ever be keyed**.

The pattern: a seam verb with no consumer looks identical to a working one from
below. Grep for callers of every verb a new backend implements, before trusting
that implementing it does anything.

**Closed on hardware, 2026-08-10.** `radiocert meters` against the live radio
reports all eight defined meters `everFed: true` — `SLC:LEVEL`, `TX:MICPEAK`,
`TX:SWR`, `TX:FWDPWR`, `TX:REFPWR`, `TX:ALC`, `TX:COMPPEAK` and `RAD:PATEMP`.
The seam this section opened is fed end to end, and four of those meters were
still described in `docs/radio-certification.md` as computed-and-discarded when
the run was made. The measurements are in that file under
*Certified by effect, 2026-08-10 (Hermes-Lite 2)*.

### 14.5 Testing UX: exercise BOTH RadioModel and TransmitModel

**The automation bridge is not a test of the user interface.** The two drive
different models, and a verb that reaches the radio proves nothing about the
button that is supposed to.

| Path | Route | Reaches the seam? |
|---|---|---|
| Bridge `key ptt` | `RadioModel::setTransmit()` → `IRadioBackend::setKeying()` | yes |
| **MOX button** | `TransmitModel::requestPttOn()` → `setMox()` → `commandReady("xmit 1")` | **no** — Flex TCP text |
| **TUNE button** | `TransmitModel::startTune()` → `commandReady("transmit tune 1")` | **no** — same |
| Bridge `tune` verb | `SliceModel::setFrequency()` | n/a |

This produced a genuinely absurd state: hardware testing showed **1080 counts of
forward power and a PA warming to 34 °C**, and the operator pressing MOX
transmitted nothing. Every automated check passed. The operator's first attempt
failed.

**The rule:** when verifying anything user-facing — buttons, meters, keying,
tune — exercise **both** models:

- `RadioModel` is what the bridge and other clients drive.
- `TransmitModel` is what the GUI controls drive, and it emits **Flex command
  strings** (`xmit`, `transmit tune`, `transmit set rfpower=`) that reach a
  backend with no command channel *not at all*.

Any TransmitModel action that must work on a non-Flex backend needs a **typed
signal** routed through the seam, gated to non-Flex families so Flex does not
receive the command twice. `rfPowerChanged`, `moxCommandIssued` and
`tuneCommandIssued` are the existing examples; the next one added should follow
that shape.

Practical check before claiming a control works: trace the widget's `connect()`
to the model method it calls, and confirm that method reaches
`IRadioBackend`. If it only emits `commandReady`, it is Flex-only.

### 14.6 The wrong-sideband bug, and why nothing internal could find it

Transmit went out on the WRONG SIDEBAND for the entire bring-up. The HPSDR wire
order has the opposite handedness to the standard analytic convention; the
receive path appeared to compensate (conjugating with `-imag()` before WDSP, the
fix filed as "USB and LSB are swapped"), and transmit never got the same
correction.

> **Correction (see §15).** That receive-side `-imag()` was itself wrong. It
> inverted every demodulated sideband, and a second error — feeding the
> panadapter the raw wire — hid it. The reasoning recorded here ("RX already
> compensates, TX needs the same") was right about the wire's handedness and
> wrong about which stage should carry the correction. **Do not use this
> paragraph as the model for a new backend; use §15.**

**Every internal check agreed with the bug**, because the panadapter reads the
same wire order as the transmitter. Our display and our transmission were
consistent with each other while both disagreed with the rest of the band:

| Check | Result | Verdict |
|---|---|---|
| `hl2_txdsp_test` sideband assertion | 85 dB suppression, "correct" side | passed, asserting the TEXTBOOK convention |
| `hl2_tx_loopback_test` through hpsdrsim | tone at the expected bin | passed, measuring the sim's feedback in wire order |
| Panadapter during TX, live radio | clean single sideband, correct side of centre | looked perfect |
| Forward power, USB vs LSB at 14.200 | 3875 vs 3876 | identical, both "working" |
| TX FIFO depth | stable 27–31, no under/overflow | refuted the starvation theory |

It was found by an operator with a second receiver: *"I heard the LSB side of
AetherSDR on the USB side of the Yaesu."*

**The generalisable lesson.** A convention error is invisible to any test that
shares the convention. Self-consistency is not correctness, and the more
internal instruments agree, the more confident the wrong answer looks. For
anything that leaves the machine — RF, a wire format, a file another program
reads — at least one check must come from **outside the system**: a second
receiver, a different decoder, an independent implementation. Measuring harder
inside the loop cannot substitute.

Related: this is why TUNE always worked and voice never did. A tune carrier sits
at ZERO offset, where handedness has no effect — the one signal that could not
have exposed the bug was the one that always looked fine.

**Why the loopback could not have caught it, and what changed.** The second row
above is worth being precise about. `hl2_tx_loopback_test` measures a loop that
conjugates twice — `Hl2TxDsp` for the wire on the way out, `Hl2RxDsp` for the
panadapter on the way back — so a handedness error present at BOTH ends cancels
exactly. Whichever sign that test asserted, it was blind to a global flip; it
was another instrument sharing the convention. The test now takes an
**independent bearing on the receive end first**: hpsdrsim generates its own
scene tones at 800 Hz and 4 kHz with a fixed handedness, and those reach the
spectrum without passing through the transmitter. Anchoring on them pins the
receive end on its own, which is what makes the transmit assertions load-bearing
instead of self-satisfying. Verified by injecting each fault separately —
mirroring the display trips the anchor and names the receive end; flipping only
the modulator leaves the anchor green and trips just the mic-path checks.

It is still inside the machine, and the lesson above still stands: the operator's
second receiver remains the only check that comes from outside it.

### 14.7 Process failures worth not repeating

- **`0x39` wedged the radio.** The filter-pipeline reset was validated with 7
  writes spaced ~2 s apart and shipped. A pan drag issues centre commands every
  33 ms, so it fired ~30 resets/second and the board halted its stream and
  stopped answering discovery until power-cycled. *Validate at the rate the UI
  actually produces, not at the rate that is convenient to test.*
- **Documenting a risk is not retiring it.** That same commit stated plainly
  that the zero-fields assumption had never been checked against the gateware
  RTL — and shipped anyway.
- **Hz vs MHz.** The automation `tune` verb takes **MHz**. The harness passed Hz
  for most of a session; every call returned `ok: true` and the model faithfully
  stored 10,000,000 MHz. It invalidated several "tested on the live radio"
  claims, and only surfaced because a screenshot's axis looked wrong. *A verb
  that accepts a wrong-unit value without complaint is a silent failure.*
- **Trusted self-consistent internal instruments.** See §14.6 — the transmitter
  was on the wrong sideband while every test, meter and display agreed it was
  right, because they all shared the convention that was wrong.
- **Verified the layer that could be scripted, not the layer the operator
  presses.** Twice: once as the Hz-for-MHz harness bug, once as MOX keying
  through a model the bridge never touches. See §14.5 — this is the single most
  expensive recurring mistake of the bring-up.
- **Test capture artifacts produced three wrong conclusions.** Block-buffered
  simulator stdout, `script` writing past a truncation, and reading a log delta
  before the pty flushed each looked like "the feature does not work". Add a
  settle delay and read by byte offset before concluding anything from a log.
- **Prefer measurable correctness over canonical implementation.** WDSP's TXA
  works (`wdsp_channel_test` proves it), but driven from this backend's config it
  returned underruns and zeros. Chasing an undocumented init sequence for a path
  that keys a transmitter is a bad trade against fifty lines whose correctness is
  a number a test prints.

### 14.8 Still open

- **Absolute watts.** Counts are uncalibrated; oracle §6 forbids presenting them
  as watts. Needs a per-unit calibration curve.
- **FIFO-servoed TX pacing.** The decoded depth follows hpsdrsim's layout, and
  the oracle's §6 table disagrees in a way that cannot both be right. **The
  gateware RTL has not been consulted.** Nothing may build pacing on that field
  until it has been.
- **PA temperature formula** is the HL2 wiki's, unverified against a reference.
  29.5 °C idle → 34 °C under load is plausible, not calibrated.
- **`0x0e` T/R gain switch** and PureSignal's feedback path.
- **Reference-oscillator calibration.** Measured **~200 Hz high at 10 MHz**
  (≈20 ppm), consistently, in both sideband directions — i.e. the radio receives
  above where it claims. Harmless for FT8 and unrelated to handedness (§16), but
  it is a real frequency error with no calibration knob. A per-unit ppm trim
  belongs alongside the power-calibration curve above.
- **`RTTY` has no HL2 mode mapping.** It is advertised in the TCI
  `modulations_list` and falls through `modeFromString` to the USB fallback —
  the same class of silent defect as the `CW` gap in §16.7. Left unmapped rather
  than guessed at; WDSP has no RTTY mode, so it needs a deliberate decision.

### 14.9 The voice chain: what persists per radio, and what does not

The Flex-shaped voice controls — the 8-band EQ applet, PROC with its
NOR/DX/DX+ level, and the Phone applet's TX low-cut/high-cut — all emit command
plane verbs that reach nothing here. They are wired instead to the DSP this host
already runs on transmit audio: `ClientEq` and `ClientComp`, which `AudioEngine`
applies before `submitTxAudio` ever sees a sample.

**Two of those three persist in different scopes, and that is deliberate.** It
looks like an inconsistency, so it is written down here rather than left for
someone to "fix".

| Control | Backing object | Persistence scope |
|---|---|---|
| TX low-cut / high-cut | `Hl2TxDsp` passband | **Per radio**, `ext.txSetpoints` in the RFC #4603 operating-state document |
| PROC + NOR/DX/DX+ | `ClientComp` (shared) | **Per client** — the app-global audio-chain settings |
| 8-band EQ | `ClientEq` (shared) | **Per client** — same |

The TX cut points are per radio because they are a *radio* setpoint in exactly
the sense the rest of `OperatingState` is: they live in the backend, they are
pushed at the modulator, and nothing but this client remembers them. Leaving
them in bare members while frequency, mode, passband, span, per-band LNA and
drive all restored around them was the asymmetry the per-domain design exists to
prevent.

PROC and the EQ are per client because the objects underneath them are the
operator's **audio chain**, not the radio's state — the same `ClientEq` and
`ClientComp` the Aetherial strip edits, with their own app-global persistence
that predates all of this. Two HL2s therefore share one PROC configuration while
each keeps its own drive and LNA maps. That is the intended reading: an
operator's voice processing is a property of their microphone, their room and
their voice, none of which change when they switch radios.

The consequence to know before changing any of it: because the EQ applet and
PROC write into the *same* `ClientEq`/`ClientComp` as the strip, the two
surfaces are two views of one object. Moving a graphic-EQ slider replaces the
strip's band layout in slots 0..7, and toggling the strip's compressor lights
PROC. Only a PROC move the operator actually made writes the NOR/DX/DX+ preset
over the strip's compressor settings — that gate is
`TransmitModel::speechProcessorCommandIssued`, and keying it off the broader
`micStateChanged` instead is a bug that overwrites the operator's own work at
the moment they enable their own compressor.

The TX cut points are **flat**, not per band or per mode, unlike the drive and
LNA maps beside them in the same extension document. There is one pair of
sliders in the Phone applet; persisting per band would make them move on their
own at every band change, which is the same surprise as a mode change silently
replacing the passband — the thing `m_txFilterFromOperator` exists to prevent.

The cut points apply to **SSB voice only** — `effectiveTxPassband()` — so a
"shape my voice" slider cannot set the CW keying envelope's bandwidth or widen a
digital mode past what the far-end decoder expects. The transmitter's actual
passband is echoed upward as a `TransmitDelta` on every mode change,
transmit-slice move and connect (`pushTxPassband()`), so the applet's readout
tracks what the modulator is running instead of what was last asked for. The one
push that does not echo is `setTxFilter()` itself: outside SSB, snapping the
readout back to the mode default would make the operator's next nudge compute
from that default and quietly overwrite the eSSB pair they had just set.

### 14.10 Two surfaces, one object: who may write

`ClientEq` and `ClientComp` **persist**; `EqualizerModel` and `TransmitModel` do
not. That asymmetry is the whole reason `core/HostVoiceChainPolicy.h` exists, and
both ways of getting it wrong shipped as far as review:

- A connect edge that re-pushes the Flex-shaped controls unconditionally is
  pushing their *construction defaults* — eight bands at 0 dB, every enable
  false — because on this radio there is no `eq`/`transmit` status to populate
  them. That lands on the operator's saved Aetherial strip layout, at every
  connect, for someone who never opened the applet.
- An unwind that fires on the family check alone fires on a plain **Flex**
  connect too (`hostModulates` is false there), switching off that operator's own
  RX EQ, TX EQ and compressor on a session with no HL2 in it.

So both turn on one bit: has the operator actually moved a Flex-shaped control in
this process? Nothing else distinguishes "re-apply their own choice" from
"overwrite work this code never made".

---

## 15. Panadapter span and display rate

Two defects with one root: **the panadapter's span and its frame rate were both
consequences of the IQ sample rate, and nothing above the seam could change
either.**

### 15.1 The operator could only ever see 48 kHz

`Hl2Backend::emitPanState` publishes the span as the IQ sample rate, which is
correct — on this radio the DDC rate *is* the span. But:

- `m_sampleRateHz` defaulted to **48000**, the NARROWEST of the four rates.
- There was no way to change it. `IRadioBackend` had `setPanCenter` but no
  `setPanBandwidth`, so a zoom request never reached the backend at all.
- `RadioModel::dispatchPanCenterBandwidth` wrote the requested span straight
  into `PanadapterModel` for a non-Flex backend and returned success.

So the view widened while the receiver kept sending its old, narrower window.
The VITA-49 tiles are honest about their own extent, so the region the data
never covered rendered **black** — the same lie #4142 fixed for pan *center*,
reintroduced on the bandwidth field. Zoom-out was clamped by
`RadioModel::maxPanBandwidthMhz()`, a FlexLib platform table that falls through
to **5.4 MHz** for any model string it doesn't recognise, so "Hermes-Lite 2"
could be zoomed **14x past its own data**.

Fixed by making the whole loop honest:

| Direction | Mechanism |
|---|---|
| Down | `IRadioBackend::setPanBandwidth` — HL2 snaps to the nearest real rate and reconfigures the DDC + WDSP chain |
| Up | `panCenterBandwidthChanged` reports the span the radio ACTUALLY took |
| Limits | `panBandwidthLimitsChanged` reports 48–384 kHz, so the zoom clamp stops where the data stops |
| Default | the narrowest rate, then whatever span the operator last chose (§15.2) |

The snap is **nearest by RATIO, not linear distance**. The rates are
octave-spaced and zoom is multiplicative, so linear-nearest biases every request
toward the wider neighbour: between 96 and 192 kHz the geometric mean is
135.8 kHz but the arithmetic mean is 144 kHz, and a 140 kHz request belongs to
192 kHz by ratio and to 96 kHz by distance. `hl2_backend_test` pins exactly that
case — every other row in its table agrees under both rules, so without it the
`log()` could be deleted and the suite would stay green.

**Do NOT send a filter-pipeline reset (`0x39`) on a rate change.** See
`MetisClient::requestPipelineReset` — doing that on every geometry change wedged
a board hard enough to need a power cycle. The decimation filters settle on
their own.

### 15.2 The span is a COST, so it is opted into and remembered

On this radio the span is not a free display choice — it IS the DDC rate, so it
sets the wire load and the DSP load together:

| Span | EP6 pkt/s | Sustained UDP | App CPU (measured, M-series) |
|---|---|---|---|
| 48 kHz | 381 | 3.1 Mbps | ~52% of one core |
| 96 kHz | 762 | 6.3 Mbps | — |
| 192 kHz | 1524 | 12.6 Mbps | — |
| 384 kHz | 3048 | 25.2 Mbps | ~62% of one core |

That rules out defaulting to the widest: 25 Mbps of sustained UDP at 3048
packets/second would be imposed on every operator at connect, including on wifi
and on hosts that cannot carry it. But defaulting to the narrowest with no
memory is the original bug — a 48 kHz window on every launch.

So the span **persists**, in the owned `Hl2` settings object (Principle V,
`{"spanMhz":0.384}`). First run is the cheap default; an operator who wants the
wide view chooses it once and keeps it.

`Hl2Settings::lowBandwidth()` reads the connection panel's existing "Use low
bandwidth mode" checkbox — READ ONLY, since that flat key is owned by the
connection UI — and caps the widest offered span at **96 kHz**. The cap applies
to the ADVERTISED limits as well as to requests, or the zoom control would let
the operator drag into a span the backend then silently refuses: the display
claiming a width the data never had, which is the same lie as the black bars.

### 15.2.1 The frame rate tracked the zoom, not the sliders

A backend that streams cooked spectra emits one frame per FFT block, so its
frame rate is the **sample rate divided by the FFT size**:

```
 48 kHz / 1024 =  47 fps      384 kHz / 1024 = 375 fps
```

Measured on the live radio: **375 fps** at full zoom out. The Display->FFT FPS
and Display->Waterfall Rate sliders governed neither — they emitted `display pan
set … fps=` and `display panafall set … line_duration=`, Flex wire text
addressed to a command interpreter this radio does not have.

For the waterfall this was **correctness, not just load**: the widget scales its
time axis from `line_duration`, so rows arriving at 375/s against a 100 ms
calibration made the visible history up to **37x shorter than it claimed**.

**The cap lives at the SOURCE** (`Hl2RxDsp::setSpectrumRateFps`, reached through
`IRadioBackend::setPanFrameRate`), where a frame that is not due costs nothing.
An earlier cut of this coalesced frames downstream in `RadioModel` instead,
averaging in the power domain to keep the noise floor stable across zoom. It
worked, but it was the wrong place: it computed every one of the 375 FFTs and
then spent 1024 `pow()` per frame per feed combining them — roughly *doubling*
the spectrum-path cost at exactly the span where cost matters most.

| Spectrum path at 384 kHz | Calculated cost |
|---|---|
| FFT alone | 22.5 ms/s |
| + downstream coalescing | 45.0 ms/s |
| **source-side cap (shipped)** | **1.5 ms/s** |

Skipping ~93% of the FFTs outright is ~30x cheaper, and it dissolves the reason
the power-domain averaging existed: nothing is combined, so every emitted frame
is a real, unmodified FFT and no level can shift with zoom.

Two details that are load-bearing:

- **The accumulator keeps filling on a skipped interval**
  (`Hl2Spectrum::accumulate`) — it is the transform that is skipped, not the
  feed. Dropping it instead (the first implementation) looked equivalent and was
  not: a due frame then has to refill from empty, and that refill is
  `fftSize / 126` EP6 blocks — 23.6 ms at 48 kHz against 3.0 ms at 384 kHz.
  Added to the interval, a 25 fps request landed near **16 fps at 48 kHz** and
  23 fps at 384 kHz, so the rate still tracked the span, which is the coupling
  this shaper exists to remove. Feeding the window bounds that cost to a single
  block (2.6 ms / 0.3 ms) and the frame stays contiguous either way, because no
  sample is ever discarded. `hl2_spectrum_rate_test` measures the spread.
- **The waterfall keeps a second gate** in `RadioModel`, because the waterfall
  rate is a separate and normally slower control. A plain drop, not a coalesce —
  frames are already scarce by the time they arrive. **The gate paces on a
  cadence derived from the rate, never on the rate value itself** — see 15.2.2.

The accepted trade: at 384 kHz and 25 fps the FFT sees a 2.7 ms window every
40 ms, so a signal landing entirely between two displayed frames is not seen.
That is standard for a display-rate panadapter, and it is the reason the
averaging was considered at all.

**The cap rounds DOWN, on purpose.** A frame is only emitted on a completed
1024-sample boundary, and the next deadline is taken from the emit rather than
advanced by the interval, so the achieved rate is the first frame boundary at or
after the target period. At 384 kHz frames complete every 2.67 ms, so a 40 ms
target (25 fps) lands on 42.7 ms — **23.4 fps**, which is exactly what the radio
measured. The alternative (advancing the deadline by the interval, letting it
catch up) hits the target average but can burst after a stall. Undershooting a
display rate by 6% is invisible; a burst is the thing this cap exists to
prevent, so the rounding stays.

Measured on the real HL2 at 580 kHz AM, waterfall rate 100 — *before* #4606, so
the gate was still reading that 100 as 100 ms:

| Span | Pan | Waterfall |
|---|---|---|
| 384 kHz | 23.4 fps | 10.1 rows/s |
| 48 kHz | ~25 fps | 10.0 rows/s |

An 8x spread across the zoom range, gone. The 10 rows/s column is the inverted
gate described in 15.2.2; at rate 100 the waterfall now tracks the pan.

Those two rows were measured on hardware against the intended design, and are
what exposed the first implementation as wrong: it could not produce the 48 kHz
row. Emptying the accumulator between frames put that corner near 16 fps, and
the table's own numbers are what made the discrepancy visible rather than
plausible. `hl2_spectrum_rate_test` now pins it offline, wall-clock paced, at
every rate the gateware offers — 23-24 fps for a 25 fps request with a ~4%
spread across the zoom range, against ~35% before the fix.

### 15.2.2 The waterfall rate is a rate, not a duration

The Display->Waterfall Rate control is a **1..100 rate: low is slow, high is
fast**. That direction was measured on real Flex hardware (#3104, issue #3070)
and it is what the slider label, the time-scale drag and SpectrumWidget's time
axis all assume.

The trap is the name it travels under. Flex calls the wire parameter
`line_duration` and FlexLib types it as milliseconds, so the field that carries
this rate through `PanadapterModel` is still called `waterfallLineDuration()`.
A backend with no radio-side display engine has to pace waterfall rows itself,
and `RadioModel::onBackendSpectrumFrame` used to read that field literally — as
a millisecond interval. The control therefore ran **backwards on the HL2**:

| Rate | Gate before #4606 | Result | After #4606 |
|---|---|---|---|
| 1 (slowest) | 1 ms | ~25 rows/s — the fastest | 5 s/row |
| 100 (fastest, default) | 100 ms | 10 rows/s — the slowest | ungated, ~25 rows/s |

**Do not read `waterfallLineDuration()` as milliseconds.** It is a rate; convert
through `src/core/WaterfallRate.h`.

#### Two producers, two laws

The conversion is not one function, and the reason is worth stating because the
first cut of #4606 got it wrong in a way that built and passed:

- **`flex*`** — a Flex's display engine owns the conversion. We do not choose
  that law and only know it by measurement (#3104's 16-point curve). It is
  steeply log-shaped: rate 50 is 677 ms/row, rate 80 is 81, and it saturates
  flat from 93 up where the radio is already producing rows as fast as the
  panadapter makes frames. Used to *ask* a Flex for a cadence, and to seed the
  time axis before real row timestamps arrive.
- **`local*`** — this host, for a backend that streams raw spectra. Here the law
  **is** ours, so it is linear in rows per second between 0.2 and 25: rate 50 is
  half the speed of rate 100, which is the only property an operator can predict
  without measuring.

Reusing the Flex curve for the local pacer looked like consistency and was
measured on the HL2 as an unusable control — rate 50 gave 1.5 rows/s and nothing
moved usefully until about 70, because 70% of the slider was spent inside the
bottom 5% of the speed range. The two laws disagree by more than 10x in the
middle of the control, which is also why `SpectrumWidget` is told which one
applies (`setWfRateShapedLocally`, from `RadioModel::shapesDisplayRatesLocally`)
rather than assuming.

At the top of the control the local gate is **lifted entirely** rather than set
to 40 ms. At `kMax` the operator is asking for the fastest the display can go,
and the honest ceiling there is the pan's own frame rate — which Display->FFT
FPS already owns. A fixed 40 ms gate would both drop rows from a 25 fps stream
on rounding and silently cap a pan the operator had set to 60.

Measured on the real HL2 at 96 kHz span, 25 fps pan:

| Rate | Predicted | Measured |
|---|---|---|
| 1 | 0.20 rows/s | 0.20 rows/s |
| 10 | 2.45 rows/s | 2.47 rows/s |
| 25 | 6.21 rows/s | 6.19 rows/s |
| 50 | 12.47 rows/s | 12.52 rows/s |
| 80 | 19.99 rows/s | 20.01 rows/s |
| 100 | pan fps | 24.61 rows/s (pan 24.61) |

`waterfall_rate_test` pins the direction, the monotonicity, both endpoints, and
that the slider midpoint is the speed midpoint — the property whose absence was
the second bug.

### 15.2.3 There is no hardware black level to select

The Display panel's **Black Level** button cycles the waterfall floor source:
`Off` (manual level), `SW` (this client's noise-floor estimate), `HW` (the
radio's own per-tile level). HW is a Flex feature — `display panafall set <id>
auto_black=1` makes the radio compute a level and embed it in each waterfall
tile.

The HL2 has no such thing. Selecting HW there sent a command to a command plane
that does not exist, and left `SpectrumWidget` waiting for a per-tile level that
never arrives — HERMES §17's failure shape again: the button moves, the setting
persists, the picture is unchanged.

HW is now gated on `RadioCapabilities::hasRadioSideWaterfallAutoBlack`, so on the
HL2 the button cycles `Off <-> SW`.

**The gate is a MASK, not a rewrite**, and that distinction is the whole design.
`DisplayWfAutoBlackRadioSide` is the operator's stored *intent*; the capability
decides what is *in effect*. On the HL2 the button reads SW, the SW estimate
runs, and `auto_black` is never sent — while the stored value is untouched, so
plugging the Flex back in restores HW by itself.

The first cut coerced instead: it forced the mode to SW *and emitted the normal
change signals*, which land in `SpectrumWidget::setWfAutoBlackRadioSide()` and
write AppSettings. One session on an HL2 then permanently deleted a Flex user's
HW preference, and switching back gave them SW with no record they had ever
chosen otherwise. A capability gate must never mutate stored intent — the
capabilities map's own rule 2 ("restore the permissive value on disconnect")
only makes sense if the gate is presentation state that can come *back*.

The seam:

| | Read | Persists |
|---|---|---|
| `wfAutoBlackRadioSide()` | intent — menu seeding, settings | yes |
| `effectiveWfAutoBlackRadioSide()` | intent ∧ capability — rendering, radio pushes | no |
| `setRadioSideAutoBlackAvailable()` | the mask | **no** |

A deliberate click on the HL2 *does* overwrite the stored intent, and that is
correct: the operator made a real choice on a real radio.

**SW is not gated and must never be**, on any backend. On a radio reporting
false it is the only automatic floor the operator has.

### 15.3 hpsdrsim cannot reproduce this

**The simulator does not honour a sample-rate change.** Commanded to 384 kHz it
keeps delivering ~40 frames/second, so the 375 fps condition is invisible there —
inferring the input period from the two observed output rates is what showed it.
Anything that needs a real HL2 rate change has to be measured on hardware.

How it was found is worth keeping: the two shaped output rates were solved
backwards for the input period. A 33 ms target producing 20 fps and a 100 ms
target producing 9 fps are only consistent with frames arriving every ~25 ms —
40 fps, not the 375 the sample rate implied. The simulator was reporting a
384 kHz span while delivering a 48 kHz stream.

### 15.4 Killing the client wedges the radio

**Cost more time during this work than any code defect, so it goes first.**

The HL2 is single-client, and the gateware watchdog halts its stream when EP2
stops arriving. A client that exits WITHOUT sending a Metis stop leaves the
board streaming at a dead endpoint; it then halts and **stops answering
discovery**, and only a power cycle brings it back. `MetisProtocol.h` documents
the mechanism; what was not written down is how easily it is triggered from the
outside.

`SIGTERM` to the application is enough. During this work the radio was wedged
three separate times that way, and each time it looked like a software failure:

| What it looked like | What it was |
|---|---|
| "connect times out at 384 kHz" | the board was already wedged from the previous kill |
| "no audio on any mode" | the stream had halted; nothing was arriving |
| "the radio is unreachable" | `ping` answered, Metis discovery did not |

The distinction that settles it in one command — a board that pings but does not
answer a discovery probe is wedged, not busy and not misconfigured:

```
EF FE 02 + 60 zero bytes  ->  udp/1024
   reply byte[2] == 0x02   idle, free to connect
   reply byte[2] == 0x03   streaming to some client
   no reply at all         WEDGED — power cycle required
```

That same exchange, sent **unicast to one host** instead of to the broadcast
address, is what the connect dialog's **Connect by IP** page does when its
*Radio type* is set to `Hermes-Lite 2` (`ConnectionPanel::probeHermesLite2`,
sharing `discoveryRequest()` / `parseDiscoveryReply()` with the broadcast
sweep). It is the only way to reach a board across a VPN or a routed subnet,
where the broadcast never leaves the local segment. Note the failure modes are
NOT distinguishable from the UI: an unreachable address, a wedged board, and a
board that is not an HL2 all look like "no reply". Fall back to the raw probe
above before concluding anything.

**Always disconnect through the normal path before terminating**, including in
automation. Verified both ways here: a bridge `disconnect` then exit leaves the
board reporting `0x02` idle, while a bare `SIGTERM` leaves it silent.

The trap for a diagnostician is that the wedge is *caused by the previous test
and observed during the next one*, so it reads as a regression in whatever
changed in between. **A measurement taken on hardware you just mistreated is not
evidence.**

### 15.5 The silent-audio hunt, and two wrong diagnoses

Receive audio stopped on a development build. The eventual cause was neither of
the first two answers, and both were wrong in instructive ways.

**It was not the DSP.** The suspicion was that raising the default span to
384 kHz had broken WDSP, which now decimates 8:1 instead of 1:1. Disproved by
running the production config at every rate offline — identical audio, **0.0 dB
spread across all four** (`hl2_rxdsp_rate_test`, written for this). Later
confirmed on the radio: **−0.06 dB** between the 384 kHz and 48 kHz spans.

**It was not EP2 pacer starvation.** The next theory was that at 384 kHz the
8× EP6 ingest, FFT and WDSP work on the shared I/O thread was starving the EP2
pacer, tripping the gateware watchdog. It is a plausible mechanism and it is
worth keeping in mind — but the only evidence for it was a connect timeout on a
board that had just been wedged by a `SIGTERM` (§15.4). **The evidence was
manufactured by the diagnostician.** The radio has since run 384 kHz stably for
long stretches.

**What it actually was:** two unrelated faults stacked.

1. A bug fixed in #4466 — `setPcAudioLocked(true)` checks the PC Audio button
   under a `QSignalBlocker`, so `toggled()` never fires, the RX sink never
   opens, and `PcAudioEnabled` is never written back to True. Both RX-start
   paths are gated on that persisted setting, so a stale False skips them, and
   the button is disabled so nobody can click it to recover. The branch under
   test predated that fix.
2. A physically disconnected antenna, which produced quiet-but-present audio
   after the first fault was resolved.

Three lessons worth more than the fix:

- **"No audio" is not one symptom.** A dead sink (`Stopped`, `device_open=false`,
  zero bytes) and a live sink carrying a weak signal look identical to the
  operator and are completely different faults. `get_state model=audio` and a
  sample capture separate them in seconds; the second fault was only visible
  once the first was gone.
- **Check the RF before the code.** A noise floor of **−116 dBm across both MW
  and HF**, where the same radio had been in ADC overload an hour earlier, is an
  antenna problem. `floors` answers this without a screenshot.
- **State a hypothesis's evidence, not just the hypothesis.** The pacer theory
  sounded strong and had exactly one supporting observation, which was
  contaminated. Naming the evidence would have shown that immediately.

### 15.6 The coverage that let it through

Every one of these defects was invisible to a green suite, and each for the same
reason: **the test shared an assumption with the code.**

- `hl2_rxdsp_test` only ever ran **48 kHz in / 48 kHz audio out**, while
  production runs 24 kHz audio and, since the span became controllable, any of
  four input rates. A rate at which the demodulator went silent would have
  passed. `hl2_rxdsp_rate_test` now sweeps the whole grid and asserts on audio
  level, not just on the channel opening.
- `hl2_tx_loopback_test` **hardcoded `binHz = 48000/n`** while silently
  depending on the backend's default being 48 kHz. Raising that default moved
  every expected bin and failed three assertions for a reason that had nothing
  to do with transmit. It now pins its rate explicitly.
- The panadapter FFT keeps working at any rate because it never touches
  `WdspChannel`, so **a healthy display is not evidence of a healthy receiver.**
  That is what made the audio fault look like a display-side change.
- The span-snap table in `hl2_backend_test` would pass under either
  ratio-nearest or linear-nearest for every row except the one deliberately
  placed between the geometric and arithmetic means. Without that row the
  `log()` could be deleted and the suite would stay green.

The general form, which §14.6 already records for the sideband inversion: **a
test that inherits a default cannot detect that the default is wrong.** Pin the
value the assertion depends on, even when it looks like a constant.

### 15.7 Noticed, not fixed

- **ADC overload chatter.** On the MW broadcast band with the default +20 dB LNA
  the overload flag dithers, and the warning in `publishTelemetry` — although
  edge-gated — fires **~133 times/second**, flushing the log ring. The gate is on
  the value changing, but the value genuinely chatters. It also buries every
  other log line, which is how it obstructed the diagnosis in §15.5.
- **The HL2 LNA gain is only settable at connect time** (`lnaGainDb` param).
  There is no seam verb for RF gain, so an operator on a strong band cannot back
  it off without reconnecting. This is why the overload above could not simply be
  turned down.
- **Audio clips hard on strong signals.** On MW with that same +20 dB LNA,
  demodulated audio measured **RMS 1.09 and peaks of 5.5 against a full scale of
  1.0**. Identical at both span extremes, so it is not rate-related — it is the
  front end being slammed, the same root cause as the two entries above.
- **No automation verbs for span or display rate.** Testing this needed
  `pan span <mhz>`, `pan rate <fps> <wf_ms>` and a `perf` verb returning
  `panFps`/`wfFps` as JSON. Without them the span had to be driven by repeated
  `pan_zoom_in`, the FPS slider reached through a menu, and the frame rates
  scraped from the log file — which the overload chatter above nearly made
  impossible.
---

## 16. Receive handedness and tuning — the two-error trap

The most expensive bug of the project, and the one most likely to recur verbatim
on the next radio that owns its own DSP. Read this section before wiring IQ into
any demodulator.

### 16.1 What was wrong

`Hl2RxDsp` handed the **demodulator** the conjugate of the wire IQ and the
**spectrum** the raw wire. Both backwards — each was wired to the other's
convention. The correct split follows from two measured facts:

1. **The HPSDR wire is the conjugate of the analytic convention.** A signal
   *above* the NCO arrives at a *negative* frequency.
2. **WDSP's RXA selects the opposite sign to its passband bounds** (§5).

So the **demodulator takes the RAW wire** — (1) and (2) cancel — and the
**spectrum takes the CONJUGATE**, having no such quirk.

```cpp
// Hl2RxDsp::processIqBlock — the whole fix
m_conjugated[n] = std::conj(iq[n]);      // spectrum: analytic convention
m_spectrum->process(m_conjugated, ...);
m_iqBuffer.insert(..., iq.begin(), iq.end());   // demodulator: raw wire
```

### 16.2 The slice shift is NOT part of the bug

`shift = slice - NCO` is **correct** and derivable once handedness is settled:
the wire puts a signal at `F` at `-(F - NCO)`, so mapping the slice's own
frequency to baseband needs `-(slice - NCO) + shift == 0`.

It looked like a co-conspirator, and flipping it was tried. It measurably broke
off-centre tuning and `hl2_shift_test` caught it within one build. **Do not
"fix" this sign.** It only ever looked wrong because it had been validated in
LSB — the one mode the conjugation bug made correct.

### 16.3 What the operator sees, and how to read it

| Symptom | What it actually means |
|---|---|
| **LSB/DIGL work, USB/DIGU do not** | the chain is coherently inverted — NOT a broken mode |
| Signals render on the wrong side of a correctly-drawn cursor | spectrum handedness |
| Slice mistunes by ~2× its offset from the NCO | shift sign disagrees with IQ handedness |
| TX gets spotted correctly, but only in the "wrong" mode | inversion is end-to-end, not display-only |

The tell for *coherent inversion* is that everything works perfectly in the
mirrored mode — including transmit, including third-party spots. A localized
mode/filter bug cannot produce a fully functional radio under the wrong label.

### 16.4 The measurement that settles it: force the shift to ZERO

Two compensating errors cancel at normal off-centre tuning, so **any measurement
taken at a non-zero shift sees a corrected result and proves nothing.** Zero
shift is the one geometry where nothing can compensate.

Force it by exploiting the NCO re-centre rule: tune far enough away that the NCO
must jump, then land on the target — the NCO follows and the shift is exactly 0.

```
tune 7.100 MHz   (far)      -> NCO jumps
tune 9.9985 MHz             -> NCO == dial, shift == 0
```

Then park a known carrier (WWV) 1500 Hz off the dial and ask which side each
mode hears. Before the fix, at zero shift:

| mode | heard | wanted | margin |
|---|---|---|---|
| usb | below dial | above | 100–300× |
| digu | below dial | above | 100–300× |
| lsb | above dial | below | 100–300× |
| digl | above dial | below | 100–300× |

After: all four correct, and at normal off-centre tuning the recovered tone is
exactly the offset (1500 Hz on a 1500 Hz offset), which is what confirms the
shift sign independently.

### 16.5 Why every instrument agreed with the bug

This is §14.6's lesson recurring, and it cost a second full session because the
compensations were *not* obviously related to each other:

- **The unit tests fed IQ no HL2 ever sends.** Both `hl2_rxdsp_test` and
  `hl2_shift_test` generated textbook `exp(+jwt)`. The wire sends `exp(-jwt)`.
  Correct expectations, wrong stimulus — so a mirrored panadapter *and* an
  inverted demodulator both passed. **Test stimulus must use the wire's
  convention, not the textbook's.**
- **`hl2_shift_test` validated in LSB**, the one mode the inversion made
  correct. A sideband test that runs in a single mode proves nothing about
  handedness.
- **The live sideband sweep put the test carrier at the pan centre.** A mirror
  is invisible on its own axis. It confirmed "all four modes correct" while the
  panadapter was visibly mirrored to the operator. **Never validate handedness
  with a signal at the pan centre; always off-centre.**
- **The audio path was correct** at normal tuning, so listening proved nothing.
  The panadapter was the only consumer with no compensating error — the one
  instrument telling the truth, and the one easiest to dismiss as "a display
  bug".

### 16.6 Bring-up checklist for the next DSP-owning backend

Do these in order, before believing any audio:

1. **Establish wire handedness first**, from the decoder, with a synthetic tone
   of known sign. Write it down. Every later decision depends on it.
2. **Conjugate exactly once**, at one place, and be explicit about which
   consumer gets which. Two consumers with opposite needs is a design fact, not
   an accident — comment it at the split.
3. **Verify at zero shift** before verifying anything else. Compensating errors
   cancel everywhere else.
4. **Verify off-centre**, in **all four** SSB-family modes, against a known
   carrier. Not one mode, not at the pan centre.
5. **Check the panadapter against the demodulator explicitly.** They are
   independent consumers of the same buffer and can disagree; if they do, one of
   them is compensating for something.
6. **Confirm from outside the system** (§14.6): a second receiver, or PSK
   Reporter spots in the mode under test. This bring-up ended with 63 spots on
   14.074 DIGU — the first evidence that could not have come from a
   self-consistent loop.

### 16.7 Related: mode changes must re-push the passband

Separate defect, same session, same root category (order-of-operations against
WDSP). `SetRXAMode` discards a passband applied before it, so the HL2's filter
was effectively sticky across mode changes: arriving at DIGU from CW handed the
decoder a ~500 Hz window and it decoded nothing, with the mode indicator
correct. A radio that owns its DSP gets no mode echo to heal this — **the
backend must supply a per-mode default passband itself**, applied on change so
an operator's own filter edit survives (oracle addendum 2 §B3).

Also fixed here: `modeFromString` knew `"CWU"` but not `"CW"` — the spelling
`TciProtocol::tciToSmartSDR` produces for TCI's `cw`, and the one a Flex
reports — so plain CW fell through to the USB fallback and was demodulated as
SSB. `NFM` was missing for the same reason. **Any mode name that appears in the
TCI `modulations_list` needs a mapping, or it silently becomes USB.** `RTTY`
still has this gap.

---

## 17. Band switching, the companion filter board, and RF gain

Three controls that all looked wired and were not. Each failed in the same
shape: the GUI drove a **Flex command plane** that this radio does not have, so
the widget moved, the setting persisted, and nothing reached the hardware.

### 17.1 Band buttons — the app is the band stack

The Band sub-panel resolved a Flex band-stack key and sent
`display pan set <pan> band=<key>`. On a Flex the RADIO owns the stack and
restores frequency, mode, filters and antenna from it. **The HL2 owns none of
that** — it has no VFO to read back — so the command reached nothing.

`MainWindow_Wiring.cpp` now branches on `usesFlexCommandPlane()`: for a
non-Flex backend the app is authoritative and tunes the slice to the band's
default frequency and mode directly. Note this is the *exact opposite* of the
rule stated for Flex three lines above it, where using the button's `freqMhz`
is explicitly called out as wrong (#1876). Both are right — the argument is a
"static UI default" only when something better exists.

`RadioCapabilities::tuningMinHz/tuningMaxHz` was added so the grid can be
honest: the HL2 reports 0.1–38.4 MHz (the AD9866's first Nyquist zone), and
band buttons outside it are disabled with a tooltip rather than tuning the
receiver somewhere it cannot hear.

### 17.2 The J16 filter byte rides the CONFIG register

**The HL2 has no switchable filters of its own.** It has seven open-collector
outputs at `0x00[23:17]`, which the *gateware* forwards as one byte to I2C
address `0x20`. Nothing in this codebase writes I2C — setting the config bits
IS the whole mechanism (oracle §8).

Two things make this the riskiest change in the area:

1. **It shares a register with the sample rate and the receiver count.** A bit
   in the wrong place lands in `[25:24]` or `[6:3]`, and the failure is a radio
   that still streams, still looks correctly framed, and delivers samples at the
   wrong rate or from an unassigned receiver.
2. **The field is shifted.** `DATA[16]` is not part of it, so the byte goes in
   as `C2 = (oc & 0x7F) << 1`. Unshifted, every selection is one relay too low —
   80 m would engage the 160 m low-pass.

Anything that rebuilds `m_ccConfig` must carry the filter byte through.
`setSampleRate()` already had this bug once for the receiver count; the comment
there now covers both.

The one-hot mapping is **Quisk's `Hermes_BandDict` verbatim**, not re-derived:
the grouping (60+40 share a filter, 17+15 share one) is a property of the N2ADR
board. Our contribution is the frequency *ranges*, and the unit test checks
those by asserting every amateur band lands on Quisk's answer.

Two deliberate departures from "always engage the HPF":
- Below 1.6 MHz nothing is engaged — the AM-blocking HPF would remove exactly
  what is being listened to.
- On 160 m the HPF stays out. The HL2's own switching supply couples spurs into
  the filter board's 160 m and HPF inductors (wiki, `Options.md`).

**Selection is logged at INFO on `aether.hl2`, not debug.** There is no
readback anywhere in the protocol — the gateware writes to I2C and nothing
answers — so that log line is the only evidence of what the relays were told to
do, and a support log captured after the fact has to already contain it.

### 17.3 Verifying something with no readback

`tests/hl2_live_band_filter_probe.cpp` (hardware-only, `EXCLUDE_FROM_ALL`) is
the answer to "the unit test shares our reading of the register map, so what
would catch a convention error?"

- **Sample rate as the independent check.** EP6 carries 126 samples per packet,
  so 48 kHz is 381 packets/second. Measured across all nine filter selections
  it stayed within 1.002× — a bit leaking into `DATA[25:24]` would have halved
  or doubled it. Non-zero IQ rules out an unassigned receiver.
- **Physics as the proof the relay moved.** Park on 0.6 MHz, A/B the AM-blocking
  HPF, and measure. On this radio the AM band dropped **22–24 dB** when the HPF
  was engaged. That is not the radio agreeing with our register map; that is the
  antenna path changing. It also confirms an N2ADR board is fitted — the writes
  are inert and harmless on a bare HL2.
- Same method for RF gain: a commanded 10 → 30 dB step moved the raw wire noise
  floor **19.8 dB**, measured before any of our DSP or dB referencing runs.

### 17.4 RF gain was connect-time only

`lnaGainDb` was applied once in `connectRadio()`. `IRadioBackend::setPanRfGain`
(default no-op, so Flex is unaffected) now carries the ANT panel's slider to
`MetisClient::setLnaGainDb` at any time, and `panRfGainInfoChanged` reports the
AD9866's real geometry — **−12…+48 dB in 1 dB steps**, against the model's
Flex-shaped default of −8…+32 in 8s, which had made two thirds of the available
gain unreachable.

`Hl2DbReference` is moved in the same call, so the trace and the S-meter do not
slide when gain changes — an operator backing off 10 dB on a strong band would
otherwise watch the noise floor drop 10 dB and read it as the band going quiet.

**The persisted key is now family-scoped** (`DisplayRfGain_hl2`). It was shared,
which was harmless while the HL2 ignored the value and stopped being harmless
the moment the slider reached the register: a gain last set on a Flex was
restored onto the HL2 as an LNA gain the operator never chose for that radio.
Observed live — a Flex-era `16` was pushed at connect.

### 17.5 Directional power is UNCALIBRATED and says so

Forward/reverse counts go through Quisk's `power_meter_std_calibrations
['HL2FilterE3']` curve. That is a *reference* curve for an N2ADR rev E3 board,
**not a calibration of this radio** — the coupler, toroid and detector diode all
vary between boards (oracle §6). The meters are labelled "(uncalibrated)" in
their own `MeterDef` descriptions, because the number itself looks exactly like
a calibrated one. Raw counts continue to be logged; a per-unit calibration
replaces the table and nothing else.

SWR remains the one directional quantity that is meaningful without
calibration — it is a ratio from the same converter, so the unknown scale
cancels.

### 17.6 Meter pacing

WDSP hands us a signal-strength reading per demodulated block — ~47/s at 24 kHz
output, scaling with the span — and every one crossed to the GUI thread. Two
separate mechanisms fix that and they are not interchangeable: an EMA smooths
*every* sample so the published value represents the whole interval, and a
100 ms gate decides how often one is published. Dropping samples without
smoothing would alias.

100 ms is the cadence `MetisClient` already paces telemetry at, and the
attack/decay constants (0.5/0.15) are the ones `MeterModel` uses for Flex
forward power — reused so meters behave the same across families.

### 17.7 The RF power slider has 101 positions and the radio has 16

`Hl2Backend::applyDrive` maps the operator's 0–100 onto the drive field's
0–255 (`kTxDriveMax`), and `setTxPower` already says the gateware decodes only
the **top nibble**. That comment is right, and the consequence is bigger than it
sounds: the slider offers 101 settings and the radio can hold **16**. Six
percentage points of travel do nothing at all, and the next single point is a
step of over a decibel.

Measured on the live radio (14.200 MHz USB, 1 kHz tone at −10 dBFS, dummy load,
gateware v74), reading `TX:FWDPWR` through the §17.5 reference curve:

| Slider | Drive register | Top nibble | Forward |
|---|---|---|---|
| 44 % | 112 | 7 | 1.984 W |
| 50 % | 127 | 7 | 2.001 W |
| 51 % | 130 | 8 | 2.312 W |
| 56 % | 142 | 8 | 2.319 W |

44 % and 50 % differ by 0.04 dB — the same nibble, so the same radio state, and
the difference is measurement noise. 50 % to 51 % is +1.25 dB. The quantisation
is a **hardware fact, not a defect**: nothing in `applyDrive` is wrong, and
rounding differently would only move the step edges. What is worth fixing is the
UI's implied precision, because an operator nudging 44 → 50 to "come down a
little" changes nothing and has no way to find that out.

This also bounds what §17.5's uncalibrated curve can be asked to prove. Halving
the slider is **not** halving the drive code — 100 % → 50 % is nibble 15 → 7,
and 50 % → 25 % is 7 → 3 — so the "halve the control, expect −6.02 dB"
certification stimulus does not describe this radio's control at all. See
`docs/radio-certification.md` for what that does and does not certify here.

---

## 18. Voice and data features: the three RX audio buses

Bringing up receive audio made the HL2 *audible*. It did not make the platform's
decoders work, and the reason is structural rather than per-feature.

There are **three** RX audio buses in this app. They carry the identical payload
— 24 kHz interleaved stereo float32 — and differ only in which wire they arrive
on:

| Bus | Signal | Producer | HL2 |
|---|---|---|---|
| **A** — Flex VITA slice audio | `PanadapterStream::audioDataReady` | Flex; also the sim's legacy shim | ❌ `panStream()` is null |
| **B** — Flex DAX channel audio | `PanadapterStream::daxAudioReady` | Flex only | ❌ same |
| **C** — backend seam | `IRadioBackend::audioFrameReady` → `RadioModel::backendAudioFrameReady` | HL2, sim | ✅ |

Every consumer is hard-wired to exactly one bus at `connect()` time. Speaker
audio and TCI were each ported to bus C individually, as separate patches
(`MainWindow_Session.cpp`, the `wireDiscovery` relay and the
`backendAudioFrameReady → onDaxAudioReady(1, …)` bridge). Nothing else was, so
everything else on bus A or B binds to a null stream and silently does nothing.

**This is gap-class 15 in §6's terms, and it is the single largest one left.**
It is not a bug in any feature; it is a bug in how features find audio.

### 18.1 What actually works, and what is dead

Audited on `feat/hl2-connect-by-ip`:

| Feature | Bus | HL2 |
|---|---|---|
| Speaker, NR2/NR4/MNR/RN2/DFNR, channel strip | C | ✅ |
| Copy Assist / ASR | engine post-DSP (`receivePresentationPostDspAudioReady`) | ✅ |
| AX.25 / APRS / KISS TNC / PMS mailbox | engine tap (`tncRxAudioReady`) | ✅ |
| WSJT-X and any TCI client | C (bridged, PR #4471) | ✅ |
| WFM demod, SignalClassifier, VoiceSignalDetector | engine-side | ✅ |
| **CW decoder (RX)** | **bus** | ✅ **as of §18.8** |
| **RTTY decoder** | **bus** | ✅ **as of §18.8** |
| **QSO recorder (RX half)** | **bus** | ✅ **as of §18.8** |
| **AetherClock (WWV/WWVB)** | B + a DAX channel hold | ❌ dead |
| **WSPR beacon** | Flex `dax_tx` | ✅ **as of §18.4** |
| **RADE / FreeDV** | B | ❌ unavailable — now **declines** instead of crashing, §18.3 |
| **DAX bridge / virtual audio device** | B | ❌ unavailable — already declined cleanly, §18.3 |
| **DAX-IQ applet** | `iqDataReady` | ❌ dead — the HL2 has raw IQ, just not on that wire |
| Digital Voice waveforms | radio-side firmware | N/A — a Flex feature, correctly absent |

The three that already work do so because they tap **AudioEngine**, downstream
of `feedAudioData()`, rather than a transport. That is the whole lesson: the
features wired to the engine are family-agnostic by construction, and the
features wired to a stream are family-locked by construction. Nobody chose
this — bus A and bus C simply happened to be the same object on a Flex.

### 18.2 The `"flex"` string that is really an enum

`AudioEngine.cpp` tags presentation audio with a hardcoded ternary:

```cpp
source == RxDspSource::KiwiSdr ? QStringLiteral("kiwi") : QStringLiteral("flex")
```

HL2 audio is therefore labelled `"flex"` in `AsrTapPolicy`'s source lock, in
`captureAutomationAudio`, and in the presentation-sync path. It is harmless
*today* precisely because it is a binary and the wrong answer is the only other
answer. It stops being harmless the moment a third family runs concurrently —
`AsrTapPolicy` would lock onto "flex" and be unable to tell two radios apart.

Fix it when the bus is unified, not before; changing the string alone would
break the Kiwi source lock's persisted expectations for no gain.

### 18.3 One live crash — RADE (fixed)

**Corrected.** An earlier revision of this section claimed RADE *and* the DAX
bridge both crashed. Only RADE did, and the correction is worth keeping because
the mistake was made by grepping for `panStream()` derefs and never checking
which ones already sat behind a guard.

- **RADE** — `activateRADE()` ran from its first line to a bare
  `connect(m_radioModel.panStream(), &PanadapterStream::daxAudioReady, …)` with
  no null check anywhere in between. Selecting RADE on an HL2 was a **SIGSEGV**,
  the same null-deref shape as §6 gap 1. **Fixed** on `feat/rx-audio-bus`.
- **DAX bridge** — already safe. `startDax()` guards on `panStream()` before
  touching anything and the `connect` further down is *inside* that guarded
  region; `stopDax()` early-returns on `!m_daxBridge`, which can only be
  non-null if the guard passed.

**Where the RADE guard goes matters more than that it exists.** Guarding the
`connect` alone would have been wrong: everything between the top of
`activateRADE()` and that line mutates real station state — it moves the
TX-slice badge, installs a PTT-off hook on `TransmitModel`, calls
`setRadeMode()` and opens mic capture. That would leave a radio half in RADE
mode with no receive path and an intercepted unkey, which is **worse than the
crash**, because it looks like it worked. The guard goes before the first
mutation.

**What it resets matters too.** RADE is selected by a toggle
(`RxApplet`/`VfoWidget::radeActivated`), not by the slice mode — it runs on an
ordinary DIGU/DIGL slice. The decline therefore leaves the operator's mode alone
and resets the toggle, using the same three setters `deactivateRADE()` uses, so
a decline and a teardown leave the UI identical. An earlier draft of this fix
reverted the slice to USB, which would have fought the operator's own DIGU
selection for no reason.

**Generalised:** a null guard added at the crash site is usually in the wrong
place. The right place is before the first irreversible side effect, and the
decline has to undo whatever the UI already drew.

### 18.4 WSPR TX: the first host-modulated transmit feature

Landed on `feat/hl2-wspr-tx`. Worth reading as a template, because the surprise
was how little was needed.

`RadioModel::prepareWsprTransmit()` refused outright on any non-Flex family,
with a well-reasoned comment: `ensureDaxTxStream()` returns true optimistically
on a pending `stream create` reply, so a family whose command sink drops that
text would "succeed" with a stream that never arrives and leave `transmit dax`
latched for minutes. Correct — for a family that needs the stream.

A host-modulating backend needs no stream at all. The audio route **already
existed**, built for TCI in #4471:

```
pumpWsprBeacon() → feedDaxTxAudioInternal() → m_hostModulation branch
  → txFinalMonitorPcmReady → RadioModel::submitTxAudio() → Hl2Backend
```

So the entire change is an early arm in `prepareWsprTransmit()` that latches a
flag and returns true, the matching release, and teaching `hasWsprTxStream()`
that "the route is ready" and "a dax_tx stream exists" are different claims.

**Three things that looked like blockers and were not:**

- **Mic collision.** Two producers into one modulator would have put shack
  ambience on the frame — the HL2 has no radio-side mic mute to hide behind, so
  `transmit dax` (which is what protects the Flex) buys nothing here. But
  `AudioEngine::startWsprPump()` already calls `setDaxTxMode(true)`, and
  `onTxAudioReady()` early-returns on `m_daxTxMode` *before* it emits
  `txFinalMonitorPcmReady`. The suppression is local and family-independent.
  It was written for Windows/Linux-without-PipeWire and turns out to be exactly
  the mechanism a host-modulating backend needs.
- **Interlock timeout.** The dialog refuses if the radio's TX timeout is under
  120 s, with a Flex-specific instruction. `TransmitModel::m_interlockTimeout`
  defaults to `0`, HL2 never sets it, and `isInterlockTimeoutSufficient(0)` is
  true. No change.
- **Keying.** `requestPttOn(PttSource::Wspr)` reaches `moxCommandIssued` →
  `m_backend->setKeying(on)` on any non-Flex family. Already wired.

**One thing that is genuinely degraded, deliberately:** `tx.setTxFilter(1200,
1800)` is Flex station state. The HL2 derives its TX passband from the mode
instead — DIGU gives 150…3000 Hz. The WSPR tone is a single ~6 Hz-wide 4-FSK
carrier at 1400–1600 Hz, comfortably inside that, so the request is *advisory*
rather than dropped-and-wrong. Documented at the call site so the next reader
does not "fix" it.

**Why this feature is worth more than its size.** §14.6 is the wrong-sideband
bug that no internal check could find, because every internal check shared the
convention. A WSPR frame decoded by a stranger's receiver and reported to
wsprnet.org is an oracle **completely outside our system**: it independently
confirms transmit sideband, frequency accuracy, and that real RF left the
socket. It is the cheapest external validation instrument the HL2 has, and it
costs one 111.6-second transmission.

**It paid off on the first frame.** KI6BCJ/DM06, 40 m, 2026-07-28 00:42:00 UTC,
`rfPower=100` into a real antenna, reported 37 dBm:

- **21 stations decoded it**, from 284 km (AK6RI-1, CM87xi) out to 2080 km
  (VE6PDQ, DO34lr). Best SNR +1 dB.
- **Sideband confirmed correct, from outside.** Dial 7.038600 plus the 1500 Hz
  audio offset lands at **7.040100** on USB; an inverted sideband would have
  put it at 7.037100. Reported receive frequencies span **7040098–7040113**,
  centred on 7040100. Twenty-one independent receivers agreeing on the upper
  sideband is the check §14.6 could not construct internally.
- **Drift 0 on every single spot** — the TX NCO is stable across the frame.

Instrument readings during the frame, for future comparison: FWDPWR 35.5 dBm
(~3.5 W), MICPEAK **−20 dBFS** — exactly the `beacon->start(…, -20.0f, …)`
level, which is what proves the WSPR generator and not the microphone was
feeding the modulator. PA temperature rose 43.6 → 47.4 °C over the frame and
was still decelerating at unkey.

**Two things to know before repeating it:**

- **SWR was 3.3** (1.05 W reflected of 3.5 W forward) on 7.0386. Per §17.5 the
  SWR ratio is the one directional quantity that is trustworthy *without*
  calibration, so that number is real — the antenna is not resonant at the
  bottom of 40 m. It completed safely, but 111.6 s of continuous duty into
  3.3:1 is not a thing to make routine. Check SWR at the WSPR dial frequency
  before arming, not at the band's phone segment.
- **`transmit dax` reads 1 afterwards, and that is not a leak.**
  `applyBeaconBand()` sets the slice to DIGU, and the #2273 rule maps any
  digital mode to `dax=1` on macOS/PipeWire. The beacon deliberately leaves the
  slice on the WSPR channel in DIGU, so the two agree, and it clears when the
  operator leaves DIGU. Worth knowing because it *looks* exactly like the
  latched-dax failure the Flex arm's save/restore exists to prevent — and note
  that on the FLEX path `applyBeaconBand()` runs BEFORE `prepareWsprTransmit()`,
  so `m_wsprTxPreviousDax` is saved as `true` after the mode change already
  flipped it, and the restore hands back `true`. Same visible end state, but
  reached by a path that genuinely is a save/restore no-op. Untangling that is
  a Flex-side cleanup, not an HL2 one.

**And one UX defect the on-air test surfaced**, fixed in the same branch:
selecting a WSPR band updated the status label and *nothing else*. The dial
stayed where it was while the label advertised a different frequency, and the
whole band change — NCO, filter relays, TX oscillator — then happened in the
last seconds before 111.6 s of RF. Selecting a band now tunes the receiver, so
the operator can look at the sub-band and its SWR before committing. Mode,
slice passband and station TX filter are still deferred to arm time, because
those are the parts that disturb a listening setup and get restored after.

### 18.5 The systemic fix, and why not to patch site-by-site

Patching each dead site to *also* subscribe to `backendAudioFrameReady` takes an
afternoon and is the wrong move. It would be the fourth repetition of a pattern
that has already shipped two bugs — the double-feed buzz (`MainWindow.cpp`, the
sim's frames arriving on both bus A and bus C, measured 48043 Hz against a
nominal 24000) and the TCI silence that made WSJT-X track frequency perfectly
and decode nothing. The existing comment already says where this goes:

> *"Any future in-process backend needs the same treatment. The gate belongs on
> 'does this backend own its RX audio', not on a list of families that happen
> not to emit the signal today."*

**Proposal — one bus, owned by `RadioModel`:**

```
RadioModel::rxAudioReady(RxAudioTap tap, int sliceId, QByteArray pcm, int rateHz)
```

- **`tap`** replaces the bus distinction with the distinction that actually
  matters: `Demod` (what the operator hears, post-AGC, post-passband) versus
  `Wideband`/`Modem` (filter-flat, pre-AGC — what a decoder wants). Today this
  is invisible because bus B happens to be pre-AGC on a Flex; on the HL2,
  WSJT-X over TCI is currently being fed **post-AGC, post-passband** audio from
  `Hl2RxDsp`. It decodes, but a modem on AGC'd audio is a known-marginal
  arrangement and nothing in the code admits it.
- **`sliceId`** replaces the DAX channel number as the routing key. Flex maps
  slice → DAX channel internally and keeps its hold registry; HL2 maps slice →
  its single DDC. Consumers never learn which.
- Flex adapts `PanadapterStream` into it, HL2 adapts `audioFrameReady`, the sim
  picks **one** and stops emitting the other (which also fixes the sim feeding
  its decoders the shim's synthetic scene while the speaker plays the real demo
  audio — two audio realities in one session, live on `main` today).
- `wirePanStreamRxAudioSinks()`, `wireBackendSeam()` and
  `rewirePanStreamAfterBackendSwap()` collapse into one rebind.

**The TX mirror.** `prepareWsprTransmit`'s old guard was one instance of a
shape; AX.25 TX has the same DAX-borrow dance, and so does RADE. The rule that
falls out of §18.4:

> **Anything reaching for `ensureDaxTxStream()` should branch on
> `hostModulates`, not on backend type.** One helper —
> `acquireTxAudioPath(reason)` returning a token that restores whatever it
> borrowed — covers WSPR, AX.25, RADE and TCI on every family.

**Capabilities that do not exist yet.** `RadioCapabilities` is the right
structure; four facts are missing, and adding them is what stops the next
backend from re-running this audit:

| Field | Why |
|---|---|
| `hasDaxAudio` / `hasDaxIq` | The honest name for what bus B *is*. RADE and the DAX bridge should decline on this, not crash on a null stream (§18.3) |
| available tap kinds | Whether a `Wideband` feed exists at all, or only `Demod` |
| `providesRadioSideWaveforms` | Digital Voice waveform install is Flex firmware; nothing should offer it elsewhere |
| `rxAudioSampleRateHz` | HL2 is 24 kHz by the deliberate divergence in §13 Tier 4. A future backend may not be, and `DEFAULT_SAMPLE_RATE` is assumed widely |

### 18.6 New seam gaps for §6's checklist

| # | Gap | Symptom | Fix |
|---|---|---|---|
| ~~16~~ | ~~CW/RTTY decoders and the QSO recorder's RX tap bind to bus A inside `wirePanStreamRxAudioSinks()`~~ **DONE** | Decoders were silently dead; no error, no log line, the toggle worked and nothing decoded | `rxDemodAudioReady`, §18.8 |
| 17 | `AetherClockEngine` binds to bus B **and** to a DAX channel-hold registry, keyed on a channel number a single-DDC radio does not have | WWV/WWVB never decodes; the DAX-hold provider correctly no-ops, which hides it | *Open* — needs slice-identity routing, not channel-identity |
| 18 | RADE and the DAX bridge dereference `panStream()` bare | **SIGSEGV on mode change** — same shape as gap 1 | *Open* — do this first |
| 19 | Presentation audio source tag is a hardcoded `kiwi : "flex"` ternary | Third concurrent family is unaddressable; `AsrTapPolicy` cannot tell two radios apart | *Open* — fold into §18.5 |

**The generalised rule, for the next backend:**

> **For every consumer of receive audio, ask which of the three buses it
> subscribed to — and whether that was a decision or an accident of the Flex
> being both.** Add it to the Phase-0 reference diff.

### 18.6.1 The same shape again: station identity stored on the radio

Found while making WSPR usable, and worth its own entry because it is the audio
lesson with a different noun. Three facts about the OPERATOR were read from the
RADIO, so on a radio that stores none of them the app behaved as though the
station had no identity:

| Fact | Was read from | On HL2 |
|---|---|---|
| Callsign | `RadioInfo::callsign`, the Flex `info` reply, `radio callsign <x>` | Empty forever. Radio Setup accepted an edit, sent Flex text nobody listened for, read back blank |
| Grid locator | `RadioModel::gpsGrid()` — a 6000-series GPSDO reading | Empty. The WSPR grid field had no persistence of its own, so it was retyped every session |
| Map home position | GPSDO lat/lon, then GPSDO grid | Neither exists, so `updateHomeFromRadio()` returned having set nothing |

The third is the one that shows how quietly this fails. PSK Reporter drew every
received spot correctly — each carries its own coordinates — and drew **no
paths**, because the map had no origin to draw them from. Nothing errored. The
operator sees a working map that is simply missing the lines.

Fixed by making each fall back to a client-side value:

- `RadioModel::callsign()` falls back to a station-wide `StationCallsign`
  setting. **Station-wide, not per-serial** — unlike the nickname
  (`Hl2Discovery::nicknameSettingsKey`), which really is a property of one
  radio. A callsign belongs to the operator and is the same on every radio they
  own. Radio's value still wins when present, so a Flex is unchanged.
- The WSPR grid persists as `beaconGrid`, alongside the power and tone settings
  that already did.
- `updateHomeFromRadio()` gains the operator's grid as a third source after the
  two GPSDO ones. A 4-character square is ~70 x 100 km: coarse for a fix,
  entirely adequate for drawing a path across a continent.

**The generalised rule:**

> **Anything the app knows about the OPERATOR — callsign, grid, location,
> licence class — must have a client-side home. A radio may report it and its
> value may win, but the radio cannot be the only place it lives.** The failure
> is silent by nature: identity is used to *decorate* and to *query*, so its
> absence looks like "no results" rather than an error.

Same discipline as the audio buses. Ask whether reading it from the radio was a
decision, or an accident of the Flex being the only radio there was.

### 18.7 Suggested order

1. **Gap 18** — the RADE/DAX-bridge null-deref. A crash outranks a feature.
2. ~~**WSPR TX**~~ — done, §18.4. Smallest diff, real operator value, and it
   forced the `hostModulates` TX branch into existence where it was easy to
   reason about.
3. **The unified bus** (§18.5), with CW + RTTY + the QSO recorder as its first
   three consumers — *as* the port, not as three more one-off connects.
4. **AetherClock** — the slice-identity work. WWV on a direct-sampling front end
   is a genuinely good demonstration, and 10 MHz WWV was already the proof
   signal for #4528's panadapter.
5. **Tap kinds** (`Wideband`) — only once there is a second consumer that wants
   one, and once someone has measured whether the AGC'd TCI feed is costing
   WSJT-X decodes.

### 18.8 The bus, as built

`rxDemodAudioReady` landed on `feat/rx-audio-bus`. Smaller than §18.5 proposed,
and deliberately so — it carries the taps and leaves the speaker alone.

**What it is.** One signal on `RadioModel`, 24 kHz interleaved stereo float32 —
byte-identical to what both producers already emitted. Exactly one producer is
bound at a time, chosen in `wireRxDemodAudioBus()`:

| Family | Producer |
|---|---|
| Flex | `PanadapterStream::audioDataReady` (an *additional* subscriber; the existing speaker connection is untouched) |
| HL2, sim | `IRadioBackend::audioFrameReady`, via `backendAudioFrameReady` |

**The predicate the bus keys off now lives on the backend.**
`IRadioBackend::ownsRxAudio()` is self-declared rather than inferred from a
family name or a `dynamic_cast`, so a backend added later cannot be missed by
the bus wiring.

It does **not** replace the existing casts, and an earlier draft of this section
claimed it did. `MainWindow::backendFeedsEngineDirectly()` (was
`backendOwnsRxAudio()`) is still a `dynamic_cast<SimBackend*>`, and it gates the
two SPEAKER-path sites — which are the two that actually produced #4490's
double-feed. The new virtual buys its "can't be missed" property for the tap bus
only.

**The two are deliberately not merged, and the MainWindow one was renamed to
stop anyone merging them.** They read alike and disagree about the HL2:

| Predicate | Asks | HL2 | sim | Flex |
|---|---|---|---|---|
| `IRadioBackend::ownsRxAudio()` | "audio arrives over the seam" | **true** | true | false |
| `MainWindow::backendFeedsEngineDirectly()` | "backend already feeds AudioEngine, so no relay" | **false** | true | false |

An HL2 owns its RX audio *and* needs the relay: `wireBackendSeam()` connects
`audioFrameReady → feedAudioData` for the sim only, so HL2 audio reaches the
engine via `RadioModel::backendAudioFrameReady`. Delegating the MainWindow
helper to `backend()->ownsRxAudio()` would therefore make the relay's early
return swallow every HL2 frame and **silence the speaker on the radio this whole
section exists to support**.

It is **not** "has no PanadapterStream". The sim has both — a stream carrying
the old shim's synthetic scene, and real demodulated audio over the seam — and
answers `true` because the seam is the real one. That silently fixed a live
defect: in demo mode the decoders were being fed the synthetic scene while the
speaker played the demo's actual audio. **Two audio realities in one session**,
and nothing in the code admitted it.

**What was deliberately NOT touched.** `AudioEngine::feedAudioData` keeps its
existing per-family wiring on every family. Nothing audible changes anywhere.
The Flex path gains one extra subscriber to a signal it already emits, and that
is the entire blast radius — which is the shape to copy for the remaining §18
items, not a one-off concession.

**Consumers bind once and never rebind.** They hang off `RadioModel`, which
outlives the backend swap that destroys and rebuilds a `PanadapterStream`. That
retires the rebind fragility `wirePanStreamRxAudioSinks()`'s own comment warns
about, for these three sinks.

**The invariant worth testing is "exactly one producer", not "audio arrives".**
Two producers is not a dead feature, it is a *wrong* one: every decoder sees
each block twice, which a Morse decoder reads as doubled timing — wrong text
rather than no text. `hl2_family_transition_test` asserts it across a family
round-trip, which is the case Qt cannot clean up for us: the seam relay has
`this` on both ends, so unlike a stream-bound connection there is nothing for Qt
to drop when the backend is replaced. The assertion was confirmed to have teeth
by removing the `disconnect` and watching that check — and only that check —
fail.

**Measured on hardware** (HL2 at 192.168.1.21, 21.067 MHz): the QSO recorder
captured 21.6 s of real RX audio — peak 20504, RMS 136.5, 87.5 % non-zero —
where the same recording was silence before. CW decode confirmed on live signals
by the operator.

**Recorder RX/TX mixing is safe, and the Flex-shaped premise happens to hold
for a different reason.** `QsoRecorder`'s header states *"while transmitting,
the radio mutes the RX stream"* — a Flex fact about the wire. An earlier draft
of this section asserted the HL2 keeps demodulating through transmit and so
violates that premise. It does not: `Hl2Backend` drops the frame outright while
keyed —

```cpp
connect(m_dsp, &Hl2RxDsp::audioReady, this, [this](const std::vector<float>& pcm) {
    if (m_keyed) return;
    emit audioFrameReady(floatBytes(pcm));
});
```

— and `Hl2RxDsp::setAudioMuted` stops the pipeline filling in the first place.
So `rxDemodAudioReady` goes silent for the duration of a transmission. **The HL2
mutes at the backend rather than at the radio; same observable behaviour,
different side of the wire.**

The conclusion is unchanged, and it is worth keeping the reason straight: the
`m_transmitting` gate in `QsoRecorder` is still the actual mechanism, and it is
still a hard mutual exclusion, so the recorder would be correct even if a future
backend *did* keep feeding through transmit. This also disposes of a review
finding that the CW/RTTY decoders would transcribe the operator's own sending —
they receive nothing to transcribe.

**Still open, narrowed:** only the UNKEY edge — whether `m_keyed` clears before
the last leakage-contaminated block drains out of `Hl2RxDsp`. The key-down edge
and the body of the transmission are both covered by the gate above.

---

## 19. Hiding Flex-only UI without asking "is this a Flex"

§17 fixed three controls that *looked* wired and were not. This is the other
half of the same problem: controls that are wired correctly, work perfectly on
a Flex, and should not be on screen at all on an HL2. The PROF applet listed a
profile store that does not exist. DAX offered to route streams this radio
never sends. Both were reachable, both were honest-looking, and both were lies.

> Field-by-field reference — what every backend declares and where each value
> is read — lives in `docs/architecture/radio-capabilities-map.md`. This
> section is the narrative; that table is the one to keep current.

### 19.1 The rule, and why the obvious fix is the wrong one

The obvious fix is `if (family == "flex")`, or a `dynamic_cast<FlexBackend*>`.
`RadioCapabilities`' own header comment forbids exactly that — it is the
structural replacement for the model-impersonation anti-pattern (RFC §1) — and
the reason is not stylistic. A family test encodes *today's* device list at
every call site. Add a fourth backend and you have to find them all, and the
ones you miss fail silently in the direction of showing a control that cannot
work.

So: gate on a **declared capability, named for the concept**. `hasDaxStreams`,
not `hasDax` — routing receive audio to a virtual device is not inherently a
FlexRadio idea, and a backend that grows the ability should be able to say so
without the field reading as a vendor special case.

The chain is the one `hasTuner` → ATU dimming established in §17:

```
RadioCapabilities field                      backend declares it
  → RadioModel pushes / relays it            one fan-out point
    → model or signal carries it             GUI never sees a backend
      → ONE widget method owns the state     no second caller
```

### 19.2 Four traps, three of which have already bitten

**Fields default to `false`, so every backend must set every field
explicitly.** This is the one that nearly shipped a Flex regression when
`hasTuner` was added — FlexBackend escaped only because it happened to set
`hasTuner = true` by hand. A backend that omits a field does not inherit a
sensible default; it silently loses the feature. The test asserts Flex reports
`true` for each flag *for this reason*, not as a tautology.

**The model-side flag defaults to `true`.** `TransmitModel::m_hasTuner{true}`
is the pattern. A widget constructed before any backend has reported must stay
in its pre-existing state rather than briefly hide a control that does exist.

**Restore the permissive value on disconnect** — `!connected || caps.hasX`,
every time. With no radio attached there is nothing to be honest *about*, and a
PROF applet that stays gone after unplugging reads as a fault, not as an
accurate report about a radio that is no longer there.

**A widget with two visibility inputs needs ONE method that ANDs them.** The
ATU has two (no tuner, and TGXL-in-Operate). Two callers each doing
`setVisible()` means whichever fires last wins — that was a real bug, not a
hypothetical. `MainWindow::applyCapabilitiesToUi()` exists so this stays true
as flags accumulate: one slot, one owning call per surface, no per-flag
connect-time lambdas.

### 19.3 `capabilitiesChanged` was a signal nobody relayed

`IRadioBackend::capabilitiesChanged` existed from the start and RadioModel
never forwarded it, so `connectionStateChanged` was the only hook. That was
survivable with one flag and stops being survivable at three.
`RadioModel::publishCapabilities()` is now the single fan-out — model-side
pushes, then `RadioModel::capabilitiesChanged(connected, caps)` — and both the
connection edges and a mid-session revision by the backend take the same path.

Note the connect edge legitimately publishes **twice** on the demo backend:
`SimBackend::connectRadio()` emits `capabilitiesChanged` itself, and the
`connected` edge follows. Every consumer is idempotent `setVisible`, so this is
harmless — but a consumer that toggles rather than sets would break here.

### 19.4 The DAX crash guard and the DAX capability are NOT the same test

`MainWindow::startDax()` null-checks `panStream()` before building the bridge.
That is a **crash guard**: auto-starting DAX against an HL2 segfaulted ~3 s
after connect, from the auto-start timer in `onConnectionStateChanged`, because
DAX rides `PanadapterStream`'s VITA-49 audio and RadioModel leaves that null for
every non-Flex family.

`hasDaxStreams` is **UI visibility only**. It is deliberately not merged with
the null check, and the two must not be collapsed into one test. One stops the
operator being offered a control that cannot work; the other stops a session
that reaches the bridge anyway — through automation, a stale setting, a future
code path — from dereferencing a null stream. Fold them together and the crash
path is guarded only by whatever the UI happened to hide.

### 19.5 The extended-DSP bug was plumbing, not policy

`RadioCapabilities::hasExtendedDsp` existed *and* FlexBackend populated it.
Nothing read it. All three GUI call sites went through
`RadioModel::hasExtendedDspFilters()`, which called `capabilitiesFor(m_model)`
— the model-**name** table — and bypassed the seam entirely. A non-Flex backend
declaring the capability honestly had no way to be heard.

The accessor now reads the backend when connected and keeps the name table as
the disconnected/unknown fallback. **Flex behaviour is unchanged, and provably
so rather than approximately:** FlexBackend computes `caps.hasExtendedDsp` as
`capabilitiesFor(model).hasExtendedDsp()`, and the model string it uses is the
same `m_model`, handed over by the `setModelProvider` lambda in
`setupBackend()`. Same table, same key, same answer — only the route changes.
The test asserts the two routes agree across an extended-DSP platform, the "S"
server variants the old substring form used to miss, and plain 6000-series
radios, plus that the table distinguishes them at all so the agreement is not
two constant falses agreeing.

### 19.6 APD needed nothing, and that is worth recording

`apdConfigurable` was checked and left alone. It rides `TransmitDelta`, which
only `FlexBackend` ever populates (from `apd configurable=1` status), it
defaults `false`, and `TransmitModel::resetState()` clears it on disconnect —
so a Flex → HL2 transition in one session cannot strand it visible. Adding a
capability for it would have been duplicate machinery for a path that already
behaved. **Verify before you add a flag**; a second source of truth for the
same fact is how the two-callers-one-widget bug gets built.

### 19.7 Radio-side DSP, and why it is not one flag

The second round covered five more surfaces: NR/NB/ANF/NRL/ANFL/ANFT in the
slice VFO, the APD row, the WNB row in the ANT panel, `File ▸ Waveforms`, and
`Settings ▸ multiFLEX`. The obvious move is one flag called `hasFlexDsp`. Two
reasons it is three flags instead:

- **The name.** A field in this struct is what a *future* backend implements. A
  non-Flex radio with firmware NR should not have to set something called
  `hasFlexDsp` to say so, and §19.1 forbids the vendor name anyway.
- **The grouping.** Waveforms is plugin management and multiFLEX is session
  multiplicity. Neither is DSP. One flag covering all five would under-describe
  what it does at three of its five call sites.

So: `hasRadioSideDsp`, `hasWaveforms`, `hasMultiClientSessions`.

**`hasRadioSideDsp` is not `hasExtendedDsp`.** The latter is narrower — the extra
8000-series firmware filters (NRS/RNN/NRF) on a radio that already has the base
set. A radio with `hasRadioSideDsp=false` has neither. The test asserts they are
independent (Flex reports base-true, extended-false for an unknown model string)
so a later change cannot quietly collapse them.

Neither flag says anything about the **client-side** modules — NR2, NR4, MNR,
BNR, DFNR, RN2, and the Aetherial RX/TX EQ. Those run on this host, work on any
family, and stay available on the HL2. The proof is visual: on an HL2 the VFO's
DSP tab keeps `ADSP` and `AetherVoice` and loses every radio-side toggle, and the
`VUDU` container keeps its Aetherial EQ tiles while the `EQ` applet goes away.

**The radio's 8-band hardware EQ rides this flag too.** `EqualizerModel` emits
`eq RXsc` / `eq TXsc` — command-plane verbs, so on an HL2 the sliders moved, the
setting persisted, and the audio never changed. §17's failure shape exactly. The
`EQ` applet is hidden; the Aetherial EQ is what an HL2 uses instead, which is
also why gating the two together would have been the worst possible outcome:
the operator would lose every equalizer they have.

That is the general test for this flag — **does the control's only effect is to
emit a verb the radio's firmware executes?** If the work happens in this
application, it is not behind `hasRadioSideDsp`.

### 19.8 Two setVisible sites that disagreed, and the fix that does NOT unify them

The six VFO buttons had **two independent `setVisible()` sites** — the slice
`modeChanged` handler and `syncFromSlice()` — which is §19.2's last trap in the
wild. The fix is one owner, `applyRadioSideDspVisibility()`.

The interesting part is what it does *not* do. `updateExtendedDspVisibility()`
(the #2177 precedent three lines away) derives mode itself, which is safe because
its callers were unified once and agreed afterwards. These six have **no agreed
rule**: the `modeChanged` handler's `isVoice` carries a `!isFdv` term, so
ANF/ANFL/ANFT hide for FreeDV on that path; `syncFromSlice`'s ANF expression does
not, so they stay. Same class of drift #2177 found on DFM, still live.

Deriving mode in the new owner would have silently picked a winner. Instead each
site caches its own answer into `m_*ModeOk` and the owner only ANDs the
capability. Behaviour preserved exactly, including the inconsistency — resolving
*that* is a separate change with its own decision to make.

**The WNB row was a bare layout.** Hiding `m_wnbBtn` alone would have left its
level slider and readout floating with nothing to label them, so the row is now
wrapped in a container and hidden as a unit. It is seeded at all four
overlay-menu build sites, because those menus are created lazily as pans appear —
the same reason `applyTuningRangeToOverlayMenu()` is called from four places.

### 19.9 APD gets a second input, not a second truth

`apdConfigurable` stays the authority on whether a Flex reports the predistorter
configurable. `hasRadioSideDsp` is ANDed with it in one `updateApdVisibility()`.

This looks like it contradicts §19.6, which said APD needed nothing. It does not,
and the difference is worth stating: §19.6's check was that APD is never wrongly
*shown* on an HL2, and that held. What did not hold is *why* it held. `m_apdRow`
is constructed **visible**, and `apdConfigurable` arrives only in Flex
`TransmitDelta` status — so on a backend that never sends it, the row's state
comes from whatever the previous session left behind, not from the connected
radio. Correct by history is not correct.

Two inputs, one method, per §19.2. Note the consequence for testing: on HL2
hardware APD is hidden in **both** the connected and disconnected states, because
`apdConfigurable` is false either way. There is no A/B to photograph — the unit
test carries that one.

**Be honest about what the capability buys here: today, nothing observable.**
Under the AND, `apdConfigurable=false` already hides the row in every reachable
state, and no backend reports `apd configurable=1` while declaring
`hasRadioSideDsp=false` — that combination would be self-contradictory. So the
second input is an assertion against a future backend, not a fix for a live bug.
Recording that is the point: a capability wired in "for symmetry" that changes no
behaviour is worth *saying so* rather than letting a reader assume it closed
something.

What WAS a live bug, found by asking that question: `m_apdRow` is a QWidget and so
constructed **visible**, and neither input had fired at startup — nothing called
`updateApdVisibility()` until a connect edge or an `apdStateChanged`. A cold
launch therefore showed a live-looking APD button and Active/Cal/Avail indicators
with no radio at all, and cold start disagreed with post-disconnect, where
`resetState()` clears `apdConfigurable` and the row correctly goes away. One
`updateApdVisibility()` call at the end of the row's construction fixes it.

The general lesson: **the default visibility of a widget is a decision, and a
`QWidget` gives you `true` whether you meant it or not.** Every gated surface in
this section needs a defined state at t=0, not just a rule for what happens when
a signal arrives.

### 19.10 Testing the capability, not the family

`tests/radio_capability_gating_test.cpp` asserts capabilities only — never
`caps.family`, never a backend type. A test that asserted the family would pass
just as happily against the anti-pattern it exists to prevent.

The connected-backend half runs against SimBackend over the **synthetic demo
connection** (RFC #4288): a demo `RadioInfo` takes `RadioConnection`'s
no-socket path, so `isConnected()` genuinely becomes true with no hardware.
That matters — the `!connected ||` half of the permissive rule is only
meaningful if some case actually reaches the connected branch.

One trap in writing it, worth repeating for anything driving the seam from a
test: **do not wait on `isConnected()`.** The connection object lives on a
worker thread and reaches `Connected` before its queued signal has crossed to
the model's thread, so a `while (!isConnected())` pump exits *before* the
emission under test and the assertion fails against working code. Wait on the
signal.
**Still open on this path:** whether the HL2's in-process RX pipeline flushes a
few blocks of transmitter leakage into the start of each RX segment after unkey
— the same class as the Flex waterfall-freeze window. Measure with a recording
running across a real over before deciding it needs a hold-off.

---

## 20. Four receivers

The backend ran one DDC. It now runs up to four, each with its own NCO, WDSP
channel, spectrum, slice and panadapter. Closes backlog items 7 and 20.

### 20.1 One ADC, four receivers — the distinction that shapes everything

The HL2 has a single AD9866. "Four receivers" means four DDCs behind one
converter, so the split between what is per-receiver and what is shared is not
a style choice — it is the hardware:

| Per receiver | Shared, because there is one ADC |
|---|---|
| NCO (`0x02`..`0x08`), slice frequency | Sample rate `0x00[25:24]` — one field, so one span for every pan |
| Mode, passband, AGC | LNA gain `0x0a[5:0]`, and the dBm reference it drives |
| Spectrum, S-meter ballistics, pan frame rate | Companion filter board (J16 open-collector) |
| Demodulated audio | The transmitter, and therefore the TX slice |

Anything shared that gets stored per receiver gives four receivers four
opinions about one register, and the last writer wins silently.

### 20.2 The EP6 payload geometry is not a constant

The payload is a sequence of ROUNDS: one sample from every active receiver plus
a 2-byte mic word, so a round is `6N + 2` bytes and the per-packet sample count
**falls** as receivers are added.

At one receiver a round is 8 bytes and 504 divides exactly — which is why
`kRxSampleBytes = 8` and `kSamplesPerPacket = 126` survived as constants for the
whole single-receiver bring-up. Neither is true at two.

Rounds never straddle a 512-byte frame. The gateware emits whole rounds while
another fits and then ZERO-PADS the rest (`usopenhpsdr1.v`, `MIC0 ->
(byte_no[8:0] > round_bytes) ? RXDATA2 : PAD`). So rounds-per-frame is a floor
division and the tail bytes are not samples:

| N | round bytes | rounds/frame | payload used | padding | samples/RX/packet |
|---|---|---|---|---|---|
| 1 | 8 | 63 | 504 | 0 | 126 |
| 2 | 14 | 36 | 504 | 0 | 72 |
| 3 | 20 | 25 | 500 | 4 | 50 |
| 4 | 26 | 19 | 494 | 10 | 38 |

Decoding the pad as samples injects a burst of digital silence into every
receiver, every packet. `hpsdrsim` computes `n = 504 / size` the same way from an
independent codebase, which is what makes it evidence rather than an echo.

**EP2 is a different, receiver-count-INDEPENDENT layout** — a fixed 126 samples
of 8 bytes whatever N is. It was sharing the EP6 constants; they are now
separate (`kTxSampleBytes`, `kTxSamplesPerPacket`) so adding receivers cannot
reshape the transmit packet or move its pacing.

### 20.3 The receiver count field is FOUR bits

`0x00[6:3]`, `0000`=1 to `1011`=12. The encoder masked with `0x07`, which capped
the encodable count at 8 and would have wrapped 9..12 into 1..4 — a request for
nine receivers configuring the radio for one. Latent while only one ran.

### 20.4 The link budget is a real limit, not a footnote

Both axes cost bandwidth: more receivers shrink the per-receiver payload of a
fixed-size packet, so the radio sends more packets. Sustained EP6 wire rate,
Mbit/s, including UDP/IP/Ethernet headers and the inter-frame gap:

|  | 1 RX | 2 RX | 3 RX | 4 RX |
|---|---|---|---|---|
| 48 k | 3.3 | 5.9 | 8.4 | 11.1 |
| 96 k | 6.7 | 11.7 | 16.9 | 22.2 |
| 192 k | 13.4 | 23.4 | 33.7 | 44.4 |
| 384 k | 26.8 | 46.8 | 67.5 | **88.8** |

**The HL2's ethernet is 100BASE-T.** Four receivers at 384 kHz does not fail
cleanly — the link drops packets, and a dropped EP6 packet is a simultaneous gap
in *every* panadapter. `maxReceiversAtRate()` admits 4 receivers through 192 kHz
and 3 at 384 kHz, at a 70% budget. The reported zoom LIMITS shrink with the
receiver count, so the operator cannot reach a span that would then be refused —
a refused control reads as broken, an absent one reads as a limit.

### 20.5 Agree-or-bypass on the shared filter board

One relay bank, four receivers, four possible opinions.

If every active receiver wants the same filter, engage it. If they disagree,
release every relay rather than pick a winner. Picking a winner is the tempting
alternative and it is worse: a low-pass chosen for 40 m *attenuates* a receiver
on 15 m, so three panadapters would show a level that is an artefact of the
fourth receiver's tuning. Bypass is honest — every receiver sees the same front
end, and a level comparison between panes means something.

**What it costs, stated plainly:** bypass drops the AM-broadcast HPF, which
matters more here than on radios with better dynamic range (oracle §8). Near a
broadcast transmitter, spanning bands can raise the noise floor on every
receiver. The log line names the spanning frequencies and says the HPF is out,
because "why did my noise floor rise when I opened a second receiver" is
otherwise an unanswerable support question.

Measured on the simulator, the full round trip:

```
band filter: 0x48 (HPF + 30/20m LPF) for 10.000000 MHz, trigger=connect
band filter: 0x00 (none (bypass)) for receivers spanning bands
             (7.200, 10.000, 10.000, 10.000 MHz) — BYPASSED, AM-broadcast HPF
             is out — was 0x48, trigger=tune
band filter: 0x44 (HPF + 60/40m LPF) for 7.200000 MHz — was 0x00, trigger=tune
```

**While KEYED the transmit receiver's filter wins outright.** Radiating through a
bypassed bank because another receiver was parked elsewhere would put harmonics
on the air, and no receive-side convenience justifies that.

### 20.6 Transmit stays singular

One transmitter, however many receivers. Exactly one slice reports
`txSlice=true`, and the TX NCO, mode and passband follow *that* receiver.
Tuning receiver 3 must not drag the transmit frequency; putting it into CW to
chase beacons must not switch the transmitter out of SSB.

Marking every slice as the TX slice would be worse than marking none: RadioModel's
interlock would find one whichever pane was selected, and the operator could key
from a receiver the TX NCO is not following.

The operator can MOVE it (the VFO panel's TX indicator, `IRadioBackend::
setTxSlice`), and the same singularity applies to the ACTIVE slice — see 20.13,
where publishing either one unconditionally is what actually went wrong.

### 20.7 Host-side audio mixing

A Flex sums its slices on-radio and sends one stream. An HL2 demodulates every
receiver on this host, so the sum is ours.

Receivers share an input clock (one EP6 packet feeds them all) but WDSP's worker
is asynchronous, so blocks are mixed `min()`-aligned rather than smeared. A
**starvation guard** mixes a stalled receiver as silence past ~85 ms: without it
one stalled DSP holds `min()` at zero and the whole radio goes silent, which is
strictly worse than the fault it reacts to.

Clamped, not scaled by 1/N. Dividing would make every slice quieter the moment a
second one is opened, which an operator reads as the radio going deaf.

### 20.8 Ordering: DSP chains are built BEFORE start()

Opening a WDSP channel costs ~19 s on a first run (FFTW wisdom) and runs on the
I/O thread — **the thread that paces EP2**. Configuring after `start()` stalls
the pacer for all of it, and the gateware watchdog halts the stream when EP2
stops arriving; it also stalls the EP6 reader, so the connect watchdog can time
out against a radio that is answering perfectly well.

This was caught by `hl2_backend_test`, which stopped seeing `connected()` at all.
It is the same lesson as §15.4 from the other direction: EP2 is not best-effort.

The count therefore comes from a **static** `MetisClient::effectiveNumRx(Params)`
— the same clamp the running client applies to the same struct. The demux and
the radio must not disagree about how many receivers exist, because the EP6
payload carries no receiver-count field and a mismatch reinterprets every round
with no error anywhere.

### 20.9 Seam gap this exposed: pan-id namespaces

`RadioModel` materialised exactly one panadapter for a non-Flex backend —
`ensureOwnedPanadapter(neutralPanIdString(0))`, hardcoded in three places — and
kept the backend pan geometry in two scalars.

Worse, every *other* backend pan signal resolved the raw backend pan id through
`resolvePan()`, whose fallback is the ACTIVE pan. With one pan that looked
correct, because the only pan was the active one. With four, every pan-addressed
update landed on whichever pane happened to be selected: **RF gain reported 20 dB
on one pan and 0 on the other three, from a single radio-wide LNA.**

Backend pan ids are now translated through a first-seen-order allocator and stay
OPAQUE in both directions — `RadioModel` does not parse a family's naming scheme,
and a backend does not learn about the `0xE1000000` stream-id space.

### 20.10 The dynamic lifecycle: receivers come and go while the radio runs

The count was fixed at connect, from a persisted setting. It is now the
operator's, at runtime: "Add Panadapter" and the pane close button. Connect
always comes up with ONE receiver.

Retiring the persisted count mattered for a reason beyond tidiness: it made
connect the only place the count could change, and a saved 4 was re-imposed on
every connect even at a span that could not carry it.

Changing the count RESTARTS the EP6 stream, deliberately. The count changes the
payload layout (`6N+2` bytes per round) and the packet carries no
receiver-count field and no marker for the packet where the change took effect.
Re-sending the config bank alone leaves a window of milliseconds in which the
radio has switched layouts and the host has not, and every round in that window
is misread on EVERY receiver with nothing reporting an error. `metis-stop` /
reconfigure / `metis-start` makes it a hard edge; the brief audio gap is what
adding a receiver looks like.

Two things must be re-asserted after a removal, because nothing reads them back:

- **Every surviving receiver's NCO.** They may have moved down a hardware slot
  and the NCO registers are addressed by that slot.
- **Transmit ownership**, if it lived on the closed receiver.

### 20.11 Identifier allocation after a removal — the same bug three times

This is the most transferable lesson in the section. Three separate allocators
picked *the next* identifier instead of *the lowest free* one, which is only
equivalent while nothing is ever removed. Each failed differently and none
failed loudly.

| Allocator | Wrong rule | What it produced |
|---|---|---|
| Receiver UI number (= seam slice id) | monotonic counter | after 4 open / 3 closed, asked for slice id 4 on a radio whose ids run 0..3 → *"Slice capacity is full"* |
| Neutral pan index in `RadioModel` | `m_backendPanIndex.size()` | closing 1 of 4 leaves size 3 while index 3 is live → new pan resolved to an EXISTING `PanadapterModel`, no pane created, occupant's frames taken over (`pans=3 slices=4`) |
| TX / active-slice role indices | kept the stored DDC index | `remove()` renumbers, so closing DDC 0 of three left transmit naming index 2 — now a different receiver |

The UI-number case is worth dwelling on because the wrong choice was
*deliberate*. The reasoning was "reusing a retired number would give two panes
the same identity" — which is false, because the retired pane does not exist.
The cost of that reasoning was real: the UI number IS the seam's slice id, and
that space is bounded by the radio's slice capacity. Reuse is also what a Flex
does with its own slice ids, so it matches what every consumer above the seam
was written against.

**The rule:** an id space with a bounded range and a remove operation must
allocate lowest-free. `size()` and `++counter` are both wrong, in opposite
directions.

Related and separate: DDC indices MUST be renumbered on removal (the gateware
streams `numRx` contiguous receivers and the index is the slot in the EP6
round), while UI numbers and pan ids must NOT be. `hl2RoleAfterRemove()` states
the three-case rule once — below the removed index, above it, or on it.

### 20.12 A closing receiver retires its slice AND its pan

`removePanadapter` emitted only `panRemoved`. The `SliceModel` outlived its
receiver, still naming a pan id that no longer existed, and `slices().size()`
never fell — so the next create failed a capacity guard against a stale count.

On a backend where a slice IS a receiver, both go. `IRadioBackend::sliceRemoved`
already existed for the Flex path; it simply had no HL2 emitter.

**Invariant worth asserting directly: pan count and slice count move in
lockstep on this backend.** Every bug in 20.11 and 20.12 shows up as those two
numbers disagreeing.

### 20.13 Exactly one: the singular roles

Two roles are singular, and publishing them unconditionally was correct while
there was one slice and wrong at two.

- **`txSlice`** — one transmitter. Marking every slice as the TX slice is worse
  than marking none: the interlock finds one whichever pane is selected, and the
  operator can key from a receiver the TX NCO is not following.
- **`active`** — one selected slice. `d.active = true` was unconditional, so
  every slice claimed it. Two slices claiming to be active is
  indistinguishable from none: tuning across a panadapter moved the right DDC
  and showed the right frequency on its VFO flag, while the RX Controls applet
  stayed pointed at a different receiver.

A Flex arbitrates both on the radio and ECHOES the deselection back. Nothing
echoes here, so the backend has to clear the previous holder itself and
republish BOTH the old and the new slice. Keep them separate: listening on one
slice while transmitting on another is routine, so selecting a pane must not
drag transmit with it.

### 20.14 The recurring failure shape: Flex wire text on a seam backend

Every remaining multi-DDC defect this session had the same shape, and it is
worth naming because it will recur for every feature added from here.

A control is implemented as Flex wire text (`slice set N active=1`,
`display panafall create`, `slice set N audio_mute=1`). On a host-demodulating
backend that text goes nowhere. The control reports success, the model updates,
and nothing happens to the radio.

The variant that hurts most is **wire text with a completion callback**:

```
createPansSequentially()  -->  sendCmdPublic("display panafall create", cb)
                                   cb never runs on a seam backend
                                   ...so the recursion driving it stops dead
```

That is why "Add Panadapter → pick a layout" created nothing on an HL2 while
the bridge's `pan create` worked throughout — the verb goes through
`RadioModel::createPanadapter()`, the dialog talked to the connection directly.
One entry point was wired to the seam and the other was not.

Found this way, all fixed the same way (route through the seam / the
`SliceModel` setter): per-slice mute, level, balance; TX-slice selection;
active-slice selection; pan creation from the layout dialog; the bridge's
`slice select`; both TCI guards.

**When adding any control, the question is not "does it work?" but "which of the
two paths did I test?"** A passing bridge verb proves nothing about the button,
and vice versa.

Two authority bugs of the same family: `maxPanadapters()` and `maxSlices()` read
`capabilitiesFor(m_model)`, a FlexLib platform table keyed by model string.
"Hermes-Lite 2" fell through to a 2-pan default and refused a third receiver on
a board reporting four. `TciServer`'s own comment had recorded the consequence
and it was still true.

### 20.15 Certification targets — invariants, and the bridge verbs that check them

Written as propositions rather than steps, because each one is a defect this
session actually produced. All are reachable from the automation bridge with a
simulator; none needs hardware.

**Lifecycle and counting**

1. Connect yields exactly one receiver, with no settings file.
   `get radio` → `panCount == 1 && sliceCount == 1`
2. `panCount == sliceCount` after EVERY create and remove. The single most
   productive assertion in this section — 20.11 and 20.12 all violate it.
3. Adds are refused at the board's reported count, not a model-string default.
   `pan create` × N → the (N+1)th returns `ok:false`, and the limit in the
   message equals discovery byte `0x13`.
4. Closing the last receiver is refused.
5. Freed ids are reused: open 4, close 3, reopen → slice id 1, not 4.
6. Closing the MIDDLE of four then reopening fills the gap, and the survivors
   keep their own numbers. This is the case that caught two separate bugs.
7. Add/close repeated ~10× leaks no WDSP channel (the pool is 32, shared with
   transmit).

**Singular roles**

8. Exactly one slice has `txSlice: true`; exactly one has `active: true`.
   `get slices` → both counts are 1, at every point in a lifecycle sequence.
9. `slice tx N` moves transmit and CLEARS the previous holder.
10. `slice select N` sets active and clears the previous holder.
11. Closing the receiver that owns transmit moves transmit to a survivor,
    and `get slice tx` still resolves.

**Shared hardware**

12. `pan rfgain <panId> <dB>` changes RF gain on EVERY pan, not the addressed
    one. One AD9866.
13. Receivers on one band → filter engaged, `wide: false` on all pans.
    Receivers spanning bands → `wide: true` on all pans, filter bypassed.
14. Span is radio-wide: a zoom on one pan reports the same `bandwidthMhz` on all.

**Link budget**

15. Four receivers refuse 384 kHz; three accept it. The reported zoom limits
    shrink with the receiver count, so a refused span is unreachable from the UI.

**Surviving the restart** — every create and remove restarts the EP6 stream
(`MetisClient::setReceiverCount`), which is the least obvious thing in this
section and the easiest to regress.

16. Adding or closing a receiver does NOT drop the link. `get radio` reports
    connected throughout, and `get_log` shows no `linkDown`. The stream restart
    re-sends metis-start, that datagram is as losable as the one at connect, and
    without a retry one lost packet ends the session ~2 s later on the silence
    watchdog. Covered in-tree by `hl2_receiver_count_restart_test`.
17. A restart is not a reconnect. Exactly one `connected()` per session — a
    spurious one makes RadioModel stage every pane as previous-session leftovers
    and rebuild the operator's layout mid-click.
18. After ~10 add/close cycles, every surviving receiver still produces spectrum
    and audio. Renumbering is what breaks here: closing the middle of three
    renumbers every DDC after it, and a chain wired to an index rather than a
    stable id goes quiet with nothing logged.

**Both paths, every time**

19. For each control, exercise the GUI widget AND the bridge verb. The layout
    dialog is modal — a synthetic click needs a settle delay before the tile
    click, or the second click lands on whatever is behind the dialog and the
    button looks dead.

**Bridge verbs currently sufficient for the above:** `connect`, `pan
create|remove|rfgain`, `slice select|tx`, `tune`, `get radio|slices|pans|slice`,
`invoke`, `clickAt`, `dumpTree`, `get_log`. Gaps worth adding when these become
real certification cases: a verb for per-slice mute/gain/balance (today only
reachable by clicking the applet); one for pane pop-out / maximize; and a
`capture_audio` window straddling a slice mute, which is the only way to check
the mixer's continuity claim (§20.7) from outside — the drain leaves residue in
the deeper queue, and the single-contributor fast path has to emit it rather than
discard it, which no in-tree test currently observes.

### 20.15.1 Two concurrency traps the receiver set introduced

Both were found in review of this work, not on the air. Both are the kind that a
passing test suite says nothing about, so they are recorded as shapes to look for
rather than as fixed bugs.

**The sample path must not read a GUI-thread container.** The EP6 fan-out started
as a loop over `m_rx`, dereferencing `m_rx[i].dsp` for every arriving packet on
the I/O thread. That is fine with a fixed receiver set and wrong the moment one
can be added or closed: `createPanadapter()`'s `push_back` reallocates and
`removePanadapter()`'s `erase` shifts, either of which can free or move the
storage under a fan-out that is halfway through it — a use-after-free on the
real-time audio path, reachable by clicking "Add Panadapter" while the radio
streams.

The fix that looks obvious is to synchronise the access. Do not: the I/O thread
is reached from `createPanadapter()` by a `Qt::BlockingQueuedConnection`, so a
lock the I/O thread also wants is a deadlock rather than a race. Ordering the two
through the event loop instead does work — one event loop never runs two
callbacks at once — but it leaves the sharing in place for every future reader to
rediscover, and see the next paragraph for why it cannot be verified.

What is in the tree is neither: the I/O thread gets its **own** list
(`m_ioDsps`), rebuilt by `publishIoDsps()` only when the receiver set changes.
`m_rx` is GUI-thread-only, `m_ioDsps` is I/O-thread-only, and there is no shared
state left to order. The ordering rule that remains is a lifetime one — withdraw
a chain from the published list *before* destroying it, never after — and
`publishIoDsps()` blocks for exactly that reason.

**ThreadSanitizer cannot see through Qt.** QtCore ships uninstrumented, so the
happens-before edge a `Qt::BlockingQueuedConnection` establishes (a `QSemaphore`
inside QtCore) is invisible to TSan. Every blocking invoke therefore reports the
callee's read of the caller's captures as a data race, with `QtCore` frames
printed as `<null>`. This is pre-existing and abundant, not new:
`hl2_backend_test`, which predates the multi-receiver work, reports 57 races
under `-fsanitize=thread`, 32 of them in the queued-functor dispatcher.

Two consequences worth carrying forward:

- **A Qt-synchronised fix is unfalsifiable here.** The event-loop-ordering
  approach above was implemented first and TSan reported the receiver vector as
  raced *because of the fence* — the fence had moved a same-thread write onto
  another thread, and TSan could not see the semaphore ordering them. Preferring
  "no sharing" over "synchronised sharing" is what made the result checkable.
- **Read the differential, never the count.** The useful measurement was
  `grep -c 'vector<...::Receiver'` over the TSan log across the two designs: 8
  frames naming the receiver vector before, 0 after, with the pre-existing
  blocking-invoke reports unchanged either side. The absolute count is dominated
  by the Qt artifact and says nothing.

`tests/hl2_receiver_churn_test.cpp` exists to give this a place to be seen: it
is the only test that adds and closes receivers against a live EP6 stream, which
is the contended window. It passes on a plain build regardless of the fix, and
that is worth being blunt about — a use-after-free on a four-element vector
usually reads memory the allocator handed straight back. Its value is under a
sanitizer, and it reports 30 of the pre-existing Qt artifacts when run there.

### 20.16 What is proven, and what is not

**Proven against `hpsdrsim -hermeslite2 -P1`:** four receivers configured and
streaming; four independent NCOs; four panadapters with their own spectrum and
waterfall; agree-or-bypass across the full round trip; RF gain consistent across
all four pans.

Added since, all through the automation bridge: connect at one receiver with no
settings file; the statusbar button and a real layout tile creating three more;
the fourth add refused at the board's reported count; close, and close-the-last
refused; open 4 / close 3 / reopen giving back id 1; close-the-middle then reopen
filling the gap; `panCount == sliceCount` at every step; exactly one `txSlice`
and one `active` slice; `slice tx` and `slice select` each clearing the previous
holder; RF gain on one pan moving all pans; WSJT-X's TCI split creating a second
DDC and moving transmit to it; and two TCI clients on RX1/RX2 both receiving
per-slice audio, including with one slice speaker-muted. 196/196 tests green
(`hl2_tx_loopback_test` was excluded at the time; it has since been fixed and no
longer needs excluding — see below).

**Proven on real hardware.** The operator has since exercised this end to end on
a Hermes-Lite 2, transmit included. That closes the gap the simulator
structurally cannot: it generates its scene independently of the NCO, so four
receivers on four frequencies show the same synthetic content there — meaning
"four DDCs genuinely tune independently" was, until hardware, argued from the
register map (`0x02`..`0x08`) and confirmed only in that the radio accepted the
writes.

Keep that distinction in mind when reading any simulator result in this section:
`hpsdrsim` failing to contradict a convention is not the same as hardware
confirming it (§7, and the wrong-sideband account in §14.6).

**Still open:**

- ~~**`hl2_tx_loopback_test` fails against the simulator** on transmit-sideband
  checks, non-deterministically.~~ **FIXED.** Correctly diagnosed as pre-existing
  — commit `256142a6` failed identically — but it was never a transmit fault at
  all. The test asserted that the fed-back tone appears BELOW centre, which was
  right against the pre-#4471 panadapter, when the display carried the raw wire.
  #4471 added the receive-side conjugation that fixed the mirrored panadapter,
  and from then on the same tone correctly read ABOVE centre while the test went
  on expecting the old sign. Two supposed symptoms are also explained: the
  "non-determinism" was the hardcoded `192.168.1.12` reaching a simulator on a
  *different* machine, so results tracked whatever that machine was doing; and
  the check that "passed sometimes" did so only when that other simulator was in
  a different state. The test now defaults to loopback, refuses to key anything
  whose discovery MAC is not hpsdrsim's synthetic `AA:BB:CC:DD:xx:FF`, asserts
  the analytic sign, and anchors the receive end on the simulator's own scene
  tones first (§14.6). It also skips when the simulator reports it is already
  streaming to somebody else (discovery status `0x03`), which is the collision
  clause above finally fixed in code rather than in a warning. Its skip paths
  exit 77 with `SKIP_RETURN_CODE` set, so the ordinary machine — no simulator
  running — reports `Skipped` rather than a `Passed` that measured nothing.
- **The link-budget ceiling is derived, not measured.** 70% of 100BASE-T is a
  working figure. Where the drop counter actually starts moving is still a
  number nobody has written down, and it is the one that would justify or move
  `kEp6LinkBudgetFraction`.
- **The skimmer gateware variants** (9–12 RX, no transmit) are still untested.
  `kMaxTunableRx = 7` bounds us to the contiguous `0x02`..`0x08` NCO run;
  RX8..RX12 at `0x12`..`0x16` are deliberately not encoded.
- **CPU cost is unmeasured beyond one observation:** 54.7% on an M-series laptop
  at 4 × 192 kHz with four panadapters rendering. That is a whole-app number from
  the status bar, not a profile.
- **None of it is automated yet.** Hardware verification does not survive a
  refactor; §20.15 lists the invariants to turn into certification cases so that
  it does.

## 21. The network readouts, on a radio with no command plane

The heartbeat dot in the title bar sat amber for a whole HL2 session. The
status-bar `Network:` field was blank. Opening Network Diagnostics on a radio
that was streaming 12.8 Mbps flawlessly showed 0 kbps, 0 packets, 0 bytes, and
`Off`.

None of that was a bug in those readouts. Every one of them was reading the
Flex stack directly.

### 21.1 The shape of the problem

Three widgets, one root cause:

| Readout | Source it read | On an HL2 |
|---|---|---|
| Title-bar heartbeat | `RadioModel::pingReceived`, emitted from the TCP `ping` reply | never fires |
| Status-bar `Network:` | `networkQualityChanged`, emitted from `evaluateNetworkQuality()` | early-returned on `!m_panStream` |
| Diagnostics pane | `RadioModel` getters, each `m_panStream ? … : 0` | structural zero |

`startNetworkMonitor()` is called from exactly one place — the Flex `client
gui` reply handler. A family that reaches `onConnected()` through the
`IRadioBackend` seam never runs it, so the ping timer never starts, the score
is never computed, and `m_netState` stays `Off` for the life of the session.

The fix is not to make HL2 answer pings. It has nothing to answer them with
(§21.3). The fix is that **the counters have to come from whoever owns the
socket**, which is the backend.

### 21.2 `IRadioBackend::LinkStats` — the seam addition

A neutral transport snapshot, published on a fixed cadence:

```
MetisClient::LinkCounters   (I/O thread, published ~1 Hz from the receive path)
  → Hl2Backend              (mirrored onto the GUI thread, like m_drops already was)
    → linkStatsUpdated()    (fixed 1 Hz timer — NOT the receive path)
      → RadioModel          (feeds the SAME scorer the Flex path uses)
```

Three decisions in there are load-bearing:

**`reported` defaults to false.** A backend that does not override
`linkStats()` sends nothing, and every consumer keeps its existing source. That
is what makes this additive — the Flex path never sees a `LinkStats` at all,
and `usesBackendLinkStats()` is `!m_panStream && m_linkStats.reported`, so both
halves must be true before any fallback engages.

**The publish timer lives in `Hl2Backend`, not in `MetisClient`.** The tick has
to keep coming *after the radio goes quiet*, because "nothing arrived this
second" is the observation the heartbeat's alarm path is waiting for. A
backend that emits only on receive can never report its own silence. So
`MetisClient` publishes counters from the receive path (and stops when EP6
stops), and `Hl2Backend`'s own timer turns the frozen counter into
`alive = false`.

**`alive` is a per-tick difference, not a cumulative total.** `rxPackets`
alone cannot distinguish a dead link from reading the same counter twice.

### 21.3 There is no RTT, and saying so is the whole point

Protocol 1 is a one-way stream. EP2 goes out on a wall clock, EP6 comes back
free-running, and **no frame in either direction answers a specific frame in
the other**. There is no round trip to time.

The trap is that `lastPingRtt()` answers `0` when nothing measured it, and
`formatNetworkMs(0)` renders `< 1 ms`. Left alone, the diagnostics pane would
have advertised the best latency the app can display, from a measurement that
never happened — and the *chart* would have drawn a confident flat 0 ms trace
under the real ones, which is more believable than the number.

So `LinkStats::rttMs` is `-1` for "not measured", `RadioModel::hasLinkRtt()`
is the predicate every RTT surface asks first, and there are four of them:
the status-bar tooltip, the Connection Details rows, the Latency tile, and the
latency *series* — which is omitted from the graph rather than zeroed.

`hasStreamCategoryStats()` is the same idea for the per-stream Audio/FFT/
Waterfall/Meter/DAX breakdown. That is a property of the VITA-49 multiplex,
where each category is a separately-sequenced stream sharing one socket. A
transport carrying everything in one stream has no such split, and five rows of
`0 / 0 packets` read as five dead streams rather than as a distinction that
does not apply.

`hasLinkTiming()` is the third, and it exists because a backend is `reported`
from its *first* tick on purpose — otherwise the consumer's first second keeps
the Flex sources and renders the blank readout this whole path exists to fix.
But `MetisClient` only fills the gap figures when a publish window closes with
samples in it, so for about a second `gapMs` is `-1` while `reported` is already
true. The model clamps that `-1` to `0` so the charts stay numeric, and `0` is
`< 1 ms` again. Same trap, one field over.

**The rule this generalises to:** when a readout is ported to a transport that
cannot produce one of its figures, the figure must render as *absent*, not as
zero. A measurement of nothing and the absence of a measurement are different
claims about the link, and only one of them is true.

**And the corollary that is easy to miss:** these predicates answer for the
*wire*, not for the moment. `stopNetworkMonitor()` drops the snapshot on
disconnect — those counters belong to the session that ended — but "this
transport has no round trip to time" stays true while the transport is down. A
predicate derived from the live snapshot alone flips back to the Flex default
the instant the session ends, and the pane an operator left open goes from
`n/a` to `< 1 ms` on a radio that is not even connected. `RadioModel` latches
the transport's *shape* (`BackendLinkShape`) separately from its counters, and
the shape dies with the backend, not with the session.

### 21.4 What IS measurable, and how it is scored

Delivery timing, and it is timed **once per socket wakeup, not once per
datagram**. A wakeup drains everything queued behind it, so successive
datagrams inside one drain are microseconds apart no matter how badly the link
is behaving — per-datagram timing reports a rock-steady 0 ms straight through a
stall that silenced the audio.

From that: `meanGapMs` and `maxGapMs` over the publish window, and jitter as
their difference — the *spread* of delivery. On a healthy link that is a
fraction of a millisecond; a congested one stalls and resumes, which is exactly
what the operator hears. Sequence gaps come from the EP6 counter that already
existed.

Those feed `evaluateNetworkQuality()` unchanged, so `Fair` means the same thing
to the operator on either radio.

### 21.5 A Flex bug the HL2 work exposed

`stopNetworkMonitor()` set `m_netState = Off` and emitted nothing. The
status-bar field is written *only* from `networkQualityChanged`, and every
emitter of that signal hangs off the ping/transport path being torn down — so
the last quality the link ever had stayed on screen after disconnect. A
disconnected radio reading `Excellent`, until the next connection happened to
overwrite it.

Pre-existing on the Flex path, invisible there only because nobody looked at
the field after disconnecting. It is fixed for both families in
`stopNetworkMonitor()`.

### 21.6 Verified on hardware

Against the HL2 at 192.168.1.21, RX-only:

- Heartbeat samples `#20c060` green at ~1 Hz with idle grey between — no amber.
  (Sampled by grabbing the title bar 40× at 50 ms and reading the dot pixel;
  the green flash is a 100 ms window in a 1 s beat, so a single screenshot
  catches it about one time in ten.)
- Status bar `Network: [Excellent]` while connected, `[Off]` after disconnect.
- 353 544 EP6 packets, 0 sequence gaps, 12.8 Mbps RX / 3.2 Mbps TX, arrival gap
  1 ms, RTT `not measured on this link` in all four places.

`hl2_link_stats_test` covers the seam contract, including the silent-link case
and the honesty invariant. `hl2_link_stats_model_test` covers the half that is
downstream of it, because the backend can publish a perfect snapshot while every
readout still lies: it drives a real `RadioModel` against a fake HL2 and pins the
readouts leaving their structural zero, the predicates on **both** sides of the
disconnect edge, and the per-session reset the two scoring-session entry points
share. Per §7 that is necessary and not sufficient — but the RTT and category
predicates are decisions about what we *refuse* to claim, which is one of the few
things a fixture can check honestly.

---

## 22. The first connect: 19 seconds of a dead application

The symptom an operator reports is "the app hangs when I connect a Hermes-Lite
for the first time." It is not a hang and it is not the radio. It is FFTW
measuring plans, on the GUI thread, with nothing on screen to say so.

### 22.1 What was actually measured

Driven through the automation bridge against `hpsdrsim -hermeslite2 -P1`. The
bridge's socket handler runs on the GUI thread, so pinging it every 100 ms from
another thread turns "is the UI frozen" into a number: a gap between replies IS
a frozen UI.

| | Before | After |
|---|---|---|
| Cold-cache connect | 21–82 s (load-dependent) | 20.2 s |
| Bridge pings answered during it | 0 of ~200 | 219 of 219 |
| Longest unresponsive gap | 58–82 s | 0.50 s |
| Wisdom on disk after SIGTERM | none | 9117 bytes |
| Next launch, same profile | 21.6 s **again** | 0.57 s |

### 22.2 Two defects, not one

**The GUI thread waited.** `RadioModel` calls `Hl2Backend::connectRadio()`
synchronously, and that method drove every `Hl2RxDsp::configure()` through
`Qt::BlockingQueuedConnection`. The work was correctly on the I/O thread; the
GUI thread simply stood there for all of it. Split into three phases now — see
the comment above `beginDspSetup()`. The §20.8 ordering is untouched: chains
still open before `start()`, serially, on the I/O thread.

**The wisdom was never saved.** Export was a `std::atexit` handler, and
`Hl2EmergencyStop` restores `SIG_DFL` and re-raises — so an unhandled signal,
which is what SIGTERM, a crash and a Force Quit all become, never runs it.
Measured: connect, kill, relaunch, connect — full cost a second time, cache file
never created. **Anyone who had ever force-quit was paying first-run cost on
every run**, which is why a one-time expense felt permanent. Export now happens
at the end of `open()`, and it is write-then-rename: the direct write truncated
a shared cache when two processes exported at once (a 48 KB cache came back as
13 KB after a `ctest -j8` run, and FFTW rejects a short file wholesale).

### 22.3 The planning cost is one-time per MACHINE, not per rate

Measured cold, both orderings, with `WdspChannel` opened exactly as
`Hl2RxDsp::configure` builds it:

| Order | 1st open | 2nd | 3rd | 4th |
|---|---|---|---|---|
| 48 → 96 → 192 → 384 kHz | **18865 ms** | 100 ms | 71 ms | 39 ms |
| 384 → 192 → 96 → 48 kHz | **18666 ms** | 64 ms | 99 ms | 175 ms |

The first open costs ~19 s whichever rate it is; every other rate afterwards is
40–175 ms. The plan sets overlap almost completely, so **do not reason about
"cold wisdom for this sample rate"** — there is one cold open per machine, ever.

### 22.4 Still open: two paths that still block the GUI thread

#### Backend teardown waits out an in-flight build

`~Hl2Backend()` stops the wire through a `Qt::BlockingQueuedConnection` before
joining the I/O thread, and `beginDspSetup()`'s opens are a single event on that
thread's loop. So a teardown during a cold connect — a **family switch**
(`RadioModel::connectToRadio` → `teardownBackend()`) or an **app quit** — blocks
the GUI thread for whatever is left of the planning.

Splitting the connect is what made this reachable, and that is not an argument
against the split: before it, the connect itself held the UI, so nobody could
get to the radio picker mid-connect. Now the UI is live for those twenty
seconds, and reaching for a different radio is the obvious thing to do while
waiting. `OpenChannel` cannot be cancelled, so the honest options are a busy
state over the teardown or a backend that can be abandoned rather than joined —
and the second one also has to replace the `QPointer` guard in
`beginDspSetup()`, which is sound today *because* teardown blocks.

#### The span change rebuilds every receiver

`applyPanBandwidth()` has the same shape the connect used to have. Crossing one
of the four rate boundaries rebuilds **every** receiver (the rate register is
radio-wide) through `Qt::BlockingQueuedConnection`, and each receiver costs an
open (40–175 ms, per §22.3) plus a close, which flushes and is bounded by WDSP's
own 100 ms timeout. Four panadapters open is roughly **0.6–1.1 s of frozen UI
per boundary crossing** — the "chunky zoom" operators describe, layered on top
of two things that are not bugs:

- **The span IS the sample rate.** Four rates only, snapped log-nearest, so the
  boundaries sit near 68 / 136 / 272 kHz. Zoom within a rate is free display
  scaling; crossing one re-spans the radio, and `emitAllPanState()` deliberately
  snaps the widget back to the span that was actually granted.
- **A 150 ms coalescing throttle** (`kBandwidthThrottleMs`, #4470), because a
  drag delivers ~30 span changes a second and each one is a full rebuild.

The three-phase split from §22.2 is directly reusable here, but the rate change
has ordering constraints the connect does not: the DSP must expect the new rate
before EP6 starts delivering at it, and a partial failure has to roll every
receiver back to a single rate. Left as a follow-up rather than bolted onto the
connect fix.
