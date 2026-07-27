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
the design note and `prototypes/hl2/README.md` carry the correction.)

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

### AGC

- `SetRXAAGCTop` is the **maximum gain in dB**, and 120 dB is the top of WDSP's
  range. Inheriting that default ran the HL2 wide open: peak 3.186, **10.31% of
  samples at or beyond full scale**. At a 65 dB ceiling: peak 2.664, 0.27%.
- Mode vocabulary: `off/slow/med/fast` → WDSP RXA 0/2/3/4. WDSP's "long" (1)
  has no representation in the four-way UI control.

---

## 6. Seam gaps found (the reusable checklist)

Each of these is "a Flex assumption that a DSP-owning backend violates".

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
| 10 | `AETHER_AUTOMATION_NO_AUTOCONNECT` appears not to suppress autoconnect on the HL2 path | Test instance grabs a radio | *Open* |
| 11 | `SpectrumWidget` **drops** inbound pan geometry during a gesture, assuming another status is coming | View parks at the old centre while slice/pan/waterfall move — measured **permanently 6.3 kHz** out after one drag-tune | `3d52d07d` |

| 12 | Slice frequency WAS the DDC NCO, so the pan centre tracked every tune | Display re-centred on every click; a slice offset from centre was unrepresentable | `a1cbe154` |
| 13 | RX filter set via `SetRXABandpassFreqs` alone, leaving the NBP stage — the filter actually in circuit — untouched | No sideband selection and no filtering AT ALL; 0 dB rejection of a tone outside the passband | `86a3d27b` |
| 14 | HPSDR wire IQ handedness is opposite to WDSP's | USB demodulated the lower sideband and LSB the upper — audibly swapped, while the panadapter looked correct | `79c54266` |
| 15 | AM in neither filter-polarity family | Switching to AM kept an SSB passband that filters the carrier OUT, so the envelope detector distorts rather than going quiet | `2996f0eb` |

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
AETHER_AUTOMATION=1 AETHER_AUTOMATION_NO_AUTOCONNECT=1 \
AETHER_AUTOMATION_SOCKET=aethersdr-hl2 \
./build/AetherSDR.app/Contents/MacOS/AetherSDR
```

- Launch the app as the **foreground process of a backgrounded shell**;
  launching it with `&` inside a foreground command gets it killed with the
  shell's process group.
- First WDSP channel open costs **~17 s** generating FFTW wisdom; subsequent
  connects are **~110 ms**. Not a bug — don't "fix" it.
- The `prototypes/hl2/` Python spike defaults to broadcasting
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
already measured a 17-second main-thread stall on first connect (FFTW wisdom).
That one lands before the stream starts, but it proves the class exists, and at
384 kHz the DSP shares the same thread. Moving the DSP off the GUI thread was
always "a later refinement"; the watchdog turns it into a correctness issue.

### 11.4 Absent subsystems, in rough value order

| Missing | Why it matters |
|---|---|
| RQST/ACK state machine (§5) | Gate for everything below it. Single outstanding request, no transaction id, echo-matched. Do NOT model as RPC |
| ADC overload bit + clip counter (§6) | Addendum 2 §A3: the CORRECT driver for any gain decision. Audio level in one slice says nothing about what saturates a converter seeing 0–38.4 MHz |
| Discovery telemetry (§1) | Temperature, power, clip count, PTT are pollable WITHOUT a stream — cheapest possible first increment, and a diagnostic when the stream itself is broken |
| Receiver count at discovery `0x13` | We hardcode `maxSlices = 1`. Standard gateware is 4; skimmer variants 9–12 with NO transmit |
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
- **First-run FFTW planning is slow BY DESIGN** (§9). Our measured 17-second
  first connect is expected behaviour, not a performance bug. The oracle's
  prescription is a progress indicator, not optimisation. That closes an open
  question from earlier in the session.

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
| 7 | Read receiver count from discovery `0x13` | O §1 | We hardcode `maxSlices=1`. Standard gateware is 4; skimmer variants 9–12 with no TX | S |
| ~~8~~ | ~~Move HL2 wire + DSP off the GUI thread~~ **DONE** | O §2 | `Hl2Backend` runs `MetisClient` and both DSP chains on a dedicated `hl2-io` thread. Note the consequence: EP2 pacing, EP6 ingest, WDSP and the panadapter FFT now share ONE thread, so per-sample cost there scales with the span (§15.2) | — |
| 9 | `SetChannelState` for start/stop; `CloseChannel` only for teardown | A3 §2 | Conflating them gives clicks or leaks. Needed before T/R | S |
| 10 | RADE null-deref at `MainWindow_DigitalModes.cpp:461` | ours, gap 9 | Same shape as the DAX crash; will kill HL2 the moment RADE starts | XS |
| 11 | `AETHER_AUTOMATION_NO_AUTOCONNECT` not honoured on the HL2 path | ours, gap 10 | Test instances grab a live radio | S |
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
| 20 | Multi-slice: index-space mapping object | A3 §3 | `{ddcIndex, dspChannel, analyzerId, uiNumber}` — never derive arithmetically. Trivial now at one slice, which is when to build it | S |
| 21 | Diversity as a **pre-channel combiner** | A3 §6 | `divEXT` takes two DDC streams and yields one. Modelling it as a two-input slice fights the DSP layer | M |
| 22 | Hardware-managed T/R LNA gain (`0x0e[15]`) | A2 §A2 | Quisk uses it; lower latency than any host round trip; PureSignal needs an unclipped feedback path | S |
| 23 | PureSignal | O §11, A1 §B6 | Needs everything above. Consumes 4 RX (2 feedback), halving the slice budget | L |

### Tier 4 — deliberate divergences, do NOT "fix" by reflex

| Divergence | Reference does | We do | Why ours is defensible |
|---|---|---|---|
| `output_samplerate` | 48000 | 24000 | AudioEngine's native rate; avoids a resample. Legitimate, but it IS a divergence in the area that produced our worst bug — keep it labelled |
| Rate change | `SetAllRates` | Rebuild the channel | Dodges the intermediate-inconsistent-state hazard entirely; heavier (re-plans FFTW). Switch if rate changes get frequent |
| Spectrum | WDSP analyzer (returns pixels) | Own `Hl2Spectrum` FFT | A3 §4 recommends exactly this for our architecture. **If it ever looks noisy, the lever is a detector/averaging mode, not a bigger FFT** |
| FFTW wisdom | `WDSPwisdom(dir)` | Own `fftw_import_wisdom_from_filename` | `WDSPwisdom` is Windows-console-only. First-run slowness is expected — the fix is a progress indicator, not optimisation |

### Settled — no action

- **WDSP licensing.** GPL-2-**or-later** in 70 of 74 `.c` files, so it upgrades
  into our GPL-3. Linking is fine. (Spot-check the four before any
  redistribution question.)
- **17-second first connect.** Expected FFTW planning, per A3 §9.
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
- **The waterfall keeps a second gate** in `RadioModel`, because
  `line_duration` is a separate and slower control. A plain drop, not a
  coalesce — frames are already scarce by the time they arrive.

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

Measured on the real HL2 at 580 kHz AM, `line_duration` 100 ms:

| Span | Pan | Waterfall |
|---|---|---|
| 384 kHz | 23.4 fps | 10.1 rows/s |
| 48 kHz | ~25 fps | 10.0 rows/s |

An 8x spread across the zoom range, gone.

Those two rows were measured on hardware against the intended design, and are
what exposed the first implementation as wrong: it could not produce the 48 kHz
row. Emptying the accumulator between frames put that corner near 16 fps, and
the table's own numbers are what made the discrepancy visible rather than
plausible. `hl2_spectrum_rate_test` now pins it offline, wall-clock paced, at
every rate the gateware offers — 23-24 fps for a 25 fps request with a ~4%
spread across the zoom range, against ~35% before the fix.

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
