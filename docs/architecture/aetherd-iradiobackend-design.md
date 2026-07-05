# IRadioBackend — the radio-facing seam (aetherd RFC step 2) — DESIGN DRAFT

**Status:** Draft for maintainer review. Step 2 of the accepted aetherd RFC
([`../aetherd-headless-engine-design.md`](../aetherd-headless-engine-design.md)
§5.5, §10). Cannot land until step 1 (#4047) merges — no stacking (RFC §10).
**Scope:** extract a radio-family-agnostic interface inside `libaethercore` so
`RadioModel` and friends stop reaching directly into SmartSDR-specific wire
classes, and wrap today's stack as `FlexBackend` with **zero behavior change**.

Vendor-agnostic by design: this doc names no specific non-Flex radio. The
first backend is `FlexBackend`; the interface is what any second backend
implements.

---

## 1. What the seam must cover (measured, not guessed)

The touchpoint audit
([`aetherd-touchpoints.md`](aetherd-touchpoints.md)) tags every gui→engine
header. Two tag classes define step 2's work:

- **26 `vendor` headers** — SmartSDR/FlexLib/Flex-ecosystem wire code. These
  move *behind* the seam into `src/core/backends/flex/`; nothing above the
  seam includes them. The load-bearing ones: `RadioConnection` (SmartSDR TCP
  text), `PanadapterStream` (VITA-49), `SmartLinkClient`/`WanConnection`
  (SmartLink WAN), `CommandParser`, `StreamStatus`, `RadioStatusOwnership`,
  plus the 4O3A/amp/tuner/firmware/waveform/DAX-IQ family.
- **16 `mixed` headers** — canonical state fused with Flex specifics. These do
  *not* move; they get **split**: the core-profile part stays as the model the
  UI/protocol sees, the Flex part becomes backend-provided. The hard five are
  `RadioModel`, `SliceModel`, `TransmitModel`, `PanadapterModel`, `MeterModel`.

The 43 `universal` + 42 `ui-support` headers are untouched by step 2.

## 2. The interface

`IRadioBackend` lives in `src/core/backends/IRadioBackend.h`. It is the *only*
thing `RadioModel` holds toward the wire. Shape (illustrative, not final):

```cpp
class IRadioBackend {
public:
    virtual ~IRadioBackend() = default;

    // ---- identity & capability (feeds the protocol `welcome`, RFC §4.1) ----
    virtual RadioCapabilities capabilities() const = 0;   // slices, sample
        // rates, TX power range, tuner/amp presence, extension namespaces

    // ---- lifecycle ----
    virtual void connect(const RadioConnectInfo&) = 0;
    virtual void disconnect() = 0;
    // signals (Qt): connected(), disconnected(), connectionError(QString)

    // ---- intents DOWN (canonical verbs; backend translates to its wire) ----
    virtual void setSliceFrequency(int sliceId, double hz) = 0;
    virtual void setSliceMode(int sliceId, const QString& mode) = 0;
    virtual void setSliceFilter(int sliceId, int lowHz, int highHz) = 0;
    virtual void setKeying(bool key) = 0;                 // gated ABOVE this
    // ... the enumerated core-profile setters (from the `universal` tag set)

    // ---- vendor extensions (namespaced, capability-advertised) ----
    // Fire-and-forget: the reply arrives async via extensionResult/Error,
    // keyed by the caller's requestId (0 = no reply expected).
    virtual void invokeExtension(const QString& ns, const QString& verb,
                                 quint64 requestId, const QVariant&) = 0;

    // ---- state & streams UP (normalized) ----
    // signals: sliceChanged(id, kvs), meterUpdate(...),
    //          extensionResult(requestId, QVariant), extensionError(requestId, reason),
    //          spectrumFrame(...), waterfallRow(...), audioFrame(...) — §4.2 formats
};
```

Key properties:

- **DSP location is invisible here.** A backend whose hardware demodulates and
  computes FFTs is a thin protocol decoder; a backend that ships raw samples
  owns an engine-side DSP chain and emits the *same* `spectrumFrame`/`audioFrame`
  signals. Above the seam, identical. (RFC §5.5.)
- **Keying is a single canonical intent.** `setKeying(bool)` is translated by
  the backend to its mechanism (a command verb, an in-stream bit, a hardware
  line). The TX guard/arbitration sits **above** the seam (RFC §6) — no backend
  self-polices; step 4 wires the guard, step 2 just routes the intent.
- **Capabilities are declared, not assumed.** `RadioCapabilities` is the honest
  self-description the RFC §4.1 `welcome` needs; it replaces the current
  implicit "the radio is a Flex" assumption and kills the model-impersonation
  anti-pattern (RFC §1, PR #4027).

## 3. Splitting the five hard `mixed` models

This is the genuine design work; the vendor-header moves are mechanical. Guiding
rule: **the model holds canonical state the UI/protocol needs; the backend owns
the wire semantics that produce and consume it.**

| Model | Core-profile (stays in the model) | Flex extension (backend-owned) |
|---|---|---|
| `RadioModel` | connection state, the slice/pan/meter/transmit sub-model set, frequency/mode plumbing | Multi-Flex handles, GUIClientID session restore, SmartSDR status routing, `m_connection`/`m_panStream` ownership |
| `SliceModel` | freq, mode, filter low/high, RX gain, active flag, S-meter binding | DAX channel, `client_handle` ownership, Flex slice-index quirks |
| `TransmitModel` | keying intent, TX power setpoint, mic/monitor levels, the CHAIN DSP stage state | ATU/TGXL/amp verbs, Flex profile load, `mox`-less interlock decode |
| `PanadapterModel` | center, span, min/max dBm, per-pan display state | FlexLib stream-id (0x40xx/0x42xx) wiring, `display pan` status keys |
| `MeterModel` | the meter values the UI renders (via `MeterSmoother`) | Flex meter-id catalog, SLC/LEVEL source decode |

Approach: keep the existing model classes as the core-profile shape; move the
Flex-specific *decode/encode* out of them and into `FlexBackend`, which drives
the models through the same signals the wire classes emit today. The models
already communicate via signals (RFC §2 fact #4), so this is decoupling the
producer, not rewriting the model.

## 4. FlexBackend — zero behavior change (the acceptance bar)

`FlexBackend` (`src/core/backends/flex/`) owns the existing
`RadioConnection` + `PanadapterStream` + `RadioDiscovery` +
`SmartLink`/`Wan`/`Tgxl`/`Pgxl` classes unchanged, and implements
`IRadioBackend` by delegating to them. The acceptance bar is the same as step
1: **byte-identical behavior on the slice-0 RX flow**, proven by
`tools/verify_slice0_rx.py` (the harness added in #4047) run before and after,
plus the 61-test suite and a live-radio bridge pass.

Step 2 does **not** add the protocol, the daemon, or any client change — the
desktop app still links `libaethercore` in-process and drives the backend
directly. It only inserts the interface.

## 5. Sequencing within step 2 (each a small, independent PR off main)

1. **Land `IRadioBackend.h` + `RadioCapabilities`** (interface only, no
   implementor wired) — pure addition.
2. **`FlexBackend` skeleton** delegating to the existing classes, wired behind
   `RadioModel` via a `std::unique_ptr<IRadioBackend>`, with the direct
   member access (`m_connection`, `m_panStream`) routed through it. Behavior
   unchanged; harness + tests gate it.
3. **Split the five models** one at a time (5 PRs), moving Flex decode into
   `FlexBackend`. Each PR: one model, harness-verified, manifest row flips.
4. **Move the remaining 21 vendor headers** behind the seam (mechanical, a few
   PRs grouped by subsystem — amp/tuner, firmware/waveform, SmartLink).

Each step is behavior-neutral and independently revertable — the trunk-based
discipline from RFC §10. The KiwiSDR path (tagged `vendor`) becomes the second
backend *after* the seam is proven, retiring its side-channels (RFC §5.5).

## 6. Resolved decisions (2026-07-05, KK7GWY — proceed-with-recommendations)

1. **`RadioCapabilities` shape** → **typed struct + `QVariantMap extensions`**
   escape hatch. Typed where universal, open where vendor. Implemented in
   `src/core/backends/RadioCapabilities.h`.
2. **Sub-model ownership** → **`RadioModel` keeps owning `SliceModel` et al.;
   the backend drives them** by emitting normalized signals RadioModel connects
   to. Minimal churn; the models already communicate via signals.
3. **One interface vs RX/TX split** → **one interface.** A receive-only family
   reports `capabilities().canTransmit == false` and implements `setKeying()`
   as a guarded no-op; the engine TX guard (§6, above the seam) denies keying
   when `canTransmit` is false. No type split.
4. **Directory layout** → **`src/core/backends/`** confirmed: the interface at
   `src/core/backends/IRadioBackend.h`, each family under
   `src/core/backends/<family>/` (FlexBackend → `src/core/backends/flex/`).

These are settled as the working defaults; any can be revised in review while
the interface has no implementor.

## 7. Sub-step status

- **2.1 — interface + capabilities (this PR):** `IRadioBackend.h` +
  `RadioCapabilities.h` land as a pure addition (header-only, AUTOMOC-listed,
  no implementor wired). Zero behavior change.
- **2.2 — `FlexBackend` skeleton (this PR):** `FlexBackend` implements
  `IRadioBackend`, is constructed/owned/torn-down by `RadioModel` in the real
  connection lifecycle, and observes live wire lifecycle events (re-emitting
  connected/disconnected/connectionError). The core verbs build the exact
  SmartSDR strings into RadioModel's command sink (scaffold for 2.3). Purely
  ADDITIVE: the wire objects and the command hot path are NOT rerouted, so
  behavior is unchanged. AGENTS.md gets the "where backend code goes" routing
  table.
- **2.2b — move wire ownership behind the seam:** `FlexBackend` absorbs
  `RadioConnection`/`PanadapterStream` + their worker threads and the #502
  teardown ordering; `RadioModel::connection()`/`panStream()` delegate. The
  single riskiest move (thread lifetime + slice-0 RX plane) — its own
  harness-verified PR.
- **2.3 — split the five `mixed` models** (5 PRs); AGENTS.md gains the
  touchpoint-claim protocol + verification recipe + the autonomous-conversion
  carve-out.
- **2.4 — move the remaining vendor headers behind the seam.**
