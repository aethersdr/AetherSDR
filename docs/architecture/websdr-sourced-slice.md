# WebSDR-Sourced VFO/Slice — Design

Status: **proposal / pre-implementation**. Companion to the WebSDR receive
module in PR #3612 (`feat/websdr-module`), which adds a standalone listen-only
WebSDR panel. This document covers the *deeper* integration the panel does not:
letting a **VFO/slice in the existing UI take its audio from a WebSDR feed
instead of the radio's VITA-49 stream**, and routing that audio through the
existing client-side DSP (AetherDSP, AetherVoice, client NR).

It is a design map grounded in the current code, not a merged feature. Two
hard prerequisites gate any implementation and are called out up front in
[§7](#7-prerequisites--caveats): the **WebSDR audio decoder's licensing**
(unresolved — see PR #3612 review) and **cross-platform parity**.

---

## 1. Goal & the core constraint

**Goal:** a user picks "WebSDR" as the receive source for a VFO and hears that
remote receiver through AetherSDR's normal audio path — same speaker, same
volume, same client DSP — as if it were "another antenna" on that slice. The
compelling use case is an **RF-quiet remote ear on the frequency you're already
watching** (kills local shack noise: SMPSUs, PLC, LED drivers).

**The constraint that shapes everything:** a `SliceModel` today is *welded to
the physical radio* and cannot be cheaply made virtual:

- It is **only ever constructed** from a radio status frame —
  [`RadioModel::handleSliceStatus()`](../../src/models/RadioModel.cpp), gated on
  `in_use=1` + `RF_frequency`. There is no virtual/local/synthetic slice path
  anywhere in the model.
- Its **id is radio-assigned** (the app sends `slice create`, the radio returns
  the index from its 0–7 pool).
- **Every setter emits `commandReady` → the radio**
  ([`SliceModel.cpp:41`](../../src/models/SliceModel.cpp#L41)) — frequency,
  mode, filter, gain, antenna, NB/NR all leave as `slice set …`.
- A slice is **rendered as a flag on a Flex panadapter** at its frequency. A
  WebSDR feed has its *own* spectrum on its *own* band, which the Flex is not
  tuned to.

**Consequence:** do **not** reuse `SliceModel` for a WebSDR receiver, and do
**not** attempt to draw a WebSDR receiver onto the Flex panadapter (its
frequency axis is independent — PR #3612's design doc hit this and backed away).
Instead, model the WebSDR source as a **per-slice audio-source override** layered
on top of an existing real slice (v1), with an optional **virtual receiver**
later (v2). See [§3](#3-source-model).

---

## 2. What already exists (and is reusable)

The audio plumbing for this is largely present. Three existing mechanisms carry
most of the weight:

1. **Per-channel audio routing (DAX).** `PanadapterStream` already demuxes
   per-channel audio and emits `daxAudioReady(int channel, QByteArray)`
   separately from the global `audioDataReady` — a "this audio belongs to
   receiver N" concept already ships.
2. **Client-side mixing of a second source (RADE).** `AudioEngine` already mixes
   a second buffer (`m_radeRxBuffer`) into the main `m_rxBuffer` sample-by-sample
   with clamping in the RX drain
   ([`AudioEngine.cpp:737-765`](../../src/core/AudioEngine.cpp#L737-L765)), each
   with its own resampler. This is the template for an aux source.
3. **A format-agnostic injection point.** `feedAudioDataImpl()` accepts any
   24 kHz stereo float32 buffer and runs the full client DSP chain. The WebSDR
   client is required to emit exactly 24 kHz stereo float32 (see [§5](#5-audio-mixing--the-aux-source)),
   so **no format work** is needed at the mixer.

PR #3612 also already provided an upstream half (worker-thread QWebSocket
client, decode, resample, self-rendered mini-waterfall). **That PR is closed and
its decoders are off-limits** (a derivative port — see its review); the
clean-room replacement is tracked separately. This design describes the
*integration contract* those pieces must satisfy, not their protocol internals.

---

## 3. Source model

### 3.1 v1 — per-slice audio-source override (recommended first)

Keep the slice a genuine Flex slice (real `VfoWidget`, real panadapter flag).
Add a **client-only** source flag to the slice's UI state — *never sent to the
radio* — with two values:

- `Radio` (default): today's behavior.
- `WebSdr`: the slice's audio comes from a WebSDR aux source instead.

When `WebSdr` is selected:
- The radio's real `rxant` / DSP state is left untouched (so switching back is
  just restoring it).
- A WebSDR aux source is engaged (see [§5](#5-audio-mixing--the-aux-source)) and
  the existing **follow-slice** sync (PR #3612) keeps the WebSDR tuned to the
  slice's frequency, so what you see on the Flex pan and what you hear stay
  aligned.

This is exclusive ("instead of the antenna"), which matches the antenna-menu UX
([§4](#4-ux-placement-the-rx-antenna-menu)).

### 3.2 v2 — virtual WebSDR receiver (optional, later)

A standalone `WebSdrReceiver` object — **not** a radio slice: created locally,
synthetic id (use **negative ids** to never collide with the radio's 0–7 pool),
never emits `commandReady`, owns its **own** mini-spectrum view, and routes
audio through its own aux source. This lets a
radio slice and a WebSDR receiver play **simultaneously**, each with independent
client-side gain/pan/mute. More work; defer until v1 proves the path.

> **Design rule:** v1 and v2 share the same audio aux-source + mixer layer
> ([§5](#5-audio-mixing--the-aux-source)). Build that layer once; v1 is the
> per-slice override on top of it, v2 is a second front-end on top of it.

---

## 4. UX placement: the RX antenna menu

The RX antenna control answers exactly the right question — *"where does this
slice's received signal come from?"* — and antenna selection is inherently
**exclusive**, which matches "receive from WebSDR **instead of** the antenna." So
the WebSDR source belongs **in that menu**, but as a **separated group**, not a
peer of ANT1/ANT2.

Current control: a button → menu built from `m_slice->rxAntennaList()`
([`VfoWidget.cpp:435-455`](../../src/gui/VfoWidget.cpp#L435-L455)); the list is
**radio-owned** (from `ant_list`/`rx_ant_list` status,
[`SliceModel.cpp:701-706`](../../src/models/SliceModel.cpp#L701-L706)); selecting
one sends `slice set <id> rxant=<name>`
([`SliceModel.cpp:95`](../../src/models/SliceModel.cpp#L95)).

Proposed menu:

```
RX source
  ─ Antenna ─
  • ANT1
    ANT2
    RX_A
  ─ Remote ─
    WebSDR…
```

Selecting an item in the **Remote** group is intercepted at the one menu handler
([`VfoWidget.cpp:452-453`](../../src/gui/VfoWidget.cpp#L452-L453)) and must do
three things — the parts that make this honest rather than a leaky overload:

1. **Suppress the radio command.** Do **not** call `setRxAntenna()` (the radio
   would reject `rxant=WebSDR`). Set the client-only source flag and engage the
   aux source instead.
2. **Signal that the whole receive chain relocated.** With WebSDR selected, the
   radio's per-slice DSP (NB/NR/AGC/filter), the **S-meter**, and the
   **panadapter trace** no longer correspond to what is heard. Grey/disable those
   *radio-side* affordances and mark the VFO as remote-sourced (the RX-antenna
   button already renders in blue `#4488ff` — use a distinct accent so the slice
   visibly reads "remote"). Crucially, keep the **client** DSP tab
   (AetherDSP/AetherVoice) fully live — it still applies (see [§6](#6-dsp-routing)).
3. **Handle coverage.** A real antenna receives whatever the slice is tuned to; a
   WebSDR only covers its server's bands. Grey the "WebSDR…" item (or show "no
   coverage") when the slice frequency is outside the chosen server's advertised
   band coverage; if the user tunes off-coverage while WebSDR is active, fall
   back to radio audio with a notice.

> **Anti-pattern:** do **not** let "WebSDR" silently occupy the antenna slot with
> no other UI change. The value of this placement comes from *also* relocating
> and greying the now-inert radio controls so the user understands the entire
> receiver moved, not just an antenna port.

---

## 5. Audio mixing — the aux source

Generalize the RADE second-buffer pattern into a small **N-source mixer** in
`AudioEngine`:

```cpp
// New, client-only. sourceId: stable per WebSDR slice/receiver.
void feedAuxAudio(int sourceId, const QByteArray& pcm24kStereoF32);

struct AuxSource {
    QByteArray            ring;        // accumulated PCM
    std::unique_ptr<Resampler> rs;     // src rate → device rate (built lazily)
    std::atomic<float>    gain{1.0f};
    std::atomic<int>      pan{50};     // client-side, 0..100
    std::atomic<bool>     mute{false};
};
QMap<int, AuxSource> m_auxSources;     // guarded for the audio thread
```

The RX drain loop ([`AudioEngine.cpp:737-765`](../../src/core/AudioEngine.cpp#L737-L765))
already mixes `m_rxBuffer + m_radeRxBuffer` with clamping; extend it to also sum
every active `AuxSource` (the same per-sample add + clamp, looped). RADE becomes
just one more aux source over time.

**Why a dedicated method and not the existing `feedWebSdrAudio()` gate:** PR
#3612's `feedWebSdrAudio()` → `feedAudioDataImpl()` → `m_rxSourceWebSdr` is a
**global switch** (the whole speaker flips Flex↔WebSDR). That is fine for v1
switch-mode but cannot express "radio slice A *and* a WebSDR slice at once." The
aux-source mixer is the superset that supports both.

**Format:** every aux source converges on **24 kHz stereo float32** before the
mix. WebSDR native audio is mono at a lower rate → `Resampler` to 24 kHz →
duplicate mono to L/R (handled by the WebSDR client before it reaches the mixer).

---

## 6. DSP routing

AetherDSP and AetherVoice are **client-side** and have **no radio dependency**,
so WebSDR audio can absolutely flow through them. The dividing line:

| DSP | Where it runs | Available to WebSDR audio? |
|---|---|---|
| Radio NB/NR/AGC/filter (`slice set …`) | FlexRadio firmware | **No** — WebSDR never touches the radio |
| **AetherDSP** — client RX chain `Eq→Gate→Comp→Tube→Pudu→DeEss` ([`AudioEngine.h:312-320`](../../src/core/AudioEngine.h#L312-L320)) | PC, on playback stream *after* radio NR ([`AudioEngine.h:307-311`](../../src/core/AudioEngine.h#L307-L311)) | **Yes** |
| **AetherVoice** — Aetherial channel strip ([`VfoWidget.cpp:1247`](../../src/gui/VfoWidget.cpp#L1247)) | PC, same family | **Yes** |
| Client NR (NR2/NR4/DFNR) in `feedAudioDataImpl` | PC | **Yes** |

A genuine upside: the **client** chain is the *only* NR available to a remote
feed, and WebSDR gets it — so a WebSDR-sourced slice still benefits from
AetherSDR's noise reduction and voice strip even though the radio's own DSP
can't reach it.

### The positional catch: combined-bus vs per-source

The client chain currently lives on the **single combined stream** (one
`ClientEq`/`ClientComp`/… instance on `m_rxBuffer`). That cleanly supports two of
three cases and forces a decision on the third:

| Mode | Status | DSP behavior |
|---|---|---|
| **Switch** (speaker = WebSDR) | works (PR #3612 path) | route WebSDR through `feedAudioDataImpl` → full AetherDSP/AetherVoice |
| **Coexist, shared DSP** | small change | sum sources into `feedAudioDataImpl` → one shared EQ/NR setting |
| **Coexist, per-slice DSP** | needs refactor | one chain instance can't hold two settings |

> **Trap:** copying the RADE pattern naively **bypasses** the client chain —
> RADE is mixed at the drain *after* the chain
> ([`AudioEngine.cpp:1388-1394`](../../src/core/AudioEngine.cpp#L1388-L1394)), so
> a RADE-style WebSDR source would get **no** AetherDSP. The injection point is a
> real choice: *through `feedAudioDataImpl`* (gets DSP; shared/switch) vs
> *separate aux buffer* (coexists; bypasses the chain unless per-source chain
> instances are added).

For true **per-slice DSP**, split the chain like a mixing console:

- **Per-source, pre-mix:** NR, gate, AGC — one instance per source (radio bus,
  each WebSDR receiver), because each has different noise.
- **Master, post-mix bus:** EQ, comp, tube, pudu, de-ess, output trim, resample —
  applied once to the final monitor mix (output character belongs on the whole
  bus).

This split benefits RADE and any future source, not just WebSDR.

---

## 7. Prerequisites & caveats

- **Licensing gates audio flow.** All WebSDR audio passes through
  `WebSdrAudioDecoder`, the codec port flagged in the PR #3612 review as an
  unlicensed derivative of PA3FWM's client JS. **Audio cannot legally flow until
  the decoder provenance is resolved** (clean-room-from-pcaps, or PA3FWM's
  permission). The mixer ([§5](#5-audio-mixing--the-aux-source)) and DSP-split
  ([§6](#6-dsp-routing)) work is license-clean and can land *ahead* of the codec.
- **Cross-platform.** Everything here is Qt (`QAudioSink`, `QWebSocket`,
  `Resampler`) — no platform-specific paths. Keep it that way.
- **Service etiquette / attribution.** Per PA3FWM's FAQ his line is
  *misrepresentation*, not access; he permits reusing the live server list and
  feeding WebSDR audio to other software. Connecting an in-app client is in
  spirit **provided** the WebSDR and its operator are clearly attributed in the
  UI, and load stays polite (single sockets, exponential backoff).

---

## 8. Phased plan

| Phase | Deliverable | Notes |
|---|---|---|
| **P0** | Resolve decoder licensing (permission or clean-room) | Blocks audio; do in parallel. |
| **P1** | `feedAuxAudio` N-source mixer in `AudioEngine` | License-clean; generalize the RADE mix; the load-bearing, concurrency-subtle piece. |
| **P2** | v1 per-slice source override + grouped antenna menu + relocate/grey radio controls | The "receive from WebSDR instead" flow, switch-mode. Full AetherDSP/AetherVoice for free via `feedAudioDataImpl`. |
| **P3** | Per-source / master-bus DSP split | Enables coexist-with-per-slice-DSP. |
| **P4** | v2 virtual WebSDR receiver | Standalone receiver + own waterfall, on the same aux-source layer. |

---

## 9. Key code references

- Slice creation (radio-coupled): [`RadioModel.cpp` `handleSliceStatus`](../../src/models/RadioModel.cpp), [`SliceModel.cpp:41`](../../src/models/SliceModel.cpp#L41)
- RX antenna control: [`VfoWidget.cpp:435-455`](../../src/gui/VfoWidget.cpp#L435-L455), [`SliceModel.cpp:91-96,701-706`](../../src/models/SliceModel.cpp#L91-L96)
- Aux-source mix point (RADE template): [`AudioEngine.cpp:737-765`](../../src/core/AudioEngine.cpp#L737-L765)
- Client DSP chain (AetherDSP): [`AudioEngine.h:307-326`](../../src/core/AudioEngine.h#L307-L326)
- AetherVoice launcher: [`VfoWidget.cpp:1247`](../../src/gui/VfoWidget.cpp#L1247)
- RADE bypasses the chain (the trap): [`AudioEngine.cpp:1388-1394`](../../src/core/AudioEngine.cpp#L1388-L1394)
- Format-agnostic injection: `AudioEngine::feedAudioDataImpl()`
- WebSDR client (the upstream half): to be provided by the clean-room initiative. The closed PR #3612's `WebSdrSource` / `WebSdrAudioDecoder` / `WebSdrWaterfallDecoder` are a derivative port and **off-limits** — do not read them; the clean-room client must satisfy the integration points in [§5](#5-audio-mixing--the-aux-source)–[§6](#6-dsp-routing) instead.
- See also [`audio-pipeline.md`](audio-pipeline.md) for the end-to-end client audio graph.
