# Hermes-Lite 2 Backend — Design Note

**Status:** Draft for maintainer review. A *new radio family* under the accepted
aetherd RFC §5.5, gated by AGENTS.md's rule that a new family "requires an
approved design doc naming its open protocol authority" (`AGENTS.md:334`). This
note is that prerequisite.

**Scope:** An in-tree `Hl2Backend : IRadioBackend` for the Hermes-Lite 2 (HPSDR
Protocol 1 / "Metis", raw-IQ over UDP:1024). Phase-1 target is a **working
receive path** — discover → stream → tune → engine-side demod → panadapter/audio
— behind the existing seam, with **zero** change to any UI or model consumer of
`IRadioBackend`. Transmit is explicitly out of scope for Phase 1. The data plane
and register facts are already proven against real hardware by the throwaway
spike in `prototypes/hl2/` (see its README); this note is the plan to port that
proof in-tree.

---

## 1. Why HL2 is the interesting backend

`IRadioBackend.h` was written anticipating exactly this case
(`src/core/backends/IRadioBackend.h:50-53`):

> DSP location is invisible here: a backend whose hardware demodulates and
> computes FFTs is a thin protocol decoder; one that ships raw samples owns an
> engine-side DSP chain — either way it emits the same normalized signals, so no
> consumer can tell the difference.

FlexBackend is the "thin protocol decoder" half: Flex hardware ships cooked audio
(24 kHz PCM) plus a hardware spectrum, and FlexBackend's job is almost entirely
`decodeXStatus` translation (FlexBackend's `decode*Status` translators). **HL2 is the other
half** — it ships nothing but raw IQ, so `Hl2Backend` must own an engine-side RX
DSP chain (tune → decimate → demodulate → FFT → S-meter) and then emit the same
normalized `sliceChanged` / `meterUpdate` / spectrum / audio outlets. It is the
first backend that exercises the "owns a DSP chain" branch of the seam, and thus
the first real test that the seam's abstraction holds.

**What the spike already proved** (`prototypes/hl2/`, live against a real HL2 at
gateware 7.4):

- Metis discovery (board id `0x06`), EP2/EP6 1032-byte framing, 0.00% loss.
- Sample-rate control (48/96/192/384 kHz) via the config C&C register.
- **RX1 NCO tuning** (`C0=0x04`, 32-bit Hz BE) — WWV 10 MHz lands exactly at
  baseband DC.
- The non-obvious must-set: **`CONFIG_MERCURY`** (config C1 bit 6) selects the
  ADC as the DDC source; without it the stream is flat ADC-floor noise.
- LNA gain (`C0=0x14`, `C4 = 0x40 | (dB+12)`), −12…+48 dB.
- End-to-end FFT panadapter resolving WWV ~50 dB over the noise floor.

Protocol authority (Principle I): openHPSDR Protocol 1, the Hermes-Lite 2 wiki /
gateware, and the pihpsdr reference client — all consulted **clean-room**, no
code incorporated. Recorded under Protocol References in `THIRD_PARTY_LICENSES`.

---

## 2. What `Hl2Backend` owns

Mirroring FlexBackend's ownership shape (it owns its wire objects and their
worker threads, constructed in a load-bearing order — FlexBackend's constructor),
`Hl2Backend` owns two internal, below-seam objects, each on its own worker
thread:

```
Hl2Backend : IRadioBackend
├── MetisClient        (UDP :1024 wire — discovery, start/stop, EP2 C&C egress,
│                       EP6 IQ ingest; owns the socket + its RX thread)
│      emits: iqBlockReady(complex<float> block), linkUp/linkDown, dropStats
│      accepts: setRxFrequencyHz(), setSampleRate(), setLnaGainDb()  (C&C)
└── Hl2RxDsp          (engine-side RX chain on its own thread)
       in:  raw IQ blocks from MetisClient
       out: demodulated PCM  → audio outlet
            FFT magnitude bins → spectrum/waterfall outlet
            S-meter level      → meterUpdate
```

`MetisClient` and `Hl2RxDsp` are a direct in-tree port of the proven
`prototypes/hl2/hpsdr.py` primitives (register map, framing, DC-removed FFT).
The DSP building blocks the chain needs — **liquid-dsp, FFTW, and the `WfmDsp`
pattern — are already in the tree** (`docs/aetherd-headless-engine-design.md:309-318`),
so `Hl2RxDsp` is assembly of existing parts, not new DSP.

Everything in this box lives under `src/core/backends/hl2/`, which is **below the
seam**: EB3 never inspects its includes and it may use any vendor/DSP header
freely (`tools/check_engine_boundary.py:66-67, 326-327`; `AGENTS.md:316-318`).
Only EB1/EB2 apply — no `gui/` include, no QtWidgets — which this backend never
needs. Adding the HL2 tree touches **no** ratchet baseline row and requires **no**
`aetherd-touchpoint-tags.json` change (nothing HL2 ends up above the seam).

---

## 3. Seam mapping

Each `IRadioBackend` verb/signal → HL2 mechanism. "Engine DSP" means the field
configures `Hl2RxDsp`, not the radio — the fundamental HL2 difference from Flex,
where the same field would be a wire command.

### Intents DOWN

| Interface verb | HL2 realization |
|---|---|
| `connectRadio(req)` | **Real, unlike Flex's stub.** `MetisClient` opens the socket to `req.host:1024` (or a discovered HL2), sends the 3-register init (config incl. `CONFIG_MERCURY`, RX1 freq, LNA gain), starts EP6 ingest. Emits `connected()` on first EP6, `connectionError()` on timeout. |
| `disconnectRadio()` | Send Metis stop (`EF FE 04 00`), stop threads in reverse order. |
| `isConnected()` | `MetisClient` link state (EP6 flowing). |
| `setSliceFrequency(id, hz)` | `MetisClient::setRxFrequencyHz` → RX1 NCO `C0=0x04`. (Phase 1: single slice, `id` fixed.) |
| `setSliceMode(id, mode)` | **Engine DSP** — selects the `Hl2RxDsp` demodulator (AM/SSB/CW/FM/…). HL2 ships raw IQ, so mode is purely engine-side. |
| `setSliceFilter(id, lo, hi)` | **Engine DSP** — sets the demod passband in `Hl2RxDsp`. |
| `setKeying(bool)` | **Guarded no-op** — `capabilities().canTransmit == false`, so the engine TX guard (RFC §6) denies keying above the seam; the backend never keys. (TX is Phase 2+.) |
| `invokeExtension(...)` | Stub, exactly like FlexBackend (FlexBackend::invokeExtension): if `requestId != 0`, emit `extensionError` immediately. HL2 advertises **no** extension namespaces (see §4). |

### State UP

| Interface signal | HL2 source |
|---|---|
| `connected` / `disconnected` / `connectionError` | `MetisClient` link transitions. |
| `sliceChanged(id, SliceDelta)` | Emitted from the backend's **own authoritative state**. HL2 has no status wire echoing frequency/mode back (unlike Flex's `decodeSliceStatus`); the engine is the source of truth, so the backend reflects the settings it applied. |
| `meterUpdate("s-meter", v)` | `Hl2RxDsp` S-meter (dBFS → dBm via a calibration constant; TBD, refine like `flex-meter-learnings.md`). |
| `panCenterBandwidthChanged(panId, ctr, span)` | Center = RX1 NCO MHz; span = sample-rate MHz. Backend-emitted from its own config. |
| `panRfGainChanged(panId, gain)` | LNA gain (−12…+48 dB) reflected back. |
| `spectrumFrameReady` / `waterfallRowReady` / `audioFrameReady` | The DSP outlets — **but see §5 for how these actually reach the UI in Phase 1** (the interface's `QByteArray` data-plane signals are not the live path yet). |

Signals HL2 simply never emits in Phase 1 (no such hardware/wire): `transmitChanged`,
`amplifierChanged`, `tunerChanged`, `gpsChanged`, `memoryChanged`, `profileChanged`,
`meterDefined` catalog. RadioModel already tolerates a backend that stays silent
on these — the wiring is per-signal `connect()` with no "all-must-fire" contract
(RadioModel's interface-typed backend signal wiring).

---

## 4. Capabilities `Hl2Backend` advertises

Per `src/core/backends/RadioCapabilities.h:25-55`:

```cpp
RadioCapabilities caps;
caps.family              = "hl2";
caps.model               = "Hermes-Lite 2";      // + gateware ver from discovery
caps.maxSlices           = 1;                     // Phase 1; gateware supports more DDCs
caps.maxPanadapters      = 1;
caps.sampleRatesHz       = {48000, 96000, 192000, 384000};
caps.canTransmit         = false;                 // RFC §6 guard denies keying
caps.txPowerMaxWatts     = 0.0;
caps.hasTuner            = false;
caps.hasAmplifier        = false;
caps.hasExtendedDsp      = false;                 // DSP is engine-side, not firmware
caps.extensionNamespaces = {};                    // advertise NONE until step 3
```

The empty `extensionNamespaces` follows FlexBackend's deliberate precedent
(FlexBackend's capabilities setup): advertising a namespace whose `invokeExtension`
can't yet reply would hang a client. HL2 keeps it empty until the step-3 encode
path lands.

---

## 5. The two structural gaps HL2 forces (and how)

FlexBackend quietly bypasses the seam in two places. HL2 cannot, because it has
no Flex-shaped `RadioConnection`/`PanadapterStream` for RadioModel to reach into.
This is a feature: HL2 is the forcing function that closes both gaps minimally.

### Gap A — backend selection (no factory today)

`FlexBackend` is **hard-wired**: a `make_unique<FlexBackend>()` literal in
RadioModel's backend construction, plus a transitional concrete `FlexBackend* m_flexBackend`
alias and Flex-specific sink injection / object grabs
(`setCommandSink`, `connection()`, `panStream()`) — all in RadioModel.
The *signal-wiring* half (the interface-typed half) is already interface-typed and reusable;
the *construction* half is Flex-shaped.

**Proposal (minimal):** introduce a tiny backend-selection seam in RadioModel —
a `makeBackend(family)` that returns `unique_ptr<IRadioBackend>` — and move the
Flex-specific sink/object wiring behind a `if (auto* flex = dynamic_cast<FlexBackend*>...)`
adapter step, so a non-Flex backend simply skips it. This is the smallest change
that lets a second family exist; it does **not** require the full step-3 protocol.
The `dynamic_cast` shim is **explicitly interim** — it is the transitional stand-in
until the step-3 engine-side registry lands and each backend injects its own
sinks; the implementation PR should label it as such so it doesn't calcify.
Selection input for Phase 1 can be explicit (config/UI "radio family = HL2"),
deferring auto-discovery-driven selection.

### Gap B — data plane not through the seam

The interface's `spectrumFrameReady/audioFrameReady` (`QByteArray`) are **declared
but never emitted** by FlexBackend; the live path is the existing in-tree frame
types flowing off `PanadapterStream` and reached via `RadioModel::panStream()` —
`spectrumReady(QVector<float> binsDbm)` / `audioDataReady(QByteArray pcm)` →
`AudioEngine::feedAudioData` (`PanadapterStream.h:208-217`; MainWindow's audio-feed wiring).
The concrete seam frame formats are **step-4 work, not final**
(`IRadioBackend.h:210-216`; `aetherd-headless-engine-design.md:251-266`).

**Proposal (Phase 1 relays existing types):** `Hl2RxDsp` produces the **same**
in-tree frame types the UI already consumes — `QVector<float>` dBm spectrum bins,
`QByteArray` 24 kHz PCM. Rather than invent a step-4 format now, expose them the
way RadioModel already consumes Flex's: the cleanest minimal option is a small
`spectrumSource()`/audio adapter on the backend that RadioModel binds the same
way it binds `panStream()` today. When step-4 finalizes the binary frame formats,
HL2 (and Flex) migrate to `spectrumFrameReady/audioFrameReady` together — HL2's
DSP output is already frame-shaped, so that migration is a re-wrap, not a rewrite.

**Sequencing:** both gaps are closable **without** waiting on step-3 (protocol) or
a finalized step-4 format. HL2 Phase 1 depends only on: (A) the small selection
seam, and (B) relaying existing frame types. TX, vendor extensions, remote/binary
data plane, and multi-RX all wait for their respective later steps.

---

## 6. Explicitly out of scope for Phase 1

- **Transmit.** `canTransmit=false`; `setKeying` is a guarded no-op. TX needs the
  MOX bit + a TX-IQ data plane + the engine TX arbiter (RFC §6 / step 4-arbitration)
  and is deliberately deferred — it is also the only path that can key a radio, so
  keeping it out of Phase 1 is the safe default.
- **Multiple DDC receivers.** Gateware supports them; Phase 1 is one slice / one pan.
- **Vendor extensions.** No namespaces advertised until the step-3 encode path exists.
- **Discovery-driven UI selection / multi-radio.** Phase 1 connects to an explicit
  host; enumeration is a later enhancement (the spike's `discover.py` proves it works).

---

## 7. Clean-room provenance (allowed inputs)

Consistent with the KiwiSDR precedent (`docs/kiwisdr-cleanroom-design.md`) and the
`THIRD_PARTY_LICENSES` Protocol-References entry. `Hl2Backend` is implemented from
clean AetherSDR interfaces plus **protocol facts** (not code) consulted from
GPL/open sources. Allowed inputs used:

- **openHPSDR Protocol 1** (TAPR, GPL-2.0) — Metis discovery handshake, EP2/EP6
  1032-byte frame layout, the C0 `(addr<<1)|MOX` register-address encoding.
- **Hermes-Lite 2 wiki / gateware** (KF7O + contributors) — board id `0x06`,
  supported sample rates, register `0x0a` extended-range LNA gain, the
  `CONFIG_MERCURY` ADC-source-select bit.
- **pihpsdr `src/old_protocol.c`** (DL1YCF, GPL-3.0) — consulted as a *behavioral
  reference* for the minimal round-robin C&C init sequence a receiver needs.
- **Live black-box observation** of the AetherSDR-owned HL2 (register effects on
  the IQ stream), recorded in `prototypes/hl2/`.

No openHPSDR, Hermes-Lite 2, or pihpsdr source is incorporated, vendored,
translated, or linked. The facts above are expressed in original AetherSDR code.
Because AetherSDR is itself GPL-3, direct adaptation would also be permissible,
but clean-room is chosen to match project convention.

---

## 8. Resolved decisions & open questions

**Resolved**
- HL2 is one `IRadioBackend` implementor, RX-only in Phase 1 (RFC §5.5 Q3: one
  interface, `canTransmit=false`, guarded `setKeying`).
- DSP is engine-side and encapsulated below the seam; consumers see the same
  normalized signals (RFC §5.5, `IRadioBackend.h:50-53`).
- Below-seam placement under `src/core/backends/hl2/` — no EB3/tags impact.
- Protocol authority named and consulted clean-room (Principle I / AGENTS.md:334).

**Open (for maintainer input)**
- **Backend-selection seam shape (Gap A):** `makeBackend(family)` factory in
  RadioModel now, vs. deferring until step-3 lands a fuller engine-side registry.
  Recommendation: the minimal factory now — HL2 is the concrete second family
  that justifies it, and it's a prerequisite for HL2 existing at all.
- **Phase-1 data-plane wiring (Gap B):** relay existing `QVector<float>`/`QByteArray`
  frame types via a `panStream()`-analogue, vs. emitting the interface's
  `QByteArray` data-plane signals early with a RadioModel bridge. Recommendation:
  relay existing types (matches how Flex reaches the UI today; least churn; both
  families migrate to step-4 formats together later).
- **S-meter calibration:** dBFS→dBm constant for the AD9866 front end; refine
  empirically as in `docs/architecture/flex-meter-learnings.md`.

---

## 9. Phasing

1. **1a — MetisClient + Hl2RxDsp in-tree** (port the spike; unit-tested against
   captured EP6 fixtures). No RadioModel change yet.
2. **1b — backend-selection seam** (Gap A): `makeBackend`, Flex-sink adapter step.
3. **1c — data-plane relay** (Gap B): HL2 DSP frames reach the existing UI path;
   first live in-app HL2 panadapter + audio.
4. **1d — capabilities + connect UX**: family reporting, explicit-host connect,
   error surfacing.

Later (own steps): discovery-driven selection, multi-RX, transmit, vendor
extensions, migration to finalized step-4 binary frames.
