# Agent guide — radio backends, aetherd, and the engine boundary

Part of [`AGENTS.md`](../../AGENTS.md). Read this before changing anything in
`src/core/backends/`, `RadioModel`, `RadioSession`, `TransmitModel`,
`ConnectionPanel`, discovery, `RadioCapabilities`, `src/aetherd/`,
`src/core/control/`, or any file that includes a vendor wire header — and
before adding a `#include` that crosses `gui/` ↔ `core/`/`models/`.

## Standing notice — #5554

The 2026-09-10 backend architecture review is tracked in **#5554** (meta:
every open seam/multi-radio issue plus the new findings). Before you submit,
check that your change does not add to any item listed there — no new
`usesFlexCommandPlane()` / family-string branches, no new raw Flex wire text
above the seam, no new `dynamic_cast` to a concrete backend, no new
capability declared without a verb behind it, no copy of HL2 scaffolding into
another host-DSP family, and no new keying-class verb that skips the TX gate
in `RadioModel`. If your PR resolves one of those items, link it. The notice
in `AGENTS.md` is removed when #5554's §2 items each have their own issue and
#5262 M1 has landed.

## In-flight: aetherd engine/UI decoupling

The accepted RFC
[`docs/aetherd-headless-engine-design.md`](../aetherd-headless-engine-design.md)
(tracking issue #3849) splits this codebase into an engine library
(`libaethercore`), a headless engine daemon (`aetherd`), and thin UI clients,
with pluggable radio backends (`IRadioBackend`). Implementation follows the
RFC's §10 staged order. Step 1 (`libaethercore`) and the step-2 seam have
landed; steps 3 and 4 are in progress — the current implementation status is
the RFC's appendix "Implementation status", and the per-stage contracts are
the `docs/aetherd-*.md` documents it links.

`IRadioBackend` (`src/core/backends/`) has **six** implementors, selected at
connect time by a `family` string through `makeBackend()`. The seam's known
gaps and the multi-radio migration order are tracked in #5262 (M0–M6) and the
review meta-issue #5554 — read both before changing anything in this table's
territory.

**Every implementor honours the THREADING AND LIFETIME CONTRACT at the top of
`IRadioBackend.h`** (backend lives on its owner's thread, every seam signal is
emitted from it, workers are private, payloads declared and registered in one
place, teardown bounded and ordered). What is pinned versus surveyed:
`backend_seam_affinity_test` pins rules 1, 2 and 6 for the simulator and
rule 1 plus a cold construct/teardown for every other family;
`hl2_connect_reentrancy_test` pins rule 2 for HL2 while its DSP build runs on
the I/O thread; `backend_family_switch_test` pins rule 5 across the
production switch with a deterministic stale-delivery injection. Live-emission
affinity for flex, anan, icom and rtl is a survey result until
`tests/SeamThreadAffinityProbe.h` is carried by a test that drives one of them.

**The probe table is GENERATED. Do not hand-edit it.** Adding a signal to
`IRadioBackend.h` means running `python tools/gen_seam_probe_table.py` and
committing `tests/SeamSignalProbeTable.inc`; `Static checks` runs the same
tool's `--check` on every PR and fails naming the signal you missed.

| Family | Backend | Notes |
|---|---|---|
| `flex` | `FlexBackend` (`src/core/backends/flex/`) | SmartSDR wire stack; the Panadapter / Slice / Meter / Transmit / Amp / Tuner status+command paths decode behind it (2.2b–2.4) |
| `hl2` | `Hl2Backend` (`src/core/backends/hl2/`) | Hermes-Lite 2, shipped v26.7.4 — Metis/HPSDR transport, raw-IQ RX/TX DSP done in-client |
| `icom` | `IcomCivBackend` (`src/core/backends/icom/`) | Networked Icom, shipped v26.8.2 — CI-V command plane inside the RS-BA1 UDP transport; the radio owns its own state, so `clientSettingsDomains` is empty |
| `sim` | `SimBackend` (`src/core/backends/sim/`) | Synthetic demo backend, shipped v26.7.4 — generates its own audio + spectrum, RX-only by construction (Principle VI) |
| `anan` | `AnanBackend` (`src/core/backends/anan/`) | ANAN-G2 receive support over openHPSDR Ethernet Protocol 2; raw-IQ RX DSP runs in-client and TX remains absent by construction |
| `rtl` | `RtlSdrBackend` (`src/core/backends/rtl/`) | RTL-SDR USB receive backend; raw-IQ RX DSP runs in-client and the backend is RX-only by construction |

### Rules for step 3 (aetherd control plane)

- New resource fields belong in `RadioResourceAdapter` and the versioned
  catalogue, never in a transport or via QObject reflection.
- No protocol TX method is advertised before the step-4 arbiter exists.
- The desktop adapter has not landed; UI code still consumes models directly,
  and that remains correct.
- The Flex PTT hardware evidence covers FLEX-6700 firmware 4.2.18.41174; do
  not claim other models were tested
  ([`docs/aetherd-flex-ptt-stop-evidence.md`](../aetherd-flex-ptt-stop-evidence.md)).

### Rules for step 4 (`TxCoordinator`)

Contract: [`docs/aetherd-stage4-tx-coordinator.md`](../aetherd-stage4-tx-coordinator.md).

- A queue-consumed callback ends local handoff only; it never proves the radio
  idle. Preserve normal operator reengagement, but use `finishLocalIntent()`
  rather than asserting a qualified stop: the coordinator retains that actor
  until matching stop evidence arrives. Uncorrelated RX status must not clear
  this handoff barrier.
- Preserve short key-down/key-up sequences, Quindar/RADE release tails, and
  held MOX when cancelling a CWX batch.
- Do not enable independent-client handoff or daemon TX until independent
  trusted grants and the qualified stop/recovery contract are complete.
- The bridge watchdog's authorization deadline is monotonic and
  non-renewable: repeated commands must not renew it, and a boolean keyed
  sample alone cannot establish ownership (CWX has QSK gaps). Deferred TX
  widget invocations retain the original input before queueing and claim only
  after admission.
- Local producers hold opaque `TxCoordinator::Intent` handles, not activity
  bits. Repeat admission reuses a producer's live handle. Mark release before
  callbacks/queueing, retain the captured handle until its normal tail is
  consumed, and end that handle only. Reengagement gets a distinct handle.
  Capture before the first queued hop; derive scheduled elements from the
  original root, never from callback-time authority. Device close,
  authorization changes and reconnect fence stale work. Keep TX audio context
  through backend queues and retries. Producer identity does not confer an
  independent actor grant.

## In-process audio and spectrum — one route each

`IRadioBackend::audioFrameReady` has two possible routes to
`AudioEngine::feedPcmFrame` — the `RadioModel::backendAudioFrameReady` relay,
and a direct connect in `wireBackendSeam()`. `FlexBackend` never emits
`audioFrameReady` (audio rides `PanadapterStream`/VITA-49), so reasoning that
holds for Flex does not hold for an in-process backend.

**The two gates have opposite senses and are deliberately named apart — do not
merge them.** The relay's gate is `MainWindow::backendFeedsEngineDirectly()`
(`dynamic_cast<SimBackend*>`, `MainWindow_Session.cpp`): sim feeds the engine
itself, so the relay returns early for sim and for nobody else. The direct
connect in `wireBackendSeam()` is sim-only for the same reason, and *that* is
the site whose gate belongs on "does this backend own its RX audio"
(`MainWindow.cpp`). An HL2 is `ownsRxAudio() == true` **and** needs the relay,
so delegating the relay to `backend()->ownsRxAudio()` silences HL2.
`Qt::UniqueConnection` does **not** protect either site (two different
signals at the relay; a lambda connect at `wireBackendSeam()`).

Spectrum has one producer and one path: `spectrumFrameReady` is consumed by
`RadioModel::onBackendSpectrumFrame` and re-emitted on the neutral `panFeed`
path every backend renders. Do not draw it a second time at the seam.

## Build targets

| Target | Contents | May link |
|---|---|---|
| `libaethercore` (`aethercore`) | `src/core/` + `src/models/` — the engine | Qt Core/Gui/Network/Multimedia/WebSockets/SerialPort/DBus, the DSP + third-party libs. Qt Gui remains because `BandPlanManager` and `DxccColorProvider` expose `QColor`; removing it is a burndown target. **Never `gui/` or QtWidgets**; the remaining EB2 warnings are source-location debt compiled only by the desktop target |
| `AetherSDR` | `src/gui/` + `main.cpp` — the desktop app | `aethercore` + Qt Widgets + qgeoview + QRhi private |
| `aetherd` | `src/aetherd/main.cpp` — headless service shell | `aethercore` + direct Qt Core/Network links; inherits the engine's runtime surface (Qt Concurrent, Gui, Multimedia, SerialPort, WebSockets, DBus, qtkeychain when enabled). Narrowing those edges is a burndown target. **Never QtWidgets** |

## CI-enforced ratchets

All run in the required `Static checks` job. Findings against a TRACKED
baseline warn; a new violation or a grown baseline errors.

- **EB1** (`tools/check_engine_boundary.py`) — no `core/`/`models/` file may
  include a `gui/` header.
- **EB2** — no `core/`/`models/` file may use QtWidgets (a shrinking
  tracked-legacy set warns, new usage errors). EB2 is a per-file **count**, so
  a lateral swap inside a tracked file passes flat.
- **EB3** — no file **above the radio seam** (all of `src/gui/`, `src/core/`,
  `src/models/`, plus `src/main.cpp` and `src/MacStartupAbortGuard.{h,cpp}`,
  **except** `src/core/backends/`) may include a **vendor header** — a
  radio-family wire class tagged `vendor(...)` in
  `docs/architecture/aetherd-touchpoint-tags.json`. Accessory transports are
  `peripheral(...)`, USB input surfaces `ui-support`, and generic models fused
  with vendor relay (e.g. `TunerModel`) `mixed(flex)`; none are EB3-gated.
- **Capability-boolean freeze** (#5262 M2, `tools/check_capability_records.py`).
  `RadioCapabilities`' boolean population may only shrink. **A new capability
  lands as a per-feature record** — `std::optional<FeatureRecord>`, engaged =
  present, fields = shape (the `cwText*` pattern) — because a bool cannot
  carry shape and an unset bool reads as a considered "no". Converting one to
  a record lowers `FROZEN_BOOL_COUNT` in the same commit.
- **Command-plane freeze** (#5262 M4, `tools/check_command_plane.py`). Raw Flex
  wire text above the seam is frozen per file and may only shrink; **a file
  not already in the baseline must stay at zero.** On HL2/Icom/ANAN/RTL that
  text is silently dropped, so a control written that way looks live and does
  nothing. New controls use a typed intent through `IRadioBackend`. Growth
  under `src/core/backends/flex/` is not counted.

`SliceModel` and `TransmitModel` no longer encode Flex commands. Their remaining
desktop setters emit validated `SliceControlRequest` / `TransmitControlRequest`
values through the identity-guarded `RadioModel` route. `FlexModelControls.cpp`
owns the encoding; HL2 and Icom adapt only implemented controls and return
explicit dispatch, local-only, or unsupported receipts. RX-only backends inherit
local active-slice selection and refuse TX controls. These are desktop intents,
not new daemon methods or TX permissions.
MOX, TUNE and ATU start/stop still use the existing operation-fenced TX path;
host TUNE-power edits additionally require a live locally admitted carrier.
HL2 host-keyer, sidetone and compressor settings return local-only receipts;
their existing local consumers still apply them (#6153). Icom CW controls retain
the current keyer capability gate; its replacement belongs to #6110.

If your change trips any of these, restructure the change — do not move the
file, weaken the check, or add an exemption. Engine code that needs a UI
callback defines a gui-free interface in `core/` (e.g. `IConnectionAutomation`)
that the gui implements.

### Engine boundary ratchet — EB3 (vendor includes)

- **Relocation does not convert a touchpoint.** The five Flex wire classes
  (`RadioConnection`, `PanadapterStream`, `SmartLinkClient`, `WanConnection`,
  `CommandParser`) live under `src/core/backends/flex/`; existing callers use
  those explicit paths and remain tracked. No forwarding headers or new
  include-directory shortcuts. Demo-only compatibility data lives in
  `core/backends/DemoRadioConstants.h`.
- **The rule.** Each tracked file's baseline row
  (`KNOWN_VENDOR_INCLUDE_BASELINE`, top of `tools/check_engine_boundary.py`) is
  the exact **set** of vendor headers it may include. A vendor `#include` in an
  untracked file, or a header not in a tracked file's set — *including a
  lateral swap that keeps the count flat* — fails. The vendor vocabulary is
  derived at runtime from `aetherd-touchpoint-tags.json`, so tagging a new
  header `vendor(...)` arms enforcement with no checker edit.
- **De-classification is pinned.** `VENDOR_STEMS_PINNED` freezes today's vendor
  stems: a stem the audit no longer tags `vendor(...)` is a blocking `EB3-load`
  error. The only permitted rebaseline is an intentional
  vocabulary-classification change: every newly tracked include proven to
  predate it against the merge base, the evidence documented, and explicit
  maintainer review; the same evidence releases a stem from
  `VENDOR_STEMS_PINNED` in that commit. Shrink-only afterwards.
- **Adding a radio feature?** Put the wire code in the family backend
  (`src/core/backends/<family>/`) and surface it through `IRadioBackend` (a
  canonical verb/signal, or the namespaced `invokeExtension`/`extensionStatus`
  channel), then consume *that* from the model/UI.
- **Removing coupling (the goal).** When you drop a vendor include, remove that
  stem from the file's row (delete the row when it empties). Never add a stem
  or row to make a build pass; if EB3 blocks you and the include is genuinely
  unavoidable, that is a design conversation for a maintainer.
- `src/gui/**` is in the `static-checks.yml` trigger, so gui-only PRs are
  checked too.

## Where radio-facing code goes

| Your change | Goes |
|---|---|
| Code speaking a vendor wire protocol (commands, discovery, stream parsing) | that family's backend under `src/core/backends/<family>/`, behind `IRadioBackend` — never in `gui/`, and increasingly not in the models |
| A new radio family | a new `IRadioBackend` implementation under `src/core/backends/<family>/` — requires an approved design doc naming its open protocol authority (Constitution Principles I & IV apply per backend) |
| A new engine feature | `libaethercore`, exposed through models — never via a new gui→core header |

Do **not** reroute existing model↔wire code through `FlexBackend` wholesale —
the per-touchpoint conversion is staged work
(`docs/architecture/aetherd-touchpoints.md`). The five `mixed` models
(Radio/Slice/Transmit/Panadapter/Meter) and the amp/tuner models decode
SmartSDR status in `FlexBackend` behind typed deltas; their remaining
model/UI consumers are converted subsystem-by-subsystem. Converting a
touchpoint follows the claim protocol + a before/after
`tools/verify_slice0_rx.py` run; a converted file drops its vendor include and
lowers its EB3 baseline.

Do not pre-emptively restructure code toward the RFC — no new engine/UI
seams, backend interfaces, or speculative library targets ahead of a landed
step. Each migration step lands together with an update to this file stating
its rules (pre-drafted in
[`docs/aetherd-agents-md-staging.md`](../aetherd-agents-md-staging.md)).
Architecture changes ahead of the RFC steps remain maintainer-only.

## Before you merge — the aetherd conformance checklist

Every item below has a green CI run behind it, so a passing `Static checks`
answers none of them. Whoever lands a PR in this territory — agent or human,
author or reviewer — walks the list:

1. **Run the four gates on the merge base *and* on the head, and diff the
   findings per file** — not the exit codes: `check_engine_boundary.py
   --strict`, `gen_touchpoint_manifest.py --check`,
   `check_capability_records.py --strict`, `check_command_plane.py --strict`
   (stdlib Python, seconds each). Tracked-baseline findings only warn.
2. **A new `gui/`→engine include stops at "regenerate", not at "justify".**
   Regenerating the touchpoint manifest clears the red check without flagging
   the new touchpoint. Diff the manifest between merge base and head; a new
   row, or a row whose includer count went up, needs a justification in the
   PR body, or it is a finding.
3. **Regenerate the manifest; never hand-edit it.** Run
   `python tools/gen_touchpoint_manifest.py` and commit the result.
4. **A baseline edit is never how a check goes green — but a reduction is
   required maintenance, not a finding.** Dropping a stem whose coupling the
   PR removed, lowering `FROZEN_BOOL_COUNT` when a bool became a record, and
   lowering a converted file's command-plane count are all conforming. Growing
   a baseline, adding a stem or row, or retagging a `vendor(...)` header is
   forbidden; the two maintainer carveouts (EB3 reclassification with
   merge-base proof; a `FROZEN_BOOL_COUNT` raise on a maintainer ruling) are
   rulings carrying that evidence, never a route to a passing check.
5. **Touching a backend?** Read the THREADING AND LIFETIME CONTRACT in
   `IRadioBackend.h` against the diff, and re-check the #5554 notice above. A
   survey result means reading is the check, because no test will fail.
