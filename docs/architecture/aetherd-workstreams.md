# aetherd — How We Work Together: Flex, HL2, and the Seam

> **Status:** Proposal. Companion to the Principle I rescope in this same PR
> (#4438) and to the AGENTS.md collaboration rules that land with it.
> **Audience:** everyone — human or agent — touching a radio backend or the
> neutral layer between them.

AetherSDR now has more than one radio family in flight at once: the shipping
FlexRadio path and the in-progress Hermes-Lite 2 path, meeting at the aetherd
`IRadioBackend` seam. Two contributors are working these concurrently, and on
2026-07-24 that concurrency produced a near-miss: a shared feature branch
diverged, and one contributor nearly force-pushed away the other's work. This
document exists so that does not happen again — it names the lanes, the
workstreams, how they depend on each other, and the mechanics that keep them
from colliding.

It is deliberately concrete. Where it cites counts (e.g. "25 decode calls"),
those are the measured baseline at the time of writing and are meant to burn
down to zero.

---

## 1. The seam is the organizing principle

`IRadioBackend` (see `aetherd-iradiobackend-design.md`) is the contract. It
divides the codebase into two halves, and ownership follows that line:

```
        ┌─────────────────────────────────────────────┐
        │  NEUTRAL LAYER  (vendor-agnostic)            │
        │  RadioModel · SliceModel · PanadapterModel   │
        │  MainWindow / UI                             │
        └───────────────────────┬─────────────────────┘
                                 │  IRadioBackend  ← the contract
        ┌───────────────────────┴─────────────────────┐
        │  BACKENDS  (vendor-specific)                 │
        │  FlexBackend            Hl2Backend           │
        │  RadioConnection        MetisClient          │
        │  PanadapterStream       Hl2RxDsp / WdspChannel│
        └─────────────────────────────────────────────┘
```

- **Above the seam** is neutral. It must not know which radio it is talking to.
- **Below the seam** is backend-specific and free to be as vendor-shaped as the
  hardware requires.
- **The seam itself** — `IRadioBackend.h`, and the parts of `RadioModel` /
  `SliceModel` that route through it — is shared contract. Changes here affect
  both families and are never made by one lane alone.

This is the structural expression of **Constitution Principle I**: the radio-side
implementation is the protocol authority (FlexLib for Flex, the gateware RTL for
HL2). The seam is where those authorities are translated into neutral intent.

---

## 2. Lanes and ownership

Three lanes. Each has a primary owner; the seam is jointly owned.

| Lane | Scope | Primary owner | Files (typical) |
|---|---|---|---|
| **Flex** | Flex features, protocol, wire objects | Flex lane owner | `FlexBackend*`, `RadioConnection`, `PanadapterStream` |
| **HL2** | HL2 bring-up, DSP, Metis protocol, later TX | HL2 lane owner | `Hl2Backend*`, `MetisClient/Protocol`, `Hl2RxDsp`, `WdspChannel` |
| **Seam** | the contract + neutral-layer decoupling | Seam lane owner | `IRadioBackend.h`, neutral parts of `RadioModel`/`SliceModel`, UI data-plane |

**The contested middle** is `RadioModel`, `SliceModel`, and `IRadioBackend.h`
itself. Both families pass through these. A change here is a *seam-contract*
change (§5) and requires the other lane's review, regardless of who writes it.

Rule of thumb: **if a change is specific to one radio, it belongs below the seam
in that backend. If it is specific to no radio, it belongs above. Nothing
Flex-specific may be added above the seam** — that is exactly the debt §4 pays
down, and re-incurring it is the failure this document exists to prevent.

---

## 3. The three work areas

### 3a. Flex — ongoing
Flex is shipping; feature work continues (DAX, TCI, SmartLink/WAN, amplifier,
tuner, waterfall/FFT rendering). This work lives **below the seam in
`FlexBackend`** or in already-neutral models. New Flex features must not add
Flex-specific code to the neutral layer — where a neutral touchpoint is needed,
add an `IRadioBackend` verb or `invokeExtension` namespace rather than reaching
up.

### 3b. HL2 — ongoing
Receive is working on hardware (discovery, tune, AM/SSB demod with correct
pitch, panadapter/waterfall/slice, audio, AGC — see `HERMES.md`). Ahead: TX,
multi-RX/nddc, further protocol depth (attenuator/preamp, PA), and the
correctness-oracle audits. All of this lives **below the seam in the HL2
backend and its DSP/protocol layer**. Where HL2 needs a control the seam does
not yet expose (notably TX), that is a seam-contract change (§5) — a joint
decision, not an HL2-only one.

### 3c. Seam unification — the extraction backlog
The neutral layer still contains substantial Flex-specific code, invisible while
Flex was the only backend and surfaced the moment HL2 arrived (every
`if (!m_panStream)` guard in `RadioModel` is a marker for a piece of it). The
workstreams in §4 pull that code below the seam so the neutral layer is truly
neutral and a third backend needs only to implement `IRadioBackend`.

---

## 4. The seam-unification workstreams

Baseline debt measured in `RadioModel` + UI at time of writing:

| WS | What it extracts | Baseline | Effort | Risk |
|---|---|---|---|---|
| **WS1** | **Status ingress → backend.** `RadioModel` routes raw Flex status through `m_flexBackend->decode*Status()`; FlexBackend should own status ingress and emit only normalized signals. Deletes the `m_flexBackend` concrete-type alias. | 25 `decode*` calls | M–L | Med |
| **WS2** | **UI data-plane decoupling.** GUI binds directly to the Flex-owned `PanadapterStream`; it should bind only to neutral `RadioModel` signals. (Spectrum/waterfall already relayed via `panFeed`.) | 64 `panStream()` bindings, 7 files | L | High |
| **WS3** | **DAX subsystem behind the seam.** DAX channel/stream management and `stream …` status parsing. | ~15 `m_panStream->` DAX calls | M | Med |
| **WS4** | **Link/network diagnostics as a neutral interface.** `rxBytes`, `packetErrorCount`, `localPort`, `audioPacketGap*`, `hasReceivedPackets` — a neutral `LinkStats` the backend provides. *These are the HL2 null-guard band-aids.* | ~10 stat calls | S | Low |
| **WS5** | **Command verbs behind the seam.** Raw Flex wire commands in `RadioModel` (`mixer`, `profile`, `interlock`, `stream`, `client`, `radio`, `display`, `file`) → `IRadioBackend` verbs / `invokeExtension`. | 14 command sites | S each | Low |
| **WS6** | **Connection lifecycle fully through the seam.** Remaining direct `m_connection->` use (`connectToRadio`, `gracefulDisconnect`, `clientHandle`). Partly done. | 11 `m_connection->` derefs | S | Low |

**Recommended order:** WS4 → WS1 → WS5 → WS3 → WS2 → WS6. WS4 is the smallest,
is HL2-motivated, and removes the band-aid guards; WS1 is the keystone that
deletes the alias. Together WS4+WS1 remove most of the neutral-layer coupling.

**Definition of "unified":** zero `m_flexBackend` references and zero direct
`panStream()`/`m_connection->` uses in the neutral layer; a new backend compiles
and runs by implementing `IRadioBackend` alone.

---

## 5. How the workstreams account for each other

The lanes are not independent; they meet at the contract. These are the
cross-dependencies to hold in mind.

- **Adding an `IRadioBackend` verb obligates every backend.** When a workstream
  or feature adds a seam method, **all backends implement it** — Flex for real,
  HL2 for real or as an explicit, capability-gated no-op. A verb only one
  backend implements is a leak waiting to happen. Optional capabilities gate
  it: e.g. HL2 TX guards on `capabilities().canTransmit`.

- **WS1 (status ingress) touches the decode path HL2 already bypasses.** HL2
  emits normalized signals directly; Flex routes through `decode*`. Moving Flex
  status ingress into `FlexBackend` must not change the normalized signals HL2
  already produces — the neutral layer must not be able to tell which backend
  fed it. Coordinate with the HL2 lane before landing, because the two share
  the signal definitions.

- **WS2 (UI decoupling) is shared by both families.** Both HL2 and Flex render
  through the panadapter/waterfall path. Any change to how the UI receives
  frames must preserve both — verified on Flex hardware *and* HL2 hardware, not
  one.

- **HL2 TX (HL2 lane) needs new seam surface.** TX verbs, keying, and a power
  model do not exist on `IRadioBackend` yet. When the HL2 lane reaches TX, the
  verbs are designed jointly with the seam lane so Flex TX can adopt the same
  surface rather than a parallel one. Until then, `setKeying` stays a
  capability-gated no-op and TX canon (never key TX; TX behind
  `AETHER_AUTOMATION_ALLOW_TX`) is inviolate.

- **New Flex features must not widen the debt.** While the seam lane is paying
  down §4, the Flex lane must not add fresh Flex-specific code above the seam.
  A neutral touchpoint gets a seam verb; it does not reach up into `RadioModel`.

- **The `panFeed` relay and `backendRebuilt` re-wire are shared infrastructure.**
  Anything binding to a backend-owned wire object must re-establish on a backend
  swap (`MainWindow::rewirePanStreamAfterBackendSwap`). New bindings account for
  this or they die silently on a family switch.

---

## 6. Seam-contract change rules

`IRadioBackend.h` and the neutral routing in `RadioModel`/`SliceModel` are the
contract. Changing them:

1. **Requires the other lane's review.** Not optional, regardless of author.
2. **Adds a verb → both backends implement it** in the same change (real or
   capability-gated).
3. **Changes/removes a signal → both consumers updated** in the same change.
4. **Optional behavior is gated by `capabilities()`**, never assumed. RX-only
   backends exist; `canTransmit` is real.
5. **No Flex-specific code lands above the seam.** New Flex behavior goes in
   `FlexBackend` behind a verb or `invokeExtension`.

---

## 7. Collision-avoidance mechanics

The process half. These are the concrete git rules; the rationale is the
2026-07-24 near-miss.

- **Integration branch + per-lane sub-branches.** The feature integration
  branch is the merge target; each lane works a sub-branch and opens a PR *into*
  it (`gh pr create --base <integration> --head <lane-branch>`). Nothing lands
  on the shared branch by direct push during concurrent work.
- **Pull before you work; never force-push a shared branch.** A shared branch is
  never rewound. If a force-push seems necessary, stop — it means histories
  diverged and the fix is a merge, not a rewrite.
- **Unrecognized commits → stop and ask.** If `git fetch` shows commits on a
  shared branch you did not make, do not reconcile unilaterally. That is exactly
  the moment the near-miss happened.
- **One owner per `main` merge.** Whoever is least mid-surgery merges `main`
  into the integration branch; the others pull the result.
- **Green CI is a merge gate, and that includes `check-windows`.** The HL2
  code's first Windows build failed on an MSVC-only `min/max` macro clash that
  no Linux/macOS run could catch. Platform-specific CI is the authority for
  platform portability the same way the gateware is for protocol.
- **Worktree-first for stateful work** (per the operator's local rules): a fresh
  worktree on a fresh branch, never editing/building in a shared checkout.

---

## 8. What lands where (this PR family)

- **Constitution Principle I rescope** — the radio-side implementation is the
  protocol authority (this PR, #4438).
- **This document** — the lanes, workstreams, and mechanics (this PR).
- **AGENTS.md collaboration rules** — the operational rules an agent reads
  before touching a backend or the seam (to be discussed; lands with this PR).

Together they are the governance + process baseline for multi-family,
multi-contributor work on AetherSDR.
