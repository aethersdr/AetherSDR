# Hermes-Lite 2 Bring-Up — Field Notes

Working notes from the HL2 receive bring-up on `feat/hl2-backend` (2026-07-24,
macOS 26.5.2 / arm64). Written to be *studied*, not just read: the last section
turns what happened into a proposed automated bring-up sequence.

Status: HL2 receives, tunes, and demodulates AM and SSB with correct pitch on
live hardware, with the slice decoupled from the DDC so the panadapter holds
still while tuning. Sixteen commits, `e556ad01..91b45ee0`.

Section 11 audits all of this against the independent correctness oracles at
`/Users/patj/oracles/hl2/` and is the best starting point for the next session.

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

### Tier 2 — correctness gaps

| # | Item | Source | Why it matters | Effort |
|---|---|---|---|---|
| 7 | Read receiver count from discovery `0x13` | O §1 | We hardcode `maxSlices=1`. Standard gateware is 4; skimmer variants 9–12 with no TX | S |
| 8 | Move HL2 wire + DSP off the GUI thread | O §2 | Watchdog stops the stream when the host stalls; we run both on the GUI thread | M |
| 9 | `SetChannelState` for start/stop; `CloseChannel` only for teardown | A3 §2 | Conflating them gives clicks or leaks. Needed before T/R | S |
| 10 | RADE null-deref at `MainWindow_DigitalModes.cpp:461` | ours, gap 9 | Same shape as the DAX crash; will kill HL2 the moment RADE starts | XS |
| 11 | `AETHER_AUTOMATION_NO_AUTOCONNECT` not honoured on the HL2 path | ours, gap 10 | Test instances grab a live radio | S |
| 12 | One dB-reference object per slice (LNA + calibration + AGC threshold) | A2 §A3 | Every LNA change shifts the absolute reference; the trace jumps and users read it as a real event | S |

### Tier 3 — absent subsystems, in dependency order

| # | Item | Source | Why it matters | Effort |
|---|---|---|---|---|
| 13 | RQST/ACK state machine | O §5 | Gate for everything below. Single outstanding request, echo-matched, no transaction id. **Do not model as RPC** | M |
| 14 | ADC overload bit + clip counter | O §6, A2 §A3 | The *correct* driver for gain decisions — audio level in one slice says nothing about what saturates a converter seeing 0–38.4 MHz | S |
| 15 | Discovery-reply telemetry (temp, power, PTT, clip) | O §1 | Pollable **without a stream** — cheapest first increment, and a diagnostic when the stream is broken | S |
| 16 | Pair WDSP `RXA_ADC_PK` with the hardware clip indicator | A3 §7 | Post-DDC slice vs pre-DDC full spectrum. They disagree by design; A3 calls this the most useful diagnostic pairing on the HL2 | S |
| 17 | TX IQ FIFO depth + servo | O §6, A1 §B3 | "The most important number in the protocol." TX pacing must servo against it, not a host timer — clock domains drift | M |
| 18 | Wideband bandscope (endpoint `0x04`) | O §7, A1 §A1 | Unimplemented by piHPSDR (dead code) and declined by SDR Console — a differentiation opportunity. **4 packets/block on HL2, not 32** | M |
| 19 | Filter board (I2C `0x20`), PA bias, config EEPROM | O §8 | Band filtering, and the AM-broadcast HPF matters more here than on radios with better dynamic range | M |
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
receive path already compensated (conjugating with `-imag()` before WDSP, the
fix filed as "USB and LSB are swapped"), and transmit never got the same
correction.

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
