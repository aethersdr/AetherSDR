# AetherD — Headless Engine & UI Decoupling — Design (RFC)

**Status:** **Accepted** — 2026-07-04, Jeremy/KK7GWY. All 17 decisions
approved as recommended; record in
[`aetherd-rfc-signoff.md`](aetherd-rfc-signoff.md). Implementation proceeds
per §10.
**Author:** Claude (Opus 4.8) with architecture direction by @ten9876
**Date:** 2026-06-26 (revised 2026-07-04: pluggable radio backends)
**Scope:** Split AetherSDR into a headless engine daemon (`aetherd`) that owns the
radio connection and all of `core/`+`models/`, plus thin UI clients (Qt desktop,
browser/web, TUI, scripting) that speak a versioned protocol — so the UI can be
radically changed, replaced, or remoted **without rebuilding the engine**, and
new radio families can be added below the same boundary **without touching any
client** (§5.5).

> Per [`GOVERNANCE.md`](../GOVERNANCE.md#rfc-process) this is an **architecture
> change** (threading, signal routing, process model) and therefore requires an
> approved RFC before a PR is opened.

---

## 0. Open decisions (need maintainer sign-off)

Bias for all decisions: **successful, consistent operation for every operator
skill level**, and **TX safety is never weakened by the split** (Principle VI).

| # | Decision | Recommendation | §  |
|---|----------|----------------|----|
| Goal scope | What problem are we actually solving? | **Remote/web/multi-client** — if it's only desktop re-styling, prefer QML (§9, Alternative A) | §1 |
| Process model | One daemon, many clients? | **Yes** — `aetherd` owns the radio; UIs are peers | §3 |
| Local transport | Control plane on same box | **`QLocalServer`** (already used by the bridge) | §4.1 |
| Remote transport | Control plane off box / browser | **WebSocket** (already linked: `Qt6::WebSockets`) | §4.1 |
| Remote security | Protecting off-box access | **Ship with WireGuard** — tunnel supplies encryption + peer identity; engine auth maps peers to capability grants | §6, §8 |
| Data plane (local) | FFT/audio same box | **Shared-memory ring buffer** (zero-copy) | §4.2 |
| Data plane (remote) | FFT/audio off box | **Binary frames + Opus** (reuse `OpusCodec`/VITA framing) | §4.2 |
| TX arbitration | Multiple UIs, who keys? | **Single-holder TX lock + per-client auth**, enforced in engine | §6 |
| Multi-client view | Shared or per-client view state? | **Per-client projection over shared radio state** | §5 |
| Radio backends | One radio family, or pluggable? | **Pluggable `IRadioBackend`** below the models; the SmartSDR stack becomes the first backend | §5.5 |
| Capabilities | Clients face radios with different feature sets | **Capability descriptor in `welcome`** — clients render what the radio reports, no hard-coded assumptions | §4.1 |
| Multi-session | One daemon, many radios? | **Session-namespaced protocol** over the existing `RadioSession` aggregate | §5 |
| Repo layout | Separate repo for `aetherd`? | **Monorepo** — engine + desktop client stay in this repo permanently; revisit only for far-side clients (web UI, SDKs) after protocol v1 stabilizes | §10 |

**Flagged tension:** a decoupled/scriptable UI is a new path to the
transmitter. Mitigation is non-negotiable and load-bearing: **the entire TX
guard lives in `aetherd`, below the protocol boundary; the client is never
trusted** (§6).

---

## 1. Problem

The UI cannot be changed without rebuilding the whole app, and there is no way
to run a different UI technology (web, remote thin client, TUI) against the
engine. Today AetherSDR is a **single monolithic executable** —
`add_executable(AetherSDR ${ALL_SOURCES})` compiles `models/`+`core/`+`gui/`
into one binary — and the UI is **100% imperative QWidget** (128 `QWidget`
subclasses, 0 QML). Any UI change relinks everything, and "a different UI"
isn't expressible at all.

The same hard-wiring exists on the radio side of the engine. `RadioModel`
directly owns `RadioConnection` (SmartSDR TCP text) and `PanadapterStream`
(FlexLib VITA-49 parsing); there is no abstraction between the models and the
vendor wire protocol, so supporting any other radio family today would mean
threading a second protocol through the same monolith. Two exhibits show what
the missing seam forces on people:

- **In-tree**: the one non-Flex source (the receive-only KiwiSDR path) had to
  bypass the architecture with per-source side-channels
  (`AudioEngine::feedKiwiSdrAudioData`,
  `SpectrumWidget::updateKiwiSdrWaterfallRow`) — ad-hoc injection is the only
  option the current structure offers.
- **Out-of-tree**: the community bridges non-Flex hardware (IC-9700, TS-450S
  via Aether-gate/flex-sim, PR #4027) by **impersonating a Flex model and then
  patching the lies with protocol-extension keys** (`bands=`). The
  impersonation is structurally leaky — capability truth splits between the
  fake model string and the patch keys, and the client resolves commands
  against whichever it consults first (PR #4027's review found band-stack keys
  that depend on the *impersonated* model's capability flags). A real radio
  family needs to be a first-class backend (§5.5) declaring its own
  capabilities (§4.1), not a Flex costume.

We want a boundary such that the engine runs headless and **any** front end —
including a browser on another device — drives it over a stable protocol; and
such that the radio-facing side of the engine is equally pluggable: **any**
radio family, speaking its own wire protocol, presents itself through the same
models and streams (§5.5).

---

## 2. Background: what already exists (the load-bearing facts)

This RFC is viable *specifically because* four of the five hard pieces are
already in the tree:

1. **A clean engine boundary is (almost) already enforced.** The dependency
   arrow points gui→core with exactly one tracked exception
   (`AutomationServer.cpp` includes `gui/ConnectionPanel.h`) plus five engine
   files that use QtWidgets — all six are step-1 relocation/seam work, and
   `tools/check_engine_boundary.py` (CI: `engine-boundary.yml`) now blocks any
   *new* leakage while that legacy set shrinks to zero. The engine can
   otherwise already run with no UI attached. This is the expensive invariant
   most codebases never hold.
2. **A model-aware control protocol already exists.** `AutomationServer`
   (`src/core/AutomationServer.cpp`) speaks JSON over a `QLocalServer`:
   `{"cmd":"get|set|invoke", "model":…, "selector":…, "property":…, "value":…}`
   → `{"ok":true|false, "error":…}`, plus `dumpTree` introspection. It already
   does property get/set by selector (frequency, `masterVolume`, per-slice) and
   `invoke` on actions. It is used for test automation today, but it is a real
   RPC surface, not click-simulation.
3. **Streaming radio data over a wire is already solved.** `PanadapterStream`
   decodes VITA-49 (`PCC 0x8003 → FFT bins → spectrumReady()`;
   see [`docs/architecture/vita49-format.md`](architecture/vita49-format.md)),
   and `SmartLinkClient`/`WanConnection`/`TgxlConnection`/`OpusCodec` already
   relay spectrum + audio across a network.
4. **The models are the state.** 29 `QObject` models in `src/models/` with
   change signals — they *are* the thing to serialize.

The **missing fifth piece** is the bulk of the work: the GUI currently reaches
into the engine through **140 distinct `core/`/`models/` headers** (measured
2026-07-04; regenerate with `tools/gen_touchpoint_manifest.py`, which emits the
burndown manifest at `docs/architecture/aetherd-touchpoints.md`). Each of
those touchpoints must become an enumerated protocol message. Cataloguing that
surface *is* the project.

That catalogue should be made once, not twice: as each touchpoint is
enumerated, tag it **universal** (frequency, mode, filter, meters, pan bins —
things every radio has) or **vendor-specific** (Multi-Flex handles, ATU,
amplifier control, GUIClientID session restore). The universal set becomes the
protocol's **core profile**; the rest become namespaced vendor extensions
(§4.1, §5.5). The same enumeration that defines what clients may call downward
defines what a radio backend must provide upward — one sweep, two contracts.

The first full tagging pass ran 2026-07-04 (results in the manifest's tags
sidecar): of 140 headers, **53 universal, 16 mixed** (core-profile protocol
surface: 69), **26 vendor** (22 flex, 4 kiwi — encapsulate behind the §5.5
seam, no client protocol needed), and **45 ui-support** (settings, theming,
device/OS plumbing, control surfaces — need engine-vs-shell home decisions,
not protocol messages). The protocol-message workload is therefore roughly
half the raw touchpoint count.

> **Update (2026-07):** reclassifications since the first pass narrowed the
> `vendor` set to **21** (17 flex, 4 kiwi). Five headers were found to be
> not-radio-wire and moved out: the 4O3A Tgxl/Pgxl/AntennaGenius transports →
> `peripheral(4o3a)` (a new tag for standalone accessory devices, not behind the
> radio seam, now **3**), the FlexControl USB knob → `ui-support`, and
> `TunerModel` → `mixed(flex)` (so `mixed` is now **17**).
> `aetherd-touchpoint-tags.json` is the source of truth.

---

## 3. Proposed architecture

```
  radio hardware ──vendor wire protocols──►  radio backends (IRadioBackend)
   (FlexRadio via VITA-49 / SmartLink;            │
    other families via their own           normalized models
    protocols, added over time)            + frames (§4.2 format)
                                                  ▼
                                     aetherd core  (canonical models,
                                     AudioEngine/DSP, TX guard + arbitration)
                                                  │
                                  ┌───────────────┼────────────────┐
                             control plane     data plane      (both versioned)
                              JSON-RPC        binary frames
                                  │               │
                         ┌────────┴──────┬────────┴────────┐
                     Qt desktop UI    web UI (browser)   TUI / scripts
```

`aetherd` is a long-lived process linking a new **`libaethercore`** (all of
`core/`+`models/`, extracted from the monolith). It owns the radio
connection(s) and every engine-side object. Inside the library, everything
radio-facing sits behind an **`IRadioBackend`** seam (§5.5); the engine core,
the protocol, and every client see only normalized models and frames, never a
vendor wire protocol. UI clients hold **no** engine state; they render a
projection of it and send intents.

**Non-goals (explicit):**
- Not a SmartSDR-protocol reimplementation; this is AetherSDR's *own* boundary.
- Not a rewrite of the desktop UI as a precondition — the existing QWidget UI
  becomes the *first* thin client, migrated incrementally.
- Not a replacement for SmartLink (radio↔engine). `aetherd` is engine↔UI; the
  two compose (§8).
- Not a lowest-common-denominator radio abstraction: vendor-specific features
  surface as namespaced extensions via the capability descriptor (§4.1, §5.5),
  never silently dropped.

---

## 4. The two channels

The control plane is ~80% built; the data plane is the real engineering.

### 4.1 Control / state plane (easy — extend `AutomationServer`)

Promote the existing JSON command channel into a versioned API with three
message kinds:

- **request/response** — extend today's `cmd` set, add a correlation `id`.
- **subscriptions** — a client subscribes to model changes; the engine pushes
  deltas (`event` frames). This is the genuinely new part: today the bridge is
  poll/command, but a live UI needs `propertyChanged` pushed to it.
- **snapshot-on-connect** — a late-joining client receives full state, then
  deltas, so reconnection and multi-client are consistent.

Version handshake on connect (`hello`/`welcome` with a protocol `version`), so
old clients fail closed against a newer engine rather than misbehaving.
`welcome` also carries a **capability descriptor** per connected radio session:
receiver/slice count, sample-rate range, TX capability and power range,
tuner/amplifier presence, the **tunable band set** (canonical, region-neutral
band identifiers — the descriptor is the single source of band truth for
*every* band surface: menus, keyboard shortcuts, controller band-cycling), and
the vendor-extension namespaces the backend implements (§5.5). Clients render
against what the radio *reports*, not against hard-coded assumptions — a
control the radio doesn't have is disabled or absent, and an unknown extension
namespace is safely ignorable. Capability state is session-scoped by
construction: the descriptor arrives in `welcome` and dies with the session,
so one radio's capabilities can never leak into the next connection.

(The band-set field, and the session-scoping and all-surfaces requirements
around it, were surfaced by PR #4027 — @nigelfenton's `bands=` discovery-key
proposal for gateway-bridged non-Flex rigs. That PR is the demand evidence for
this descriptor; its review findings — stale declarations across sessions,
only one of four band surfaces gated, silent parse drops, US-only band names —
are requirements this design must satisfy.)

Sketch:
```jsonc
// client → engine
{ "id": 42, "cmd": "subscribe", "model": "slice", "selector": "0",
  "properties": ["frequency", "mode", "filterLow", "filterHigh"] }
// engine → client (snapshot, then deltas)
{ "id": 42, "ok": true, "snapshot": { "frequency": 14074000, "mode": "DIGU", … } }
{ "event": "changed", "model": "slice", "selector": "0",
  "changes": { "frequency": 14074500 } }
```
Transport: `QLocalServer` for local clients (already used), **WebSocket** for
remote/browser (project already links `Qt6::WebSockets`). Same message schema
on both.

**Scope note — `AutomationServer` splits; it does not move wholesale.** Only
its *model-facing* half is promoted into the engine: the `get`-by-selector
machinery, selector resolution, and the TX-guard policy (the seed of §6). Its
*widget-facing* half — `dumpTree`/`grab`/`invoke` and the connection-flow
facade — is inherently QtWidgets and stays with the Qt client as a client-side
test bridge, which keeps its full value after the split (the thin client's own
rendering/behavior still needs agent-drivable verification). Splitting along
this internal seam is also what retires the class's tracked entries in the
engine-boundary checker's legacy lists.

### 4.2 Data plane (hard — new work)

`spectrumReady()` FFT frames (up to ~30fps over a wide bin array), waterfall
history, audio, and meters cannot ride JSON. Two routes, both wanted:

- **Local: shared-memory ring buffer.** Engine writes FFT/audio frames; clients
  `mmap` and read latest-frame-wins. Zero-copy, lowest latency. The control
  channel carries only a handle + format descriptor.
- **Remote: compact binary frames** over the socket/WebSocket — reuse VITA-style
  framing for FFT and `OpusCodec` for audio (already in tree). Accepts
  compression cost + latency.

**Backpressure is mandatory** (this is the failure class behind #3811's
SO_RCVBUF/adaptive-throttle work): per-client bounded queues, latest-frame-wins
for spectrum/waterfall, so a slow UI can never stall the engine or the radio
drain.

---

## 5. Multi-client semantics

FlexRadio is itself multi-client (multiple slices/pans). `aetherd` adopts
**per-client projection over shared radio state**: there is one authoritative
engine state (the radio's slices/pans/meters), and each connected UI chooses
which slices/pans it renders and how. Clients do not get private engine state;
they get private *views*. Two UIs watching slice 0 see identical data; either
can request a change, and the resulting `changed` event fans out to both.

**Sessions.** The engine already aggregates per-radio state in `RadioSession`
(model + TCI + CAT, #3445). The protocol namespaces every subscription and verb
by session id, so one daemon can host several radios — of the same or different
families — concurrently, and a client chooses which sessions it projects. The
per-client-projection rule above applies unchanged within each session.

Open sub-question for sign-off: whether a client may *create/destroy* pans/slices
(engine-global, affects all clients) or only attach to existing ones. Default
recommendation: creation allowed but treated as a shared mutation, announced to
all clients.

---

## 5.5 Pluggable radio backends (`IRadioBackend`)

The engine↔UI boundary is model-shaped, not vendor-shaped — nothing in §4's
protocol names a wire format. The same discipline is applied on the radio side:
inside `libaethercore`, everything that touches a vendor protocol sits behind
one interface. An `IRadioBackend` implementation owns, per radio family:

- **Discovery** — however that family announces itself (broadcast, directory,
  manual endpoint), normalized to one discovered-radio record for the picker.
- **Connection lifecycle** — connect/teardown/reconnect against the vendor
  transport.
- **Intents down** — the engine's canonical verbs (tune, mode, filter, gain,
  key/unkey, …) translated to vendor commands.
- **State and streams up** — vendor status normalized into the canonical
  models, and spectrum/waterfall/audio/meter data delivered in the §4.2 frame
  format, whatever the radio actually sends.

**Where DSP runs is a backend property, invisible above the seam.** Some radio
families demodulate and compute FFTs on the hardware — that backend is mostly
a protocol decoder. Others deliver raw IQ and expect the client to do the work
— that backend owns an engine-side DSP chain (demodulation, AGC, S-meter,
panadapter FFT; the building blocks — liquid-dsp, FFTW, the `WfmDsp` pattern —
are already in the tree). Either way the models and frames above the interface
are identical, so no client can tell the difference. This is also why the
backend seam belongs in the *engine*: DSP-heavy backends make the §8 topology
(engine at the shack, thin clients remote) the natural deployment for radio
families that have no native remote transport of their own.

**The SmartSDR stack becomes the first backend.** `FlexBackend` wraps the
existing `RadioConnection` + `PanadapterStream` + `RadioDiscovery` classes with
**zero behavior change** — a pure seam-extraction refactor, verified
byte-identical on the slice-0 RX flow. The KiwiSDR path and its side-channels
are a candidate to migrate into a second, receive-only backend once the seam
exists, retiring the per-source injection API.

**Canonical core vs vendor extensions.** Every backend must implement the core
profile (§2's universal set) and may register namespaced extensions, surfaced
through the capability descriptor (§4.1). The core profile versions with the
protocol; extensions version independently per backend.

---

## 6. TX safety across the boundary (Principle VI — non-negotiable)

This is the most important section. A decoupled or scriptable UI is a new path
to the transmitter, so:

1. **The guard lives in the engine, below the protocol.** The
   `kTxKeyingProperty` marker + `AETHER_AUTOMATION_ALLOW_TX` logic that
   `AutomationServer` already enforces must gate *every* TX-keying verb in
   `aetherd`. The client is never trusted to self-police. A protocol request to
   key TX is checked engine-side exactly as a local widget action is today.
   The guard is also **backend-agnostic**: it sits above the `IRadioBackend`
   seam (§5.5), so there is exactly one arbitration/guard path no matter how a
   given radio family is keyed (command verb, in-stream flag bit, hardware
   line). A backend translates the engine's single keying decision to its
   vendor mechanism; no backend implements its own guard.
2. **TX arbitration.** With multiple clients, exactly **one** may hold the TX
   lock at a time; others are read-only for keying or denied. Acquiring/
   releasing the lock is an explicit, audited protocol verb. No client may key
   without holding it.
3. **Per-client authentication.** Remote clients authenticate before any
   control verb; TX capability is a separate grant from RX/observe. A
   read-only/observer client (e.g. a public dashboard) can never key.
   Transport security ships as **WireGuard** (§8): `aetherd` binds its remote
   listeners to localhost and the WireGuard interface only, so off-box
   clients reach the engine exclusively through the tunnel, which supplies
   encryption and cryptographic peer identity. The engine's own auth layer
   then maps a peer to its capability grant — the tunnel authenticates the
   *device*, `aetherd` authorizes the *client*. Browser clients that cannot
   ride a VPN use WebSocket over TLS with the same capability grants; both
   paths fail closed.
4. **Fail closed.** Unknown protocol version, lost lock, dropped auth, or
   ambiguous state → TX denied + force-unkey, mirroring the existing watchdog
   (`forceUnkey()`), including the CWX-buffer abort (#3646). For radio families
   keyed by a continuous TX stream, force-unkey additionally halts that stream,
   so any hardware-side keep-alive watchdog provides a second, independent
   unkey path. Because the guard runs in the engine, fail-closed is **local to
   the radio** in the §8 shack topology: a lost remote client is detected and
   unkeyed engine-side — no unkey command has to survive the dying WAN link.

The acceptance bar: **a malicious or buggy client must not be able to key the
transmitter.** Any design that can't guarantee that is rejected.

---

## 7. The hard problems (honest cost)

1. **Enumerating the surface.** The 140 gui→engine header dependencies each map
   to protocol messages. Mechanical but large; this is the bulk of the effort.
   The non-mechanical part is the §2 tagging pass: deciding, touchpoint by
   touchpoint, what is core profile and what is a vendor extension. The models
   grew up around one vendor's semantics, so canonicalizing them is design
   work, not transcription.
2. **Data-plane latency/throughput.** 30fps wide FFT + low-latency audio. Solved
   locally by shm; remotely it's a compression/latency budget.
3. **State sync & reconnection.** Snapshot-on-connect, delta consistency,
   client reconnect without engine restart.
4. **Lifecycle.** Daemon start/stop, crash recovery, socket discovery, the
   desktop UI auto-spawning a local `aetherd`.
5. **Backpressure** (see §4.2).

---

## 8. Relationship to SmartLink

SmartLink relays **radio↔engine** over WAN. `aetherd` relays **engine↔UI**. They
compose, and the composition unlocks a better topology: run `aetherd` *at the
shack* next to the radio (e.g. on a Pi), keeping the heavy VITA hop on the LAN,
and send only the (cheaper, already-compressed) UI stream over the WAN to thin
clients anywhere. SmartLink remains the radio-discovery/relay layer; `aetherd`
sits above it.

The same topology is *more* valuable for radio families that have no native
remote transport at all — and for DSP-heavy backends (§5.5) whose raw sample
streams are too fat for a WAN: the engine at the shack reduces them to the same
compact frames every other radio sends, so remote operation falls out of the
architecture instead of being re-solved per radio family.

**Why engine-at-the-shack** (the operator-facing case, informing §12 Q4):

- **WAN trouble degrades the picture, not the radio.** The §4.2 backpressure
  rules (per-client bounded queues, latest-frame-wins) apply to a lossy WAN
  exactly as to a slow UI: frames drop and the waterfall thins momentarily,
  but the radio drain and the audio pipeline stay LAN-local and never stall.
  A remote *thick* client puts WAN jitter directly on the timing-critical
  radio streams; this topology structurally cannot.
- **TX fail-closed executes at the radio.** The §6 guard runs in the engine,
  physically at the shack — a dropped or misbehaving WAN client is detected
  and force-unkeyed locally, with no unkey command needing to survive the
  dying link.
- **The session outlives the client.** The engine holds the radio connection
  continuously; snapshot-on-connect (§4.1) means an operator can sleep the
  laptop, switch to a tablet, or ride out a WAN drop and reconnect to a live,
  fully-current session — the radio side never resets.
- **Thin clients are genuinely thin.** All DSP runs engine-side (§5.5), so the
  remote device only renders frames and plays Opus — browser/tablet-class
  hardware suffices, with no per-device DSP setup to keep consistent.

**Remote access ships with WireGuard.** `aetherd` does not invent transport
security: packaging bundles WireGuard (kernel-native on Linux, official apps
on every client OS, `wireguard-go` as fallback) plus provisioning helpers —
engine-side key generation and per-client peer-config/QR export — so
"operate remotely" is a first-run workflow, not a networking project. The
protocol listeners bind only to localhost and the WireGuard interface
(default-deny toward the WAN); §6's per-client capability grants ride inside
the tunnel. WireGuard is GPLv2/MIT (license-compatible) and cross-platform,
consistent with §11.

---

## 9. Alternatives considered

- **Alternative A — QML on `libaethercore` (in-process).** Extract the engine
  library, expose models via `Q_PROPERTY`/`Q_INVOKABLE`, rebuild the UI in QML
  loaded at runtime (hot-reloadable, no C++ recompile to restyle). **Far
  cheaper** because it skips the entire data-plane serialization problem
  (in-process shared memory is free). **Recommended if the goal is desktop
  re-styling only.** Note: models are not QML-ready yet (only 2/29 use
  `Q_PROPERTY`, 0 `Q_INVOKABLE`), so step one is the same property work.
  Backend pluggability (§5.5) is orthogonal: the `IRadioBackend` seam lives in
  `libaethercore` and works identically under this alternative.
- **Alternative B — UI plugin `.so` via `QPluginLoader`.** Swap a compiled UI
  plugin without rebuilding core. Less friendly than QML (plugin is still
  compiled C++), keeps native QWidget. Middle ground; weak "no rebuild" payoff.
- **`aetherd` (this RFC)** is the maximal-decoupling option and the **only** one
  that delivers web/remote/multi-client/headless. Its cost is concentrated in
  the data plane; pay it only if that remote/web/multi-client story is the real
  objective.

**Decision rule:** choose `aetherd` iff the goal includes
web/remote/multi-client/headless — including remote operation of radio families
with no native remote transport (§8). Otherwise choose Alternative A. Pluggable
backends (§5.5) are reachable either way; only the UI boundary differs.

---

## 10. Staged plan

1. **Extract `libaethercore`.** Split the monolith into an engine library +
   thin gui shell. **Rehearsed 2026-07-04** — the split configures, builds,
   and passes all 56 tests on Linux
   ([dry-run findings](architecture/aetherd-step1-dryrun.md)). Verdict:
   ~95% a CMake change, not 100% — one source edit is unavoidable (a
   moc-jumbo-TU incomplete-type bug the monolith masks) plus the
   `AutomationServer`↔`ConnectionPanel` dependency inversion; the
   `HAVE_*`-defines-PUBLIC judgment calls and ranked risks are in the
   findings doc. Speeds nothing by itself (isolation, not raw speed) but is
   the shared prerequisite for *both* this RFC and Alternative A.
2. **Extract the `IRadioBackend` seam** (§5.5): wrap the existing SmartSDR
   stack (`RadioConnection`, `PanadapterStream`, `RadioDiscovery`) in a
   `FlexBackend` with zero behavior change, verified byte-identical on the
   slice-0 RX flow. From this point a new radio family is a new backend —
   shippable in-process in the desktop app immediately, and served through the
   daemon automatically once the later steps land. Additional backends proceed
   in parallel with steps 3–7 without touching them.
3. **Promote `AutomationServer`** into the versioned control protocol:
   `hello`/`welcome` handshake with capability descriptor, `subscribe`,
   snapshot-on-connect, `event` deltas (§4.1).
4. **Engine-side TX arbitration + per-client auth** (§6) — landed *with* step 3,
   never after.
5. **Binary data plane** (§4.2): shm ring buffer for local first, then
   binary-frame-over-WebSocket for remote.
6. **One reference thin client** — port a slice of the existing Qt UI to talk
   protocol instead of direct calls, to prove the surface is complete.
7. **Web UI / additional clients** fall out of the proven protocol.

**Repo layout: monorepo, for the whole migration and beyond.** `aetherd` is
another CMake target (`libaethercore` + a headless executable) alongside the
existing app target, in the pattern the repo already uses for `hal-plugin/`
and `plugins/`. The reasons are structural, not preference:

- Steps 1–6 carve the boundary out of 140 shared touchpoints; nearly every PR
  in that window touches engine and desktop client together. One repo keeps
  each conversion an atomic, revertable commit with one CI run — two repos
  mean paired PRs and protocol-version skew between checkouts.
- The desktop app auto-spawns its local engine, so the two ship and version as
  a unit (one CalVer, one CHANGELOG, one release pipeline).
- Governance is repo-scoped and load-bearing here: the constitution, agent
  guides, signed-commit/CODEOWNERS protection, CI image, and the step-4 rule
  ("TX arbitration lands *with* the protocol, never after") are enforceable by
  review in one repo but become cross-repo coordination promises in two.
- The engine/client separation is enforced the same way core/→gui/ already is:
  by dependency direction (`aetherd` must never link QtWidgets), checked
  cheaply in CI — a directory boundary, not a repo boundary.

**Agent-guide discipline.** AetherSDR's contributors are predominantly AI
agents that read `AGENTS.md` before every change, so each milestone PR above
must update `AGENTS.md` **in the same PR** — the routing rules, ratchets, and
claim/verification recipes for each step are pre-drafted in
[`docs/aetherd-agents-md-staging.md`](aetherd-agents-md-staging.md) and land
atomically with their step. A milestone PR that changes structure without its
`AGENTS.md` block is incomplete.

The natural fracture line for future splits is the **protocol, not the
process**: once protocol v1 is stable, far-side clients with different
toolchains and contributor bases (web UI, third-party client SDKs, a
standalone protocol spec for external authors) may earn their own repos. The
engine, the models, and the Qt desktop client stay together permanently. A
shack-appliance build (the §8 topology on a small ARM box) is packaging work
in `packaging/`, not a repo split.

---

## 11. Constitution compliance

- **Principle VI (TX safety):** the entire guard moves *into* the engine below
  the boundary; arbitration + auth + fail-closed (§6). No client is trusted.
- **Principle V (settings):** protocol/engine config persists as nested JSON
  under a single key; no new flat `AppSettings`.
- **Principle IV (clean-room):** the protocol is AetherSDR's own design, not
  derived from any proprietary binary or wire format.
- **Principles I & IV, per backend (§5.5):** FlexLib remains the sole protocol
  authority for the SmartSDR backend. Any additional backend must name its own
  authoritative, openly documented protocol reference in its design doc and
  implement from that documentation — the clean-room rule applies
  backend-by-backend.
- **Cross-platform (operator feedback):** `QLocalServer`/WebSocket/shared-memory
  are all Qt-portable; no platform-specific surface in the boundary.

---

## 12. Open questions for reviewers

1. Is the real goal remote/web/multi-client (→ `aetherd`) or desktop re-styling
   (→ Alternative A)? This determines everything.
2. Pan/slice creation by clients: shared mutation, or attach-only? (§5)
3. Auth model for remote clients — transport security is decided (WireGuard,
   §6/§8); still open: are capability grants a separate `aetherd` credential
   or a reuse of SmartLink identity, and what credential carries the
   browser/TLS path where a VPN isn't practical?
4. Is a headless-at-the-shack topology (§8) a target use case, or is local-only
   sufficient for v1?
5. Multi-session (several radios under one engine, §5): required for v1, or
   does v1 ship single-session with the namespacing reserved in the protocol?
6. Should the core-profile / vendor-extension split (§2, §5.5) be ratified as
   part of protocol v1, or grown incrementally as the second backend lands?
