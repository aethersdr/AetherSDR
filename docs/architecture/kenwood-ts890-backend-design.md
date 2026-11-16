# TS-890S Ethernet receive backend — design proposal

**Status:** Proposed, not approved or implemented by this document.
**Scope:** One TS-890S receive session over KNS Ethernet; no transmit, memories,
remote power control, or new native setup pages in the first landing.
**Family RFC:** [#7377](https://github.com/aethersdr/AetherSDR/issues/7377), approval pending.
**Architecture:** An out-of-process backend, dependent on RFC #7277's shared
protocol and host. No exception to the #5554 backend freeze is requested.

## 1. Intent and approval boundary

The [November 14 maintainer ruling](https://github.com/aethersdr/AetherSDR/pull/7118#issuecomment-6883584363)
requires a separate design approval and family RFC, followed by an
out-of-process RX-only implementation. The
[November 15 integration advice](https://github.com/aethersdr/AetherSDR/pull/7118#issuecomment-6901139113)
requires preserving the common client experience and using `radiocert` for
bring-up. This follows the documentation-first precedent of
[FLEX-1500 PR #7107](https://github.com/aethersdr/AetherSDR/pull/7107), not its
older in-process implementation proposal.

[PR #7118](https://github.com/aethersdr/AetherSDR/pull/7118) stays open as the
reference implementation. Its commit `75ec733bf18b58ac03734ff23dfc7c6ae53ad437`
is the implementation/evidence snapshot considered here, not a merge candidate
endorsed by this design. In particular, its compiled TX paths, shared TX changes,
new setup widgets and raw lab artifacts are not proposed for the first landing.
The family RFC must be approved before implementation; documentation approval
alone does not approve new UX, a process protocol or a freeze exemption.

## 2. Protocol authority and provenance

### Sources and precedence

1. Kenwood's public [PC Control Command Reference, January 30, 2019](https://www.kenwood.com/i/products/info/amateur/pdf/ts890_pc_command_en_rev1.pdf)
   is the command authority. Its individual command entries define parameter
   domains; its LAN procedure defines authentication and liveness.
2. Kenwood's [KNS Setting Manual, August 30, 2018](https://www.kenwood.com/i/products/info/amateur/ts_890/pdf/ts890_kns_manual_e.pdf)
   defines network setup and built-in VoIP operation. The
   [official download index](https://www.kenwood.com/i/products/info/amateur/software_download.html)
   identifies editions. Kenwood's instruction and in-depth manuals explain
   operating context, not an alternative wire encoding.
3. Reproducible observations on an identified TS-890S firmware can establish a
   narrow exception to the published encoding. Record firmware, request/reply,
   preconditions and restoration; add a regression. An exception applies only
   to that observed context. Unknown firmware is not silently assumed identical.
4. Public community source corroborates or suggests experiments. It cannot
   override the manual or establish hardware support by itself. Disagreements
   without adequate observation leave a control unavailable.

The following sources were inspected at these exact commits:

| Source | Revision | Relevant evidence |
|---|---|---|
| [tonyc/open890](https://github.com/tonyc/open890/tree/9d134e5e5c060a35366357c8b09c6ebb3b267164) | `9d134e5e5c060a35366357c8b09c6ebb3b267164` | `tcp_client.ex`: KNS handshake and scope; `udp_audio_server.ex`/`rtp.ex`: receive audio; `connection_commands.ex`: command inventory |
| [aneitzel/open890rc](https://github.com/aneitzel/open890rc/tree/3a0ce2d2e6ed70a4a87f5f40681895a1fec3d38f) | `3a0ce2d2e6ed70a4a87f5f40681895a1fec3d38f` | `internal/radio/conn.go`: login/liveness; `internal/audio/audio.go`: PCM and sequence discontinuities; `internal/cat/messages.go`: APF discrepancy |
| [w9fyi/open890](https://github.com/w9fyi/open890/tree/2ca36b1d0f6fb3e9e47bf0e653729b26612627ac) | `2ca36b1d0f6fb3e9e47bf0e653729b26612627ac` | Open890-derived control/scope and additional TX audio; not an independent hardware validation |

The inspected root license texts contain MIT terms; Open890 also identifies
its pcm-player license. No source code, UI assets or dependencies from these
projects are included by this document. Any later reuse requires its own
license/notice review. Protocol design uses public documentation, open-source
references and qualified wire observations, never proprietary binary inspection
or decompiler output (Constitution IV). The previously referenced local
`Kenwood-TS-890-Client-TCI-Server-v2` checkout was not found during this study;
no new claim rests on it. Earlier parity notes and screenshots are historical
research, not an unnamed authoritative reference application.

### Evidence that changes the design

- **Documented:** KNS uses `##CN` then `##ID`, semicolon framing and a
  communication timeout. Default TCP control is 60000; built-in receive VoIP
  uses UDP 60001. Ports and framing are not an encrypted channel.
- **Firmware-observed, historical:** firmware 1.01 returned APF Off as `AP00`
  and On as `AP01`. Open890RC explicitly describes the conflict with the
  manual's shifted enumeration. The future backend must pin that exception
  and verify its adopted firmware profile rather than accept arbitrary values.
- **Firmware/profile discrepancy:** the 2019 `BS4` span table differs from
  Open890/Open890RC's eight-entry table and historical 1.01 span observations.
  Readback of an index alone cannot resolve its physical width. Pin a
  firmware-qualified table using a known off-centre carrier and confirmed
  scope edges; do not infer width from the UI label or import a table blindly.
- **Implementation evidence:** the reference decodes 640 scope levels and
  RTP-framed 16 kHz mono PCM. These are bounded protocol/transport assumptions
  to revalidate across the process boundary, not raw IQ or calibrated RF data.
- **Historical operator evidence:** receive audio was audible; tuning, scope,
  APF and reconnect checks were exercised on firmware 1.01. Some endurance was
  measured and a later 12-minute gate was operator-accepted rather than fully
  measured. None certifies a future executable or process transport.
- **Not established:** current firmware coverage, cross-platform live behavior,
  process-hop latency/endurance, production certification, or TX operation.

## 3. Process boundary and upstream dependencies

Baseline for this assessment: upstream `9002b51bb2ae4a50fe6776e69aa5e3c82a98868a`.

| Existing upstream | What it does not yet establish |
|---|---|
| `IRadioBackend`, typed requests/deltas and capability records | A serializable process protocol: callbacks, pointers, synchronous results and in-process lifetime guards need explicit IPC equivalents |
| `core/control/ControlProtocolCodec`, resource adapters, local/remote engine listeners | These serve clients of aetherd; they are not a backend-worker host protocol |
| `ReceiveData.h` leases and bounded Flex receive delivery | In-process revocation/ownership, not shared-memory lifecycle or cross-process stale-frame protection |
| `RadioCertification`, automation bridge and persistence tools | Diagnostic evidence; not universal hardware pass/fail or ready-made Kenwood persistence scenarios |
| Signed application updater | Backend package identity, compatibility, activation/rollback and installer integration still require the approved shared package contract |

[RFC #7277](https://github.com/aethersdr/AetherSDR/issues/7277) now has a
[maintainer ruling](https://github.com/aethersdr/AetherSDR/issues/7277#issuecomment-6885056767) approving its migration direction: aetherd owns credentials
and settings; receive DSP is centralized in aetherd; restart resumes a session
without a TX grant; packages use the signed updater. These decisions are
binding direction, not proof the worker protocol/host has landed. Its simulator
prototype and protocol acceptance gates remain prerequisites. The remote-engine
client [design](../aetherd-remote-backend.md) is explicitly a different boundary.

Proposed ownership, subject to that shared contract:

- One backend process per TS-890S session, supervised by aetherd. The worker
  owns the radio's TCP/UDP sockets and vendor parser. It never owns widgets,
  the settings database, QtKeychain, global DSP or a raw client command tunnel.
- aetherd supplies a session-scoped endpoint/credential and feature-document
  snapshot over authenticated, access-controlled local IPC. No secrets in
  argv, environment, logs, recordings or protocol-error dumps. Local secrets
  remain transient in the worker and retire with the session.
- Requests and observations use the approved typed vocabulary. IPC must carry
  session/resource incarnation, request correlation and explicit completion,
  cancellation and unsupported results. Queue acceptance is not radio readback.
  No second Kenwood-specific IPC, JSON-RPC server or speculative seam is added.
- Supervision retires all leases, pending replies and audio before reporting
  failure. A new worker gets a new incarnation and fresh radio snapshots;
  delayed old control/status/data cannot attach to it. Restart follows #7277:
  automatic receive recovery, operator notification, never replayed TX authority.
- Version/capability negotiation refuses incompatible workers before radio
  connection. Packaging/signing and atomic activation follow shared #7277
  infrastructure on Linux, macOS and Windows; no ad hoc helper installer.

Initial connection uses an explicitly configured endpoint through the approved
connector/host surface. No new discovery probe, login panel or automatic
power-on is introduced here; any missing connector surface is a separately
approved prerequisite.

KNS already supplies demodulated audio and a scope. Centralized host DSP does
not imply inventing an IQ stream from PCM or running a second demodulator.
The shared data plane must accept this native PCM/spectrum case. Use its approved
bounded audio ring and latest-complete-spectrum delivery, with sample rate,
sequence, timestamp and incarnation metadata. On overflow, report audio gaps
and discard obsolete display frames; never grow queues without limit or replay
old audio after reconnect. One producer reaches each existing engine consumer.
Local incarnation metadata fences IPC but cannot by itself identify a delayed
UDP packet from the same radio after reconnect. The stream-start admission and
sequence/SSRC reset policy needs measured reconnect evidence; stale audio must
not be called solved by tagging it with a new local generation.
Exact formats, capacities, timeout budgets and compatibility fields belong to
the shared protocol and must be pinned before implementation.

## 4. First experimental RX capability plan

One physical receiver and one spectrum view; A/B are radio registers, not two
independent receivers. VFO memory, TF-SET, transverter or observed TX contexts
that cannot safely use ordinary RX coordinates refuse affected writes with a
reason. External radio changes win over client drafts. Connection snapshots
never restore frequency/mode/filter from client settings.

`D` = official command documentation; `S` = reference software implementation;
`H` = limited historical firmware-1.01 observation. No row claims validation of
the new process implementation.

| Capability | Existing seam/control to reuse | Kenwood source | Evidence; disposition |
|---|---|---|---|
| Identity, login, liveness | Connect descriptor, connection/error observations | `ID` (`ID024`), `##CN`, `##ID`, bounded keepalive | D/S/H; first RX landing, reject other models |
| Absolute tuning and RX VFO | `SliceTuneRequest`, frequency delta, existing VFO entry | `FA`, `FB`, `FR`, `IF` | D/S/H; first landing, radio readback confirms frequency |
| Native relative tuning | Approved typed native intent/extension only | `FC` multipliers 1/2/5/10/50/100; `UD` step count on RX-selected VFO | D/S/H; retain backend plan, explicit UX approval before adding an action; existing wheel/keyboard/STEP unchanged |
| Modes and passband | `ReceiveModeControl`, `SliceFilterRequest`, existing mode/filter controls | `OM`, `SL`, `SH`, `FW`, filter-bank context | D/S/H representative modes; first landing only for tested mode/width combinations |
| RF gain, squelch, AGC | Existing receive records and `SliceAgcRequest` | `RG`, `SQ`, `GC`, `GT` | D/S; first landing after readback/effect tests; no invented AGC-threshold mapping |
| Native NB/NR/notch/APF | `SliceDspRequest` and existing available controls | `NB1/2`, `NR`, `NT`, `BC`, `AP0` | D/S, APF H; publish only exact supported semantics; native advanced parameters deferred |
| CW receive pitch, RIT state | Existing pitch/RIT controls and confirmed deltas | `PT`, `RT` On/Off only; shared offset readback | D/S; first landing after effect/restoration tests; `RU`/`RD` offset writes and `RT2` shift deferred, XIT and keyer excluded |
| Scope and waterfall | Existing pan requests/signals and render route | `DD0`, `##DD2`, `BS3/4/5`, edge/context readbacks | D/S/H; first landing, fixed geometry keeps waterfall stationary while VFO moves; zoom obeys native spans |
| Receive meter | `MeterModel` and existing meter consumers | `SM` | D/S/H; 0–70 native deflection dots, not invented dBm/S-unit calibration; invalidate on TX/unknown, fresh RX sample required; representation contract requires agreement |
| PC receive audio | Existing engine PCM/playback consumer, host gain/mute | `##VP` and receive RTP/PCM | S/H plus KNS manual; first landing; no TX audio socket/encoder or DATA SEND mutation |
| Preamp/ATT, antenna selection | Existing front-end/antenna controls | `PA`, `RA`, `AN` | D/S; first landing only where individual fields preserve unrelated TX/routing fields |
| Advanced filter-bank/reset, APF shift/width/gain, EQ, routing, scan/auto-mode, display/beeps, decoders | Proposed native pages in #7118 | Native commands in reference inventory | Mostly D/S, limited H; deferred to separately approved UX/feature PRs |
| Memories, memory scan | Unavailable reason, no read/recall/write commands | Memory command families | Unsupported by operator scope; existing host bookmarks unchanged |
| Power, firmware and administration | No public mutation route | `PS`, Wake-on-LAN, firmware and KNS-user writes | Excluded |
| All transmit/keying/tune/audio | Existing unavailable TX presentation | `TX`, `KY`, `AC`, VOX and generator paths | Future TX PR; absent from RX executable |

A capability is absent until its actual verb/observation and conversion are
implemented and validated. Existing controls remain visible but unavailable
with accessible reasons where required by the GUI guide. No family-name branch
above the seam, new global default, changed shortcut or new native widget is
part of this landing. Native units/relative amplitude must be represented by
an approved neutral contract; if existing consumers cannot display them honestly,
that is a separate reviewed prerequisite, not a reason to falsify calibration.

RIT and XIT share an offset register: `RU`/`RD` change both stored offsets,
even when XIT is Off. First scope therefore reads the shared offset but does
not write or clear it; `RT` is restricted to On/Off, excluding `RT2` shift.
Any later offset editing needs explicit approval of shared-register ownership
and truthful presentation of its TX-state effect, not merely an XIT-Off guard.

`SM` reports S-meter deflection only during confirmed RX; during TX it reports
power. Invalidate receive level and freshness immediately on observed TX or
unknown receive state. Do not relabel a power sample as an RX level. Re-enable
only after fresh, qualified RX-state and meter observations in the current
session; reject ambiguous or late pre-transition replies. Socket-free tests
must cover external TX, unknown state, late replies and return to RX without
ever granting the backend TX authority.

## 5. RX-only and transport safety contracts

`canTransmit` is always false, even for an administrator or `##TI1` account.
Every derived TX capability is absent. The RX package compiles no keying, CW
text, tune, ATU-start, VOX-enable, test-generator or TX-audio encoder. It has no
outbound microphone/PCM path. In particular, reference `KenwoodTransmit.cpp`,
`KenwoodTune.cpp`, native TX widgets and shared `TxCoordinator` additions are
not carried into the RX contribution.

The final vendor writer accepts only a reviewed RX command allowlist, including
operation-specific parameter validation. No generic raw CAT/extension escape
hatch can encode TX, power, memory or administrative writes. Fail-closed tests
inject every TX producer and exercise an account granting TX, unknown state,
reconnect and malformed IPC. They must observe no forbidden wire command;
mutating the final allowlist/refusal must make those tests fail. Read-only
observation of an externally keyed radio grants no write authority.

Bound TCP message/tail lengths, datagrams, arrays and work per event-loop turn.
Validate scope levels/geometry and full RTP structure before sample access;
accept UDP only from the configured/resolved TCP peer address, count
loss/reordering and reject stale-session packets. KNS authenticates the client login, not the
radio/server identity; source-address matching is filtering, not cryptographic
authentication. Fixed UDP port ownership is exclusive; report
contention rather than silently sharing another session's audio. Whether a
second receive port reservation is actually required is a measured protocol
question; a TX socket is not justified by the reference doing it.

Normal disconnect requests VoIP stop using bounded graceful drain, then closes;
a timeout forcibly retires local sockets and reports incomplete cleanup. An
abort immediately after buffered writes is not successful delivery. Peer loss
and worker death retire local authority regardless of radio-side acknowledgement.
None of this proves a physical transmitter is unkeyed; this RX package cannot
claim ownership of or control over an externally initiated transmission.

## 6. Credentials and persistence

KNS credentials travel unencrypted on the radio link. Use a trusted network or
an independently secured path; do not expose KNS directly to the Internet or
reuse a valuable password. This belongs in the eventual connection user guide.

Only aetherd's credential owner accesses QtKeychain. Persistence needs explicit
consent, session-only fallback without secure storage, a scoped credential-policy
entry, redacted export and checked write results. Opt-out/Forget must delete a
previously stored secret; endpoint/user/role changes must not orphan it silently.
Credential replies retain the originating connection incarnation and revision.

aetherd owns one versioned connection/preference object and atomic writes. The
worker receives it and requests changes through the approved host API; it never
opens `AppSettings`. Radio-persistable values stay on the radio, with empty
`clientSettingsDomains` for those values. Host playback/layout preferences stay
client-owned. Confirm actual persistence field by field; do not assume all
scope or VoIP settings are transient. Starting PC Audio must not change `MS`
DATA SEND or any transmit input setting as a side effect.

## 7. Bounded bring-up and acceptance evidence

No radio is contacted to prepare this design. Later hardware work requires an
exclusive, operator-authorized RX session. Record executable/package and source
hashes, firmware, effective capability profile, RX-only build proof and TX
permission disabled. Snapshot every field to be touched; no radio power cycle,
firmware operation, memory write or transmit is part of this plan.

**Mandatory tooling gates before live phases:** current `radiocert` is not
yet sufficient for the contracts below. Land and verify separately:

- Session/resource-incarnation and per-field edit-revision fences on every
  test mutation and restoration, including individual-stage guards and the
  final epilogue. Current `RadioCertification::run()` re-fetches slice 0 and
  restores captured frequency/mode unconditionally. Capture original ownership;
  restore only fields still owned by this run and unchanged since its last
  write. On operator edit, disconnect/reconnect or replacement, retire the run
  and never write the old snapshot into newer state. Regressions inject each
  event during a phase and before restoration, and must fail without the fence.
- Capability-driven, side-effect-free RX phase selection. A TX-disabled
  `meters` command currently still runs TX-oriented stages: key refusal does
  not skip their tone/configuration probes. RX runs must skip these stages and
  report them **not applicable**, without TX test tones, microphone/processor
  gain changes, TX-monitor changes or RF-power clamping/restoration. Apply the
  same no-TX-side-effect contract to tune/rx and their common epilogue. Pin it
  with injected state/transport tests and negative mutation checks. Keep the
  bridge TX refusal and final backend allowlist as independent backstops.

Do not run the live commands below until these gates are demonstrated. The
RX-only phase selection is a required tool change, not an existing command flag.

| Phase | Observations and objective evidence | Manual check and restoration |
|---|---|---|
| `radiocert tune` | UI → typed request → IPC → radio → observation → UI; absolute/mode round trips; pan/slice/waterfall frequency agreement, native span boundaries and rapid tuning; radio-side changes converge without feedback | Off-centre known carrier and fixed-scope marker motion; restore original VFO/mode/filter/scope context with readback |
| `radiocert rx` | Known carrier, zero/off-centre and sideband cases; filter change effect; PCM sequence/gap counts and engine-consumer agreement; exercise each declared RX control both ways | Listen for intelligibility and verify carrier location against the radio; restore DSP/filter/audio and any changed receive fields |
| `radiocert meters`, gated RX-only tooling, TX disabled | Fresh meter definition/value/age at weak and stronger signals; both widgets and published model agree on native units; run non-keying control/interlock/squelch stages; skip TX-oriented stages as not applicable; no tone/TX-config mutations | Compare native deflection to front panel with a steady signal; no dBm/S-unit accuracy claim; restore gain/squelch/front-end settings |
| `radiocert persist` plus applicable runner | Snapshot alone is read-only. A bounded runner checks representative radio-owned RX values and host-only preferences through normal quit/relaunch, without stale replay; two polls establish continuing convergence | Compare radio/front-panel state before and after reconnect; restore seeded values only if unchanged by the operator; verify each restored readback |
| Failure/endurance | Two reconnects, worker crash/restart, IPC interruption/backpressure/stale replies, and at least 12 measured minutes receiving; continuous liveness/PCM/display and bounded memory/backlog evidence | Audible RX before/after recovery; verified final baseline and clean teardown; no substituted operator acceptance for timed measurement |

Stop on unsupported/ambiguous state, unexpected power/TX/routing mutation,
external operator edits, stale identity, failed restoration, loss of valid
geometry/audio or unbounded backlog. Retain partial evidence and report the
failure; never restore an older snapshot over a newer operator choice. Every
phase restores before proceeding, and a restoration failure ends the session.

`radiocert` is a diagnostic, not automatic pass/fail. Never run its current
unadapted meters path, `radiocert tx` or unrestricted `all` for this scope.
Production certification remains a later maintainer decision with the full
applicable suite and manual checks; experimental acceptance is not `all`
certification.

Other separately verified tooling prerequisites are native meter units,
non-applicable DSP results for radio-demodulated audio, process incarnation/
backlog observations, and persistence runners adapted to the actual capability
and ownership contract. Their current family lists/scenarios cover
Flex/HL2/Icom rather than Kenwood. Do not simply add a family name to a runner
that assumes different ownership, controls or units. Tool changes and tests
land separately; this document changes none.

## 8. Delivery sequence and decisions

1. **This design and family RFC:** approve authority/precedence, bounded RX
   matrix and exclusions. Keep #7118 as reference. No executable/build/CI edits.
2. **Shared #7277 prerequisites:** approved protocol, simulator process proof,
   supervision, settings/credentials, native PCM/spectrum data plane, compatible
   packaging and the desktop/engine route. Each belongs to its upstream owner;
   Kenwood does not implement a parallel host or widen the seam opportunistically.
3. **RX worker/parser:** port only admitted command/stream logic, remove all TX
   and persistent-store access, pin boundary/allowlist/lifetime regressions.
4. **Engine integration and existing controls:** use the approved common host;
   justify every unavoidable shared change and new UI→engine touchpoint. Separate
   generic fixes from family integration; no common UX changes. Address #7118's
   credential deletion and VoIP drain findings before reuse. External-client
   support stays limited to verified generic behavior; its TCI volume and rigctl
   no-change-width findings must not be inherited.
5. **Certification and experimental docs:** first land and verify §7's required
   restoration and RX-only tooling gates; then execute §7 on the actual process
   candidate, then maintainers decide experimental availability. Native setup
   pages and other advanced UX get separate approval/PRs. TX requires its own
   hardware evidence, protocol-failure safety and Principle VI review.

Decisions requested before step 3:

- Approve the first RX matrix and defer the reference's advanced setup pages;
  specify whether native FC/UD actions belong in a later UX PR.
- Name the shared #7277 milestone/API the worker should target, its owner and
  Kenwood's migration slot; the published order does not name this new family.
- Confirm native PCM/scope support within the approved data plane and the honest
  native-meter/relative-spectrum representation, including whether shared
  prerequisite records or consumer changes need separate RFC approval.
- Confirm experimental firmware/platform acceptance criteria and the mechanism
  for narrowly recorded firmware exceptions. Historical 1.01 evidence does not
  establish all currently available firmware or Intel/Windows behavior.

These are approval questions, not permissions inferred from green CI. This
proposal neither declares the integration complete nor changes the existing
client experience for any radio.
