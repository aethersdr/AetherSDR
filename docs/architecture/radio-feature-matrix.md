# Radio feature register — where the click ends, per radio

**What this answers.** Not *"did the backend declare it"* and not *"is there a
control on screen"*, but **where the operator's click ends**. A control that
does nothing looks exactly like a control that works: the widget moves, the
model commits, a readback confirms the value, nothing logs and nothing signals.
This document exists to tell those two apart, and
[`tools/check_radio_feature_matrix.py`](../../tools/check_radio_feature_matrix.py)
exists so it cannot quietly stop being true.

**The derivable cells are GENERATED from the source and DIFFED against the
committed record**, machine-readable half in
[`radio-feature-matrix.json`](radio-feature-matrix.json). The checker runs as a
`--strict` step of `.github/workflows/static-checks.yml`. It is deliberately
*not* a gate that forces the matrix to be touched when a capability is touched:
that enforces *a* change, not *the right* change, and a counter can be paid off
with a comment. A diff cannot.

---

## Two tables, two derivations — and the test that decides which

The GUI matrix and the headless matrix are built by **different methods**, and
that is the substantive finding rather than an implementation detail. The test
that decides is one question:

> **Does anything refuse when the declaration is absent?**

**In the GUI, nothing does.** The defaults are permissive — `hasFmRepeaterOffset`
is declared `= true` — the gates are hand-written and optional, and
`ControlAvailabilityRegistry`, the one mechanism built to enforce them, has **no
production consumer** — a registered unit test and nothing else. A capability
nobody declared leaves the control live. So a GUI cell is derived from
**reachability**: does the intent reach an implementation, and if not, where
does it die? A declaration is a cross-check, never the source.

**In `aetherd`, something does, and it is written into the target.**
`ModelReceiveControlTarget` opens every admission with
`caps.<record> && known(authority)` and returns the typed error
`capability.unavailable`; `ModelSliceFrequencyTarget::validCoverage` does the
same with a range. There the **declaration is the contract**, and reading it is
not a shortcut past the truth.

Deriving the headless cells the GUI way would produce a **phantom `W`** for
every verb the daemon never exposes — the override exists and nothing headless
calls it. So a feature with no `aetherd` method gets **no headless cell at
all**; it is listed under [Not on the headless surface](#not-on-the-headless-surface-at-all)
instead of being scored.

---

## The vocabulary

| code | meaning |
|---|---|
| `W` | **works / reachable** — the control reaches the backend and the backend acts |
| `D` | **dead** — the control exists but the path terminates (e.g. the base is `Q_UNUSED`) |
| `P` | **phantom** — reports success without the radio moving; reads back cached state. Two grounds, and they are different repairs — see below |
| `R` | **refuses visibly** — the user is told no |
| `H` | **hidden** — the GUI gates the control away on a capability |
| `U` | **unverified** — the generator could not decide |
| `V` | **verified on real hardware** — hand-added, and it carries a citation to a run |
| `∅` | on a row: **no capability field exists for this at all**, for any backend |
| `*` | on a cell: reached **outside the `IRadioBackend` seam**, by SmartSDR wire text |

Four of these are worth a sentence each, because the distinctions do work.

**`D` and `P` are the same experience and different repairs, and the
discriminator is PROVENANCE — not the value of any field.** The question is
*did this backend ask for this control?*

- It **asked** — it assigned the gating capability itself — and did not
  implement the verb: that is a bug in that backend, and the cell is **`D`**.
- It **never asked** — it does not mention the field at all, and a permissive
  default offered the control on its behalf: that is a gap in the default, one
  assignment repairs it, and the cell is **`P`**.
- It asked for the control to be **off**, or inherited an off default: the
  operator sees nothing either way, and the cell is **`H`**.

Reading a field's *value* as the discriminator gets the FM rows right by
accident and will be wrong on the next backend that writes a permissive value
down on purpose. `FmTonePresentation` defaults to `Hidden`, and `Legacy` is what
turns the tone controls **on**; the Demo backend assigns `Legacy` explicitly, so
it asked, and its unimplemented tone verbs are `D`. `hasFmRepeaterOffset`
defaults to `true` and the Demo backend never mentions it, so it never asked,
and the repeater rows one line below are `P`. Two adjacent rows, opposite
verdicts, and only provenance separates them. The HL2 held both halves of this
example until it declared `Hidden` and `false` outright; all six of its FM rows
are now `H`.

The generator enforces exactly this: `PERMISSIVE` is reachable only from
`DEFAULT_TRUE`, which is returned only when `capabilities()` carries no
assignment for the field at all.

**`R` is kept apart from `D`** although both mean *you cannot do this*. Turning
a `D` into an `R` removes the entire operator harm — they stop being lied to —
at a fraction of the cost of turning it into a `W`. A vocabulary that scored
them alike would make the cheap fix invisible.

**`H` is a good cell, not a gap.** On a receive-only radio a hidden transmit
control is the app being correct. Read `H` as *done*.

**`W` does not mean the radio obeyed.** It means the call reaches an
implementation. Reachability is necessary and not sufficient; only `V` claims
more, and only with a run behind it.

**`*` is the weakest evidence here.** An override is a compiler-checked fact. A
`*` cell is a file, a symbol and a literal wire token that all have to still be
there — the checker verifies all three, and they are marked so a reader knows
the evidence class differs.

**A `*` that stops being true is TWO different facts, and the checker splits
them.** Every alternate path in this register documents the same thing: a
control reaching a Flex by wire text built *above* the seam. The `aetherd`
receive-control migration exists to delete exactly those bypasses, so a boolean
"is the record still true" would raise an error on every **successful**
migration — and an error that fires three times on good news is ignored by the
fourth. So:

| | what happened | the check |
| --- | --- | --- |
| **rotted** | the record's claim is false and nothing stronger replaced it | fails, naming which condition refused |
| **retired** | the bypass was deliberately deleted because the control moved *behind* the seam | reported as progress, never fails |

A retirement has to satisfy all four: the wire token is **gone from the declared
file** (a file that still emits it has the bypass and merely names it wrong —
that is an unrecorded rename); the backend **overrides the row's seam verb**
with a body that is not a `Q_UNUSED` no-op; the cell **still derives `W`**; and
the wire token is **still emitted from the backend's own source**, so the bypass
was absorbed below the seam rather than lost. The question the split answers is
*did the evidence get weaker, or did a weaker piece of evidence stop being
needed?*

**One case it cannot separate, and it says so rather than pretending.** Where a
backend **already** overrode the verb and **already** carried the same wire
token below the seam — three records today, `rx/filter`, `rx/agc-threshold` and
`rx/pan-center` on Flex — mangling the above-seam literal yields a tree
indistinguishable from the migration, because in every respect this register
scores it *is* the same tree: the cell is still true and the bypass no longer
carries that text. The other **37 of 40** records are load-bearing — the
derivation actually rests on them — and breaking one of those moves the cell,
so `feature-matrix-drift` fires beside the alt-path error.

**A retirement notice is an edit request, not a status line.** It repeats every
run until the `alt_path` entry is deleted from the sidecar and the `*` dropped
from that cell here.

---

## The GUI matrix

Columns: **Flex** (`supported`) · **Icom** (`early`) · **HL2** (`experimental`)
· **ANAN**-G2 (`experimental`, RX-only) · **RTL**-SDR (`experimental`, RX-only)
· **Demo** (`SimBackend`).

**Read the Icom column as "on a profiled model".** `IcomCivBackend::capabilities()`
is pervasively dynamic, built per radio from `IcomModels`/`IcomModelProfile`.
Only three models carry a full bring-up profile — IC-705, IC-9700, IC-7300MK2 —
and `README.md` claims CI-V-guide verification for the first and third only. An
unrecognised address gets `kUnprofiled`, where `supports()` is true for `Core`
alone. A single Icom column is a simplification the source does not support, and
the dispatch trace below says which rows are unconditional.

Host-side features are deliberately absent. The AetherDSP noise modules, the
RX/TX equaliser, the CW sidetone and the iambic keyer run in this application
and work on every family; printing them as six identical columns would be noise,
and scoring them per radio would be wrong.

### Receive

| feature | Flex | Icom | HL2 | ANAN | RTL | Demo | control |
|---|:--:|:--:|:--:|:--:|:--:|:--:|---|
| `rx/frequency` | W | W | V | W | W | W | VFO tuning — dial, keypad, band buttons, click-tune |
| `rx/mode` | W | W | W | W | W | W | mode selection (USB/LSB/CW/AM/FM/DIGU/…) |
| `rx/filter` | W* | W | V | W | P | W | receive filter width / passband edges |
| `rx/filter-preset` | H | H | H | H | H | H | stored RX filter preset (FIL1/FIL2/FIL3) |
| `rx/agc` | W | W | W | W | D | W | AGC mode — off / slow / med / fast |
| `rx/agc-threshold` | W* | H | W | W | H | W | AGC-T threshold slider |
| `rx/pan-center` | W* | W | W | W | W | W | drag the spectrum or waterfall (pan centre) |
| `rx/pan-bandwidth` | W* | W | V | W | W | D | zoom / span |
| `rx/pan-framerate` | W* | D | W | W | W | D | waterfall / spectrum frame rate |
| `rx/rf-gain` | W* | W | V | W | W | D | RF gain slider (ANT panel) |
| `rx/preamp` | H | W | H | H | H | H | preamp step (named positions) |
| `rx/attenuator` | H | W | H | H | H | H | attenuator step |
| `rx/antenna` | W* | W | H | H | H | H | receive antenna selection |
| `rx/audio-mute` | W* | D | W | D | W | D | slice mute |
| `rx/audio-gain` | W* | W | W | D | W | D | slice audio gain |
| `rx/audio-pan` | W* | D | W | D | W | D | slice audio pan |
| `rx/dial-lock` | D | W | D | D | D | D | radio dial lock |
| `rx/rit-enable` ∅ | W* | W | D | D | D | D | RIT on/off |
| `rx/rit-offset` ∅ | W* | W | D | D | D | D | RIT offset |
| `rx/xit-enable` ∅ | W* | W | D | D | D | D | XIT on/off |
| `rx/xit-offset` ∅ | W* | U | U | U | U | U | XIT offset |

### Receive DSP

| feature | Flex | Icom | HL2 | ANAN | RTL | Demo | control |
|---|:--:|:--:|:--:|:--:|:--:|:--:|---|
| `dsp/noise-reduction` | W* | W | H | H | H | H | NR button and level |
| `dsp/noise-blanker` | W* | W | W | W | H | H | NB button and level |
| `dsp/auto-notch` | W* | W | H | H | H | H | ANF button |
| `dsp/manual-notch` | H | W | H | H | H | H | MN per-slice manual notch |
| `dsp/notch-create` | W | H | W | H | H | H | tracking notch (TNF): create |
| `dsp/notch-edit` | W | H | W | H | H | H | tracking notch: move / resize / depth |
| `dsp/notch-remove` | W | H | W | H | H | H | tracking notch: remove |
| `dsp/notch-enable` | W | H | W | H | H | H | tracking notches on/off |
| `dsp/squelch` ∅ | W* | W | D | D | D | D | SQL button and threshold slider |

### Transmit

| feature | Flex | Icom | HL2 | ANAN | RTL | Demo | control |
|---|:--:|:--:|:--:|:--:|:--:|:--:|---|
| `tx/keying` | W | W | V | R | R | R | MOX / PTT |
| `tx/tune` | W | W | W | R | R | R | TUNE |
| `tx/power` | W* | W | V | D | D | D | RF power |
| `tx/mic-gain` | W* | W | W | D | D | D | microphone gain |
| `tx/filter` | W* | W | V | H | H | H | TX filter low/high cuts |
| `tx/monitor` | W* | W | D | D | D | D | TX monitor on / level |
| `tx/audio-monitor` | D | W | W | D | D | D | TX audio monitor toggle |
| `tx/speech-processor` | W* | W | D | D | D | D | speech processor (PROC) |
| `tx/vox` | W* | W | D | D | D | D | VOX |
| `tx/atu` | W | W | H | H | H | H | ATU / antenna tuner |
| `tx/freq-check` | H | W | H | H | H | H | transmit frequency check |
| `tx/tx-slice` | W* | D | W | D | D | D | which slice transmits |

### CW

| feature | Flex | Icom | HL2 | ANAN | RTL | Demo | control |
|---|:--:|:--:|:--:|:--:|:--:|:--:|---|
| `cw/keying` | W* | D | W | D | D | D | CW key down / up |
| `cw/speed` | W* | W | D | D | D | D | CW speed (WPM) |
| `cw/pitch` | W* | W | W | W | D | D | CW pitch |
| `cw/break-in` | W* | W | D | D | D | D | CW break-in |
| `cw/text-send` | W* | W | H | H | H | H | text keyer: send |
| `cw/text-abort` | W | W | H | H | H | H | text keyer: abort |

### FM repeater and tone

| feature | Flex | Icom | HL2 | ANAN | RTL | Demo | control |
|---|:--:|:--:|:--:|:--:|:--:|:--:|---|
| `fm/tone-mode` | W* | W | H | H | H | D | CTCSS / DTCS tone mode |
| `fm/tone-tx` | W* | W | H | H | H | D | TX tone frequency |
| `fm/tone-rx` | D | W | H | H | H | D | RX tone frequency |
| `fm/dtcs` | D | W | H | H | H | D | DTCS code and reverse flags |
| `fm/repeater-dir` | W* | W | H | P | H | P | repeater shift direction |
| `fm/repeater-offset` | W* | W | H | P | H | P | repeater offset (Hz) |

### Memories, slices and panadapters

| feature | Flex | Icom | HL2 | ANAN | RTL | Demo | control |
|---|:--:|:--:|:--:|:--:|:--:|:--:|---|
| `mem/recall` | W* | W | U | U | U | U | recall a memory channel |
| `mem/refresh` | D | W | D | D | D | D | refresh the memory list |
| `slice/create` | W* | R | R | R | R | R | add a slice / receiver |
| `slice/remove` | W* | R | R | R | R | R | remove a slice / receiver |
| `slice/active` | W* | D | W | D | D | D | select the active slice |
| `pan/create` | W* | R | W | R | R | W | add a panadapter |
| `pan/remove` | W* | R | W | R | R | W | remove a panadapter |

---

## The headless matrix — `aetherd`'s control protocol v1

Derived from the **capability records**, because there absence is enforced:
`ModelReceiveControlTarget` and `ModelSliceFrequencyTarget` refuse with a typed
`capability.unavailable` when a record is missing or its authority is `Unknown`.
So `R` here means *the client is told no in a typed error*, which is a different
and better thing than the GUI's silent `D`.

**The whole surface is six receive operations plus a frequency target and a
transmit lease.** `ReceiveControlTarget` declares a closed typed intent set —
`slice.setMode`, `slice.setFilter`, `slice.setAudioGain`, `slice.setAudioMute`,
`panadapter.setCenter`, `panadapter.setBandwidth` — with no reflection, no raw
commands and no runtime setter names.

| headless | Flex | Icom | HL2 | ANAN | RTL | Demo | method | control record |
|---|:--:|:--:|:--:|:--:|:--:|:--:|---|---|
| `rx/frequency` | R | R | W | R | W | W | `slice.setFrequency` | `sliceFrequencyControl` |
| `rx/mode` | W | R | W | R | W | W | `slice.setMode` | `receiveModeControl` |
| `rx/filter` | W | R | W | R | R | R | `slice.setFilter` | `receiveFilterControl` |
| `rx/pan-center` | R | R | W | R | R | R | `panadapter.setCenter` | `receivePanCenterControl` |
| `rx/pan-bandwidth` | R | R | R | W | W | R | `panadapter.setBandwidth` | `receivePanBandwidthControl` |
| `rx/audio-mute` | R | R | W | R | W | R | `slice.setAudioMute` | `receiveAudioControl` |
| `rx/audio-gain` | R | R | W | R | W | R | `slice.setAudioGain` | `receiveAudioControl` |
| `tx/keying` | U | R | U | R | R | R | `tx.setKeying` | `canTransmit` |

**Icom is absent from this surface entirely.** `ModelRadioConnectionTarget::supports()`
admits `sim`, `rtl`, and `lan` for `flex`, `hl2` and `anan`. CI-V is
serial-transported and has no LAN discovery path here, so `ControlService`
answers `capability.unavailable` — *"radio connection is unsupported"* — for
every Icom. That is the single largest divergence between the two surfaces and
it appears in no `README`.

**The ranges are checked, not just the authority — and that changes two cells.**
`validCoverage()` requires `minimumHz > 0`. Flex declares
`sliceFrequencyControl = {Authority::Radio, 0, 0}` and ANAN
`{Authority::Engine, 0, 0}`, so **headless tuning is refused on both**, on the
one family whose GUI column is otherwise the fullest. A derivation that read
only the authority would have printed `W` there. This is exactly the phantom-`W`
failure the two-method split exists to prevent, and it is the reason the
generator evaluates the record rather than trusting it.

**`tx/keying` is `U` on the three TX-capable families, and that is honest.** The
daemon binds a transmit target only under `--allow-local-tx`
(`src/aetherd/main.cpp`), so the cell depends on how the process was launched
and not on the radio. Where `canTransmit` is explicitly `false` the cell is a
confident `R`: the engine preflight refuses whatever the flag says.

### Not on the headless surface at all

Every other row in this document. `aetherd` exposes no DSP control, no notch
management, no squelch, no RIT/XIT, no CW, no ATU, no memories, no preamp,
attenuator or RX antenna, and no slice or panadapter lifecycle. Those are absent
rather than unsupported, and the distinction matters to a script author: nothing
refuses, because nothing is offered.

**Its honest limit, which the daemon documents and a script must respect:** a
reply confirms *intent accepted*, never hardware acknowledgement. The effect is
observed through `resource.subscribe`.

---

## Dispatch traces

Four backends had no trace of this kind. What follows is what actually happens
on each when the GUI or TCI asks — read from the code, cited by file and symbol.

### Flex — the seam is not where this family lives

**The structural fact that decides half the column.** `RadioModel` builds its
`SliceModel` at **two sites**, and a Flex only ever reaches one. The
seam-wiring site in `RadioModel::handleSliceChanged` is guarded
`if (!s && !m_flexBackend)`, and it is the site that connects
`squelchCommandIssued`, `ritCommandIssued`, `xitCommandIssued`,
`noiseReductionCommandIssued`, `noiseBlankerCommandIssued`,
`autoNotchCommandIssued`, `manualNotchCommandIssued` and the six FM intents to
the backend. **On a Flex those connections do not exist.** For roughly fifteen
rows the seam verb is not a no-op that gets called — it is never called at all,
and the control reaches the radio as SmartSDR wire text instead. That is what
every `*` in the Flex column means.

`FlexBackend` overrides 21 seam verbs and all four command sinks are wired
(`setCommandSink`, `setSliceCommandSink`, `setTxCommandSink`), so an override
that calls `send`/`sendSlice`/`sendTx` genuinely reaches the wire.

**`setSliceMode` is the one load-bearing override.** Every other overridden
slice verb duplicates wire text `SliceModel` also composes. `SliceModel::setMode`
deliberately emits none — it raises `modeChangeRequested` — so
`FlexBackend::setSliceMode` is the sole path to the radio for mode selection.
An override that looks like one more redundant copy is the one that matters.

**Three overrides that reach the wire have no live caller.**
`FlexBackend::setPanCenter` is gated away on both planes: RadioModel's seam
branch is `!m_flexBackend` and `receivePanCenterControl` is `std::nullopt`.
`setSliceAgc` is reachable only through the control protocol and automation.
`abortCwText` is reached on the operator cancel path by nothing —
`CwxModel::clearBuffer` runs instead — and only fires at teardown.

**`capabilities()` is not constant.** `caps.model` is read live from
`RadioModel` and arrives in a `radio …` status *after* the connect edge, so
`hasExtendedDsp`, `maxSlices` and `maxPanadapters` flip mid-session;
`decodeRadioStatus` emits `capabilitiesChanged()` when the model name moves, and
`setRadioReportedCapacity` overrides the capacities from the discovery packet.
Everything else in the body is a literal.

### Icom CI-V — the override is not the question

Icom overrides 55 seam verbs, so *"does it override"* settles almost nothing.
The question is **whether the override is conditional on a model profile**, and
the answer splits the rows three ways:

- **Attempted unconditionally on every Icom, including an unrecognised one** —
  frequency, mode, filter, filter preset, AGC, RF gain, audio gain, NR, NB, ANF,
  manual notch, squelch, TX power, mic gain, TX monitor, TX audio monitor,
  speech processor, VOX, and all three RIT/XIT verbs.
- **Refused outright when the profile is absent, and almost always silently** —
  RX antenna (IC-7300MK2 only), attenuator, TX filter, ATU, transmit frequency
  check, dial lock, all six FM rows, memory recall and refresh. Only
  `setAtu` (a `qCWarning`) and `refreshMemories` (a real `configurationWarning`)
  say anything; `setSliceFrequency` is the one row whose profile refusal is
  visible to the operator.
- **Never reaches the seam at all, because the gate is above it** — CW speed,
  pitch, break-in and text send, dropped by `RadioModel` on
  `hasRadioSideCwKeyer`; and AGC-T, closed by `hasAgcThreshold = false`.

**`setXitOffset` is correct here, and this is worth stating because it looks
wrong.** Icom overrides `setRitEnabled`, `setRitOffset` and `setXitEnabled` but
not `setXitOffset`, whose base body is the alias `setRitOffset(hz)`. On an Icom
that is right by construction: `cmdTuneOffsetHz` is CI-V `21 00`, `cmdRitEnable`
is `21 01` and `cmdXitEnable` is `21 02` — **one shift register, two enables**.
The decoder agrees, setting `d.ritFreq = hz; d.xitFreq = hz;` from the same
frame, and `IcomControls` carries a single `"rit.offset"` spec labelled
*"RIT / XIT offset"*. There is nothing to distinguish, so nothing is lost. The
hazard is upstream of Icom: on a two-register family the alias would write the
wrong one, which is why the generator emits `U` for that row rather than `W`.

### ANAN (P2) — three live setters, and everything else at connect time

`P2Client` exposes **three** `Q_INVOKABLE` live setters — `setDdc0FrequencyHz`,
`setDdcRateLive` and `setStepAttenuationDb`; its class comment says *"RX-ONLY:
there is no PTT parameter anywhere in this class"*. Everything else the radio is
told is a connect-time `P2Client::Params` field. So on this family:

- **reaches the wire** = frequency (`applyTuneToRadioAndPan()`, behind a 33 ms
  leading-and-trailing throttle), and the DDC sample rate, written **live on the
  running session**: `AnanBackend::finishRateChange` invokes
  `P2Client::setDdcRateLive`, which returns false without sending unless the
  session is running and otherwise resends the whole DDC-Specific packet, three
  times across the settle window because the protocol offers no ack. The rate no
  longer costs a session stop/reconfigure/restart — `startP2ClientSession` is
  reached from the connect path alone, and `AnanBackend::beginRateChange` now
  only rebuilds the DSP channel off-thread before handing over. Its own leading
  comment still describes the old restart, so the source says both things; and
  **RF gain**, which on a G2 is the step attenuator in front of the ADC rather
  than a gain stage the host drives: `AnanBackend::setPanRfGain` clamps the
  slider into -31…0 dB, negates it into 0…31 dB of attenuation and invokes
  `P2Client::setStepAttenuationDb`, which stores the per-ADC value and resends
  the High Priority packet **immediately when the session is running** rather
  than letting the operator wait up to 100 ms for the next keepalive. It is the
  one control written on both planes: the attenuation is mirrored into
  `m_pendingParams` as well, so a restart that does rebuild the session resends
  it;
- **reaches the DSP** = mode, filter, AGC, frame rate, CW pitch and the **noise
  blanker**, through `QMetaObject::invokeMethod(m_dsp, …)` into `AnanRxDsp`.
  The blanker is WDSP's ANB on the raw IQ: `capabilities()` declares
  `hasHostNoiseBlanker = true` while `hasRadioSideDsp` stays `false`, and
  `AnanBackend::setSliceNoiseBlanker` forwards into
  `AnanRxDsp::setNoiseBlanker`. The NB button gates on the OR of those two
  fields, so declaring the host blanker is what keeps the control live on a
  radio whose own firmware runs no DSP.

Both count as the control working — on a raw-IQ radio the engine-side chain is
where the demodulator is — and the trace distinguishes them because the repair
differs if either breaks.

**`setCwPitch` is a receive control here.** ANAN overrides it, and it reaches
`pushModeFilterShift()` where `cwBfoOffsetHz()` sets the BFO and re-derives the
passband. It is reached because ANAN declares `hostModulates = true`, which is
the second of RadioModel's two CW-pitch forwards. A method-first trace that
tested only `hasRadioSideCwKeyer` would call CW pitch dead on this radio and on
the HL2, where it demonstrably works.

**No alternate path exists for anything ANAN does not override.** Every
non-overridden verb has exactly one model-side call site, and the Flex wire
plane is Flex-only — `RadioModel::setPanPreampFor` says so in its own comment:
*"No Flex fallback: a Flex publishes no preamp or attenuator labels"*.

### RTL-SDR — a store-but-never-consume phantom, proven

`RtlSdrBackend` overrides 20 verbs. Two of them do not do what the operator
sees, and one is provable:

**`setSliceFilter` is the `P` cell in this document.** The backend validates the
edges, stores `m_sliceFilterLow`/`m_sliceFilterHigh`, pushes them to
`RtlSdrDdc::setSliceFilter`, which stores them into `m_filterLowHz` and
`m_filterHighHz`, and emits a `SliceDelta` carrying them.
**Nothing reads either atomic anywhere in the tree** — the checker asserts this
mechanically and will flip the cell to `W` the day someone wires it up.
`RtlSdrDdc::processAudio` loads the sample rate, centre, slice, mode, mute, gain
and pan, and nothing else; the only band-limiting is the two decimation stages.
The backend's own copies *are* live — they round-trip through
`currentOperatingState()`/`applyRestoredState()` and echo in the delta — so the
edges survive a reconnect and the GUI shows exactly what the operator set. They
never affect a sample. That is the worst shape a phantom can take.

**`setSliceAgc` is worse in one respect and is a `D` here: it does not even
store.** The body is three `Q_UNUSED` and a *"Phase 2"* comment, while
`capabilities()` explicitly re-asserts the full four-mode `agcModes` list, and
`connectRadio` deliberately turns the RTL2832's own digital AGC **off**.

**`setSliceMode` accepts nine mode labels that resolve to five demodulators.**
`Fm`/`Fmn` are identical; `Am`/`Sam` are the same envelope detector, so SAM is
not synchronous; `Cw`/`Cwr` are the same real-part tap with no BFO offset, so
CWR is CW. An unknown mode returns with no error, no log and no `sliceChanged`.

**`RtlReceiverRegistry` serves no row in this table.** 690 lines, compiled
unconditionally, included by its own `.cpp` and its test and nothing else — the
prepared foundation for RFC #5468 multi-RX. `docs/HERMES.md` says so: *"has no
production caller today"*.

---

## The five `P` cells, by provenance

`P` has two grounds. Both satisfy the definition — the control reports success,
the radio does not move, and a readback confirms the value the operator set —
but they are different findings for a contributor, so the checker labels each
one and prints the split rather than a single tally.

| ground | cells | what it is | repair |
|---|---|---|---|
| **inherited default** | `fm/repeater-dir` and `fm/repeater-offset` on ANAN and Demo (4) | The backend never mentions `hasFmRepeaterOffset`, which is declared `= true`. Both readers spell the gate `!connected \|\| caps.hasFmRepeaterOffset`, so the control is offered unless a *connected* radio actively denies it. **Nobody decided to offer it.** On the ANAN it is a repeater duplex control on a radio with no transmitter. The HL2 carried the same two cells until it made the assignment this column prescribes; both are now `H`. | One assignment per backend: `c.hasFmRepeaterOffset = false;` |
| **cached readback** | `rx/filter` on RTL (1) | The backend **did** ask: `RtlSdrBackend::setSliceFilter` validates the edges, stores them, pushes them to `RtlSdrDdc::setSliceFilter`, and echoes a `SliceDelta`. The DDC stores them into two atomics that **nothing in the tree reads**. The edges round-trip through `currentOperatingState()`/`applyRestoredState()`, so they survive a reconnect and the GUI shows exactly what the operator set — and never affect a sample. | Real work in `RtlSdrDdc`: consume the edges, or stop claiming them |

The second ground is why `P` is not simply "the permissive-default class". A
backend that asked and did not implement is `D`; a backend that asked,
implemented a store, and confirms it back to the operator is the more
misleading case, and the vocabulary's *"reads back cached state"* clause is
there for exactly it.

ANAN's four **tone** rows are `H`, not `P`, and the reason is the same rule read
the other way: `AnanBackend` does not mention `fmTonePresentation` anywhere, and
that field's default is `Hidden`. A default the backend never requested is only
a phantom when it is **permissive**; an off default hides the control, the app
and the radio agree, and the cell is honest.

---

## The `V` cells — verified on real hardware

Seven cells, all on the HL2, each citing one recorded bench run. **`V` is never
generated.** The checker asserts only that the cell carries a run identifier and
a line saying what that run showed, and that the generator independently derives
`W` for the same cell — a `V` on a cell the source calls dead is either an
over-claimed `V` or a broken generator, and both need a person.

| cell | run | what the run showed |
|---|---|---|
| `rx/frequency` / HL2 | `d114-full-band-baseline` | 93 chunks swept 0.29-31.74 MHz on the real radio; 8/8 broadcast spans clear the floor by 15 dB, loudest 71.4 dB at 9.592 MHz - the commanded frequency changes what the radio receives |
| `rx/filter` / HL2 | `d111-noise-density-vs-bandwidth` | S-meter -108.52 -> -99.15 dBm across a commanded 500->3000 Hz passband, noise density flat, slope 0.219 dB/dB against a 0.40 bound, forward and reverse legs agreeing |
| `rx/pan-bandwidth` / HL2 | `d101-pan-limits` | delivered EP6 rates 48000.8 / 95999.8 / 192002.3 / 383992.6 Hz counted from sequence numbers across four zoom levels; the 48 k floor and 384 k ceiling both proven by extra clicks that change nothing |
| `rx/rf-gain` / HL2 | `d103-gain-fold` | LNA code 31 = -53.20 dBFS against code 32 = -97.75 dBFS, a -44.55 dB step; all seven wrapped codes land on their mod-32 twins within 0.52 dB |
| `tx/keying` / HL2 | `d126-ep2-cw-drive40` | MOX set on 7744 EP2 frames over 10.16 s keyed, corroborated by two independent instruments: sustained forward power 0.652 W and PA temperature +4.97 degC |
| `tx/power` / HL2 | `d99-drive-sweep` | the radio's own forward-power detector read 1.6 / 115 / 115 / 126 / 154 / 225 / 332 / 434 / 476 across drive 0->100 %, monotone above 10 % and better than 3x from 25 to 100 % (RESULT.md; OBSERVATION.md's external-meter leg is retracted as unreadable) |
| `tx/filter` / HL2 | `d110-usb-geometry-low-edge` | TX passband commanded 300/2700 -> 150/3000 and read back, measured at a remote receiver: USB window +13.8 dB, LSB -13.2 dB, a 27.0 dB split against a pre-registered >=+10 / <=-10 / >=20; corroborated at wire level by d125's 54.81 dB CW-passband suppression |

**The provenance rule that excluded more than it admitted.** Runs `d77` and
`d87` ran against `hpsdrsim` on loopback and are simulated; they produce no `V`
however good their numbers are. Every run cited above answered from the DUT's own
MAC on gateware 74, and the discriminator is the *answering* MAC, not the saved
`LastConnectedRadioSerial` in the restore snapshot — which carries the real
radio's MAC into almost every simulator run and will mark d77 and d87 as
hardware if grepped naively.

**Four things a run showed that did not earn a `V`**, because being conservative
is the point:

- **RX mode.** `d123-cw-level-deficit` measured a real USB-versus-CW difference
  in the radio's own forward power, which is a *transmit* consequence of a mode
  change. That mode reaches the receive DSP is evidenced on `hpsdrsim` only.
- **Slice mute.** `d83-unkey-transient` measured the demodulator mute across the
  T/R transition, which is not the operator's `setSliceAudioMute` control.
- **Speech processor.** `20260906T103010Z-d73-keyed-alc` measured the voice
  processor preserving the envelope on the real radio — but that is this host's
  TX DSP chain, not `setSpeechProcessor`, which the HL2 does not override. The
  checker would have rejected a `V` there, and the row stays `D`.
- **AGC.** `d85-tr-window` is a T/R-window consequence, not attack or decay.

---

## What the generator could not decide, and why

Nine cells are `U`. Both reasons are the same shape — the base class forwards
the intent somewhere the generator will not follow — and both are honest gaps
rather than missing work.

| cells | why |
|---|---|
| `rx/xit-offset` on Icom, HL2, ANAN, RTL, Demo | The base `setXitOffset` is an **alias** into `setRitOffset`. The intent is reachable, but it is not necessarily *this* control: on Icom the alias is correct because the radio has one shift register, and on a two-register family it would silently write the other one. Calling it `W` would launder that; calling it `D` would be false on Icom. |
| `mem/recall` on HL2, ANAN, RTL, Demo | The base `applyMemoryRecallDetails` calls `setSliceFmRepeater`, which fans out into four FM setters that are themselves no-ops on those families, **and then returns `true`**. A caller that checks the return value is told the recall succeeded. Whether the operator notices depends on which memory fields were set, which the generator cannot know. |

And four things the checker cannot see at all, stated so a green run is not read
as more than it is: whether the radio obeyed (only `V` claims that); whether a
control reaches its signal, since the widget-to-model wiring is Qt lambdas in
constructor bodies; whether a model gate refuses visibly or drops silently (the
`visible` flag is authored, not derived); and whether an override that reaches
the wire writes the **right** register.

---

## What a green check does not prove — the roster is not guarded

The checker proves the **cells** are true. It does not prove the **roster** is
complete, and that is a real hole rather than a theoretical one.

`main()` iterates the committed record — `for rid, row in matrix.items()` — so
every check it runs is a question asked *about a row that already exists*.
Nothing iterates the other way: nothing enumerates `IRadioBackend`'s seam verbs,
or the controls the GUI builds, and asks whether each one has a row. Two
consequences follow, and both are silent:

- **Delete a feature entirely** — its record from the JSON and its line from the
  table — and the run stays green. `aethersdr-agent` did exactly that with
  `rx/agc` while reviewing this PR: `--strict` reported
  `60 feature(s) … 0 disagreement(s)` and exited 0.
- **Add a new seam verb with a GUI control and no row**, and nothing notices
  either. The register simply does not describe it, and says nothing about the
  omission.

`MIN_CELLS` is not a roster check. It is a vacuity floor against a broken
parser, set well below the current count: at 200 cells over six backends it
does not fire until the matrix falls below 34 rows, so **27 of today's 61 rows
could be deleted and the run would still be green**. It catches a wholesale
collapse, never a row.

So a green run reads: *every cell committed here agrees with the source.* It
does **not** read: *every control an operator can reach is in this table.*
Closing that needs a **roster anchor** — deriving the feature list from the seam
and the GUI control inventory and diffing it the way the cells are diffed, so an
addition or a deletion fails instead of passing quietly. That is a generator of
a different shape and it is deliberately not in this PR.

---

## Defects this trace found

Recorded here, not filed. Each is a candidate for its own issue.

| # | where | what |
|---|---|---|
| 1 | `RtlSdrDdc` | `setSliceFilter` stores the passband edges in two atomics that **nothing in the tree reads**, while the value round-trips through save/restore and echoes to the GUI. `rxFilterWidthsHz` is inherited empty, so the operator also gets the full mode-preset width grid, every button of which is inert. |
| 2 | `RtlSdrBackend` | `setSliceAgc` discards all three arguments while `capabilities()` explicitly asserts the four-mode `agcModes` list, so a fully populated AGC selector drives nothing. `connectRadio` additionally turns the dongle's own AGC off. |
| 3 | `AppletPanel` | The S-Meter is constructed unconditionally, with no capability read and no family test, and `"VU"` is deliberately excluded from `markHardwareConditional`. ANAN, RTL and Demo publish **no meters at all**, so the needle parks at scale minimum permanently — indistinguishable from a dead-quiet band, and announced to screen readers as a reading. The honest "---" face exists but only the KiwiSDR path can trigger it. |
| 4 | `RadioModel` | The `canTransmit` refusal is applied at the **keying** sites only. RF power, mic gain, TX monitor, TX audio monitor, speech processor and VOX run straight into base no-ops on three receive-only families with no guard — and `hostModulates = true` makes ANAN worse, pushing `setTxPower` and `setMicGain` once on every connect. One guard, already written, applied in more places, turns six `D` cells into `R` cells on three radios. |
| 5 | `IcomCivBackend` | `hasRadioSideCwKeyer` is `profile.cwTextKeyer.has_value()` — a question about the CI-V `17` **text** keyer — and `RadioModel` uses it to gate CW **speed, pitch and break-in**. The IC-9700 has no text keyer and sets `hasCwTune = true`, so on that radio three CW controls reach nothing, silently. One capability answering two different questions. |
| 6 | `FlexBackend` | `setTxAudioMonitor` is not overridden, has no alternate wire path and no capability to gate on — and its only caller is `RadioCertification`, which opens the monitor *"just for this stage"* and whose own result is downstream of it. A silent no-op inside a diagnostic. |
| 7 | `RxApplet` | When the RX-antenna list is empty — which it always is on HL2, ANAN, RTL and Demo — the menu falls back to `"ANT1"`/`"ANT2"`. The operator is offered two named antennas the radio never claimed. Worse than a dead control: an invented one. |
| 8 | `AnanBackend` | `hasFmRepeaterOffset` is never assigned and inherits `true`, so the repeater duplex control is live on a receive-only radio whose `modeFromString` accepts `FM` and `NFM`. One line fixes it; the same shape still holds on Demo. |
| 9 | `IcomCivBackend` | `setPanPreamp` on a model with no verified preamp ladder clamps to 0 and **still writes OFF to the register**, against the file's own stated rule (*"no verified table means publish nothing"*) and against its own sibling `setPanAttenuator`, which returns early. |
| 10 | `FlexBackend` | `capabilities()` advertises a complete CWX text keyer — `cwTextKeyerName = "CWX"` plus three shape flags — while not overriding `sendCwText`, whose base returns *"radio has no text keyer"*. **Not a live bug**: `RadioModel::dispatchCwxText` short-circuits `return true` for the Flex plane before the seam call. It is a contradiction held together by one family-name comparison, and the next seam caller added without that short-circuit gets a false refusal from the one family that obviously supports the feature. |

---

## Keeping this true

- `python tools/check_radio_feature_matrix.py --strict` — the gate. It fails on
  any cell that disagrees with the source, on a `V` without a citation, on a
  `∅` row whose capability field has since been added, on an alternate path
  that has **rotted** — as against one that **retired**, which it reports as
  progress and does not fail — on a gate the GUI no longer applies, and on the
  document disagreeing with its own JSON sidecar.
- **When a `*` retires, delete the record.** The check prints a
  `feature-matrix-alt-path-retired` notice naming the row and the family; the
  repair is to remove that family's `alt_path` from the sidecar and the `*`
  from the cell above. The notice repeats until someone does.
- `python tools/check_radio_feature_matrix.py --print` — the derived matrix
  alone, for checking a change before committing it.
- **A green run does not mean the roster is complete.** The gate iterates the
  committed record, so it guards cells, not the list of rows — see
  [What a green check does not prove](#what-a-green-check-does-not-prove--the-roster-is-not-guarded).
- **Do not add a capability boolean to fix a `∅` row.**
  `tools/check_capability_records.py` freezes `RadioCapabilities` at 71 booleans
  and it may only shrink. The shape that can land is an
  `std::optional<…Control>` record whose **absence means no support**, in the
  style of `ReceiveAudioControl` — or an interface returned through the seam,
  the way `AutoRfGainControl.h` did when auto-RF-gain hit the same wall.
