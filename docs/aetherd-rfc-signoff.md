# aetherd RFC — maintainer sign-off packet

Companion to [`aetherd-headless-engine-design.md`](aetherd-headless-engine-design.md).
Every open decision from the RFC's §0 table and §12 questions, one entry
each: recommendation, why, and what changes if vetoed. Check a box (or
strike it and note the override) — when all entries are resolved, flip
the RFC's **Status** line from Draft to Accepted and implementation can
begin per §10.

Bias inherited from the RFC: successful, consistent operation for every
operator skill level; TX safety is never weakened by the split.

---

## D1 — Goal scope (§1, §12 Q1)

**Recommendation: `aetherd` (the daemon split), not Alternative A (QML
restyling).**
The goals on the table — remote operation, thin/browser clients,
multi-client, headless shack box, and pluggable radio families including
DSP-heavy ones with no native remote transport — are exactly the set the
RFC's own decision rule says only `aetherd` delivers. Alternative A
solves only desktop restyling.
*If vetoed:* stop after `libaethercore` + `IRadioBackend` (steps 1–2,
which Alternative A needs anyway) and reassess.

- [x] Approved — 2026-07-04, KK7GWY (all recommendations as written)

## D2 — Process model (§3)

**Recommendation: one long-lived `aetherd` daemon; all UIs are peer
clients.** The desktop app auto-spawns a local engine, so ordinary users
never see the daemon.
*If vetoed:* the RFC collapses to Alternative A/B — see D1.

- [x] Approved — 2026-07-04, KK7GWY (all recommendations as written)

## D3 — Local transport (§4.1)

**Recommendation: `QLocalServer`** — already proven by the automation
bridge, portable, zero new dependencies.
*If vetoed:* localhost TCP/WebSocket works but gives up filesystem-
permission access control.

- [x] Approved — 2026-07-04, KK7GWY (all recommendations as written)

## D4 — Remote transport (§4.1)

**Recommendation: WebSocket** — already linked (`Qt6::WebSockets`),
browser-compatible, one schema shared with the local path.
*If vetoed:* raw TCP framing loses the browser story.

- [x] Approved — 2026-07-04, KK7GWY (all recommendations as written)

## D5 — Remote security (§6, §8)

**Recommendation: ship with WireGuard.** Remote listeners bind to
localhost + the WireGuard interface only (default-deny toward the WAN);
packaging bundles provisioning helpers (keygen, per-client peer-config/QR
export). The tunnel authenticates the device; `aetherd` capability grants
authorize the client. Browser/TLS path is an explicit opt-in for the
no-VPN case.
*Consequence to be aware of:* out of the box there is NO internet-exposed
`aetherd` port at all.
*If vetoed:* TLS + credential auth on the open WebSocket becomes the
primary surface — more exposed, more to audit.

- [x] Approved — 2026-07-04, KK7GWY (all recommendations as written)

## D6 — Data plane, local (§4.2)

**Recommendation: shared-memory ring buffer**, latest-frame-wins;
zero-copy FFT/waterfall/audio to local clients. Precedent in-tree:
the DAX HAL plugin's shm ring.
*If vetoed:* frames over the local socket — simpler, measurable CPU cost
at 60 fps panadapter rates.

- [x] Approved — 2026-07-04, KK7GWY (all recommendations as written)

## D7 — Data plane, remote (§4.2)

**Recommendation: binary frames over the WebSocket + Opus audio**,
reusing the in-tree VITA-style framing and `OpusCodec`. Mandatory
per-client backpressure, latest-frame-wins.
*If vetoed:* no realistic alternative at 30 fps spectrum over WAN; this
one is effectively load-bearing.

- [x] Approved — 2026-07-04, KK7GWY (all recommendations as written)

## D8 — TX arbitration (§6)

**Recommendation: single-holder TX lock + per-client auth, enforced in
the engine, backend-agnostic, fail-closed (force-unkey local to the
radio).** Acceptance bar: a malicious or buggy client cannot key the
transmitter. Lands *with* the protocol (step 4), never after.
*If vetoed:* no acceptable alternative exists; a veto here should reject
the RFC instead.

- [x] Approved — 2026-07-04, KK7GWY (all recommendations as written)

## D9 — Multi-client view semantics (§5)

**Recommendation: per-client projection over shared radio state.**
Two UIs watching slice 0 see identical truth; either may mutate; changes
fan out to all.
*If vetoed:* per-client private state forks reality between screens —
recommend against.

- [x] Approved — 2026-07-04, KK7GWY (all recommendations as written)

## D10 — Pan/slice creation by clients (§5, §12 Q2)

**Recommendation: creation allowed, treated as a shared mutation
announced to all clients** (matches how multiple SmartSDR clients already
behave against one radio). Observer-grade clients (D5/D8 grants) cannot
create/destroy.
*If vetoed (attach-only):* simpler, but a remote client could never open
a new pan — hurts the primary remote-operation use case.

- [x] Approved — 2026-07-04, KK7GWY (all recommendations as written)

## D11 — Radio backends (§5.5)

**Recommendation: pluggable `IRadioBackend` inside `libaethercore`;
SmartSDR stack becomes `FlexBackend` (zero behavior change) in step 2.**
Where DSP runs is a backend property invisible above the seam; KiwiSDR
migrates to a backend later, retiring its side-channels.
*If vetoed:* every future radio family threads through the monolith —
the touchpoint audit (121 headers, 26 already vendor-specific) argues
this is the cheapest moment to cut the seam.

- [x] Approved — 2026-07-04, KK7GWY (all recommendations as written)

## D12 — Capability descriptor (§4.1)

**Recommendation: per-session capability descriptor in `welcome`**
(slice count, sample rates, TX power range, tuner/amp presence, extension
namespaces); clients render what the radio reports.
*If vetoed:* clients hard-code radio assumptions and every new backend
breaks every client.

- [x] Approved — 2026-07-04, KK7GWY (all recommendations as written)

## D13 — Multi-session (§5, §12 Q5)

**Recommendation: reserve session namespacing in protocol v1, ship
single-session first.** The message shape carries a session id from day
one (cheap now, breaking change later); the engine hosts one radio until
multi-radio work is actually scheduled.
*If vetoed toward full multi-session v1:* adds engine lifecycle work to
the critical path for little immediate gain.

- [x] Approved — 2026-07-04, KK7GWY (all recommendations as written)

## D14 — Repo layout (§10)

**Recommendation: monorepo, permanently for engine + models + Qt client;
future splits happen at the protocol line only** (web UI, SDKs, spec).
Rationale in RFC §10 (atomic migration commits, ship-as-a-unit
versioning, repo-scoped governance, CI-enforced dependency direction).
*If vetoed:* paired-PR coordination overhead lands on every migration
change at 5–10 PRs/day.

- [x] Approved — 2026-07-04, KK7GWY (all recommendations as written)

## D15 — Auth credential model (§12 Q3, remainder)

**Recommendation: a separate `aetherd` credential** (per-client identity
+ capability grant: observe / control / TX), NOT SmartLink identity.
SmartLink authenticates *radio↔engine*; reusing it for *engine↔UI* would
couple the boundary to one vendor's account system — wrong for a
multi-backend engine. The same credential rides the browser/TLS opt-in
path.
*If vetoed toward SmartLink reuse:* simpler for Flex-only users, blocks
vendor-neutral remote operation.

- [x] Approved — 2026-07-04, KK7GWY (all recommendations as written)

## D16 — Shack topology as v1 target (§8, §12 Q4)

**Recommendation: yes — engine-at-the-shack is a v1 target**, not a
later nicety. The operator-facing case is §8's list (WAN loss degrades
the view not the radio; TX fail-closed executes at the radio; sessions
outlive clients; thin clients stay thin). Concretely: `aetherd` must run
headless on small ARM hardware (Pi-class) from v1, and CI should build
for it.
*If vetoed (local-only v1):* remote work compresses into "later," and
D5's shipping decision loses its main consumer.

- [x] Approved — 2026-07-04, KK7GWY (all recommendations as written)

## D17 — Core-profile ratification (§2, §5.5, §12 Q6)

**Recommendation: grow incrementally; ratify at protocol v1.0.** The
2026-07-04 tagging pass (121 headers: 38 universal / 16 mixed / 26 vendor
/ 41 ui-support) is the provisional core-profile draft; it hardens as
touchpoints convert and freezes when protocol v1.0 does. Ratifying a
final core profile before any conversion experience would be
speculation.
*If vetoed toward ratify-first:* adds a design phase before step 3 can
start.

- [x] Approved — 2026-07-04, KK7GWY (all recommendations as written)

---

**When all boxes are resolved:** flip the RFC Status line to Accepted
(record date + any overrides), announce the step-1 move window, and start
the §10 sequence.
