# The remote-engine backend (slice 2): desktop AetherSDR as a client of a remote aetherd

Status: **design, approved in its rulings below; for review** (RFC #3849, slice 2 of the
receive-only remote clients). It follows
[`aetherd-remote-clients.md`](aetherd-remote-clients.md) (slice 1: the listener,
scoped credentials, pairing, run-time revocation; #7287, #7305) and the
2026-11-13 amendment to [`aetherd-headless-engine-design.md`](aetherd-headless-engine-design.md)
§10, which orders "a desktop client backend" next. That amendment is the
maintainer's exception to the 2026-09-11 backend freeze for this family; no
other new family is implied.

## Why

An ANAN G2 now runs aetherd on the radio, holding the radio for its own front
panel and serving paired clients over `wss://`. Desktop AetherSDR cannot use
that: its ANAN entry is "in use" and correctly refuses to connect, because a
second Protocol-2 controller makes the relays chatter. This backend makes the
desktop a client of the radio's engine instead of a second controller of the
radio: tune on either the G2 or the desktop and both follow.

## Rulings this document records

| Question | Ruling (date) |
|---|---|
| Order | Ahead of the data plane, receive-only: a desktop client backend after the listener (2026-11-13). |
| Finding an engine | **Advertised by aetherd** on the LAN; the paired engine then appears in the radio list (2026-11-14). |
| Pairing | A **Remote engine** section in the existing connection panel: the advertised (or typed) address, the code shown on the radio's screen, Pair (2026-11-14). |
| Radio ownership | **Never** `radio.connect` or `radio.disconnect` in this slice. The desktop mirrors whatever radio the engine already holds (2026-11-14). |
| Storage | The token in the **OS keychain**; address and certificate fingerprint in settings. A build without a keychain pairs for the session only and says so (2026-11-14). |
| Discovery port | **UDP 4996**, fixed (2026-11-14). |
| Family id | **`aetherd`** (2026-11-14). |
| Advertising | **Whenever the TLS listener runs**; no separate switch (2026-11-14). |

## Protocol authority (Constitution Principles I and IV)

The open protocol authority for this family is this repository's own
[`aetherd-control-protocol-v1-design.md`](aetherd-control-protocol-v1-design.md)
with [`aetherd-remote-clients.md`](aetherd-remote-clients.md). No vendor
protocol is spoken by this backend; the engine on the far side speaks the
radio's. The backend implements only methods and resources those documents
define, and the far engine's `capabilities` decide what is offered.

## The pieces, in PR order

### 1. aetherd advertises its listener (aetherd)

- **Responder:** when the TLS listener runs (`--remote-listen`), aetherd answers
  LAN discovery probes on **UDP 4996**, a fixed port independent of the
  configured `wss` port. It never sends unsolicited broadcasts. Advertising
  follows the TLS listener: there is no separate switch.
- **Probe:** a datagram that is exactly `AETHERD-DISCOVER 1`. Anything else is
  ignored.
- **Reply** (unicast to the prober, one JSON object, at most 1 KiB):
  `{"aetherd":1, "name": <hostname>, "radio": {"model", "family", "connected"},
  "wss": "wss://<address>:<port>", "certificateSha256": <hex>}`. Only what a
  LAN neighbour can already learn by connecting: no credential, no client list,
  no session state beyond whether a radio is held.
- **Bounds:** at most 10 replies a second in all; a probe from a non-private or
  non-link-local source address is ignored. The responder binds the wildcard
  UDP address only to receive broadcasts; the TCP listener keeps its no-wildcard
  rule.
- **The tailnet** is not advertised (no broadcast there): a tailnet address is
  typed.

### 2. The backend (desktop, family `aetherd`)

`RemoteEngineBackend` under `src/core/backends/aetherd/`, selected by
`makeBackend("aetherd")`. It owns one `QWebSocket` on its owner's thread
(THREADING AND LIFETIME CONTRACT, rules 1–6; no worker thread).

**Connect.** `connectRadio(request)` opens `wss://` to the paired address,
checks the presented certificate's SHA-256 against the stored fingerprint (any
mismatch fails closed, with the reason shown, and nothing is sent), sends
`hello` with the stored token, then subscribes to `radioSession`, `slice` and
`panadapter`. `RadioCapabilities` is built from the engine's `radioSession`
capability resource (the records aetherd already publishes:
`sliceFrequencyControl`, `receiveModeControl`, `receiveFilterControl`,
`receiveDspControl`, `receiveAudioControl`, `receivePanCenterControl`,
`receivePanBandwidthControl`, `receivePanRfGainControl`, `radioSpeakerControl`,
`receiveCwPitchControl`), intersected with the methods the engine advertises.
No new capability boolean; a record the engine does not send stays absent.

**Four rules that make a mirror different from every other family** (each is
a test, see *Tests*):

1. **Connecting is silent.** No control request is sent on connect: no restored
   frequency, mode, filter, AGC or band state. `clientSettingsDomains` is empty
   (as Icom's), and whatever `RadioModel` pushes during connect is dropped until
   the first snapshot has arrived.
2. **Disconnecting never releases the radio.** `disconnectRadio()` closes the
   WebSocket and nothing else; `radio.disconnect` is never sent (and neither is
   `radio.connect`). The G2's own panel keeps the radio.
3. **Revisions and rate.** Every control method carries `expectedRevision`. A
   second client moving the same slice makes ours stale (`request.conflict`).
   One request is in flight per resource; newer intents for it replace the
   queued one (latest wins); a conflict re-reads the resource and retries once
   with the latest intent. The engine's per-session budget (100 requests/s,
   burst 200; over it the session is **closed**) is never approached: a knob
   produces at most one request per resource per round trip.
4. **Resync.** `resource.resyncRequired` and a sequence gap trigger a fresh
   snapshot; `resource.changed` for a new slice or panadapter id creates it in
   the model, `resource.removed` retires it.

**Mapping.** Engine resources become the seam's existing typed deltas
(`SliceDelta`, the panadapter and session deltas); no new delta type. Seam
verbs become protocol methods:

| Seam verb | Protocol method | Notes |
|---|---|---|
| `setSliceFrequency` | `slice.setFrequency` | |
| `setSliceMode` | `slice.setMode` | |
| `setSliceFilter` | `slice.setFilter` | |
| `setSliceAgc(mode, threshold)` | `slice.setAgcThreshold` | v1 has no AGC-mode method: a call that changes the mode is refused, and the AGC feature is declared for the threshold only |
| audio gain / mute | `slice.setAudioGain`, `slice.setAudioMute` | |
| NB / NR | `slice.setNoiseBlanker`, `slice.setNoiseReduction` | |
| RIT | `slice.setRit` | |
| CW pitch | `radioSession.setCwPitch` | |
| radio speaker | `radioSession.setRadioSpeaker` | |
| pan center / bandwidth / RF gain | `panadapter.setCenter`, `.setBandwidth`, `.setRfGain` | |
| `setKeying` and every TX verb | none | **TX absent by construction** (Principle VI): no transmit capability is declared and keying is refused, as in the simulator |
| `connectRadio` / `disconnectRadio` | none | rule 2 |

Anything else the seam offers stays at its default (unsupported) and its
capability is absent (#5554: no capability without a verb behind it).

**No spectrum, no audio** in this slice: the panadapter carries the engine's
center, bandwidth and RF gain, but no frames until RFC step 4a; no receive
audio until step 4b. The radio list labels the entry so the operator knows.

### 3. Pairing and the radio list (desktop UI)

- **Discovery:** the desktop sends `AETHERD-DISCOVER 1` with its existing LAN
  discovery and lists each reply as "<model> — remote engine (<name>)". The
  same radio's raw ANAN entry stays listed as "In use" (the F5 rule already
  refuses to connect to it).
- **Remote engine section** in the connection panel: address (filled from the
  advertised entry, or typed), the code from the radio's screen, Pair. Before
  sending the code, the desktop checks the code's last four digits against the
  certificate it sees (slice 1 §3); a mismatch is shown and nothing is sent.
- **Storage:** after pairing, the token goes into the OS keychain (QtKeychain,
  the same path as other credentials); the address, the certificate fingerprint
  and the engine's name go into settings under the engine's credential id. With
  no keychain backend, the pairing lasts for the session and the panel says it
  will not be remembered.
- **Revocation from the desktop** is "Forget", which deletes the local token
  only; removing the client from the radio is done on the radio's screen.

## Hazards

- **Two controllers:** the backend never speaks Protocol 2 and never sends
  `radio.connect`. The raw ANAN entry for the same radio remains refused while
  in use (`ConnectionSharingPolicy.h`). The desktop's own ANAN/HL2 auto-connect
  must stay off for a radio its engine holds, as today.
- **A changed certificate** (the G2 reinstalled) fails closed with a message
  naming the fingerprint change; re-pairing replaces it.
- **The engine restarts or goes away:** the WebSocket closes, the backend
  reports disconnected; reconnect is the existing automatic-reconnect path,
  with no busy probe needed (nothing on the radio is touched).

## Out of scope for slice 2

Spectrum and waterfall frames (step 4a), audio (step 4b), transmit, slice and
panadapter creation (no methods yet), `radio.connect`/`radio.disconnect`,
remote engines over SmartLink.

## Relationship to the desktop adapter and the open RFCs

This backend is **not** the RFC's desktop adapter (the Qt UI driving a local
engine through the protocol instead of the models): that has not landed, and
the desktop keeps consuming its models directly. This is a radio family whose
"radio" is a remote aetherd, and it feeds those same models.


#7276 proposes that the Qt app become an aetherd client; this backend is that
direction's first concrete step and does not depend on #7276 being accepted.
#7277's out-of-process backend packages do not apply: this family runs in the
desktop process like every current family, and speaks only aetherd's protocol.

## Tests

- **Loopback** (socket-owning, 127.0.0.1, ephemeral port, as
  `remote_control_loopback_test`): the backend against an in-process
  `ControlEngine` plus `RemoteControlServer`, with a fake receive target.
  - connect sends nothing but `hello` and `resource.subscribe` (rule 1);
  - disconnect sends nothing and the engine still holds its radio (rule 2);
  - a burst of 1000 frequency intents produces at most one request per round
    trip and ends at the last value, under the budget (rule 3);
  - a conflicting change from a second session is retried with the latest
    intent (rule 3);
  - a forced resync and slice add/remove reach the model (rule 4);
  - a wrong certificate fingerprint fails before anything is sent;
  - keying is refused and no TX capability is declared.
- **Seam:** the family joins `backend_seam_affinity_test` (rule 1 and a cold
  construct/teardown) and `backend_family_switch_test`.
- **Discovery:** the responder's probe format, reply contents, rate bound and
  source-address filter, socket-free where possible plus one loopback probe.
- **Live:** this desktop against the G2, with the desktop's FLEX isolation and
  its ANAN/HL2 auto-connect off.
