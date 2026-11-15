# The remote-engine backend (slice 2): desktop AetherSDR as a client of a remote aetherd

Status: **design, approved in its rulings below; for review** (RFC #3849, slice 2 of the
receive-only remote clients). It follows
[`aetherd-remote-clients.md`](aetherd-remote-clients.md) (slice 1: the listener,
scoped credentials, pairing, run-time revocation; #7287, #7305) and the
2026-11-13 amendment to [`aetherd-headless-engine-design.md`](aetherd-headless-engine-design.md)
§10, which orders "a desktop client backend" next. The maintainer's
2026-09-11 decision of no new radio backends until aetherd is complete
([#4920](https://github.com/aethersdr/AetherSDR/pull/4920#issuecomment-5641933855))
does not cover this family, because the family is part of aetherd itself.
That amendment orders this family and no other.

## Why

An ANAN G2 now runs aetherd on the radio, holding the radio for its own front
panel and serving paired clients: `wss://` on the LAN and, where configured,
plain `ws://` on a tailnet (WireGuard) address. Desktop AetherSDR cannot use
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
  "wss": "wss://<address>:<port>", "certificateSha256": <hex>}`. This is more
  than a connection without a token learns: before `hello`, a session answers
  every request with `auth.required`. The reply gives any LAN prober the host
  name, the radio's model and family and whether a radio is held, with no
  authentication. That is accepted because the radio list needs the model and
  name to label the entry. The reply never carries a credential, the client
  list or any other session state.
- **Bounds:** at most 10 replies a second in all. A probe is ignored if its
  source is neither private (RFC 1918, or IPv6 ULA `fc00::/7`) nor link-local
  (`169.254/16`, `fe80::/10`). It is also ignored if it arrives on a WireGuard
  interface (`tailscale*` or `wg*`, the same names as the server's tailnet
  check) or comes from `100.64/10`. The interface test is needed because
  Tailscale's IPv6 range `fd7a:115c:a1e0::/48` is itself ULA. The responder
  binds the wildcard UDP address only to receive broadcasts; the TCP listener
  keeps its no-wildcard rule.
- **The tailnet** is not advertised, per the bounds above: a tailnet address is
  typed (see *Two paths* below).

### 2. The backend (desktop, family `aetherd`)

`RemoteEngineBackend` under `src/core/backends/aetherd/`, selected by
`makeBackend("aetherd")`. It owns one `QWebSocket` on its owner's thread
(THREADING AND LIFETIME CONTRACT, rules 1–6; no worker thread).

**Two paths, matching slice 1's two listeners**
([`aetherd-remote-clients.md`](aetherd-remote-clients.md) §3, §4):

| Path | Listener and scheme | Pairs here | Authenticates with |
|---|---|---|---|
| LAN | the TLS listener (`--remote-listen`), `wss://` | yes, the only place `pairing.complete` is accepted | the pinned certificate fingerprint, then the bearer token at `hello` |
| Tailnet | the tailnet listener (`--remote-listen-tailnet`), plain `ws://` | no: the server refuses `pairing.complete` with `capability.unavailable` | the bearer token at `hello`, over WireGuard; there is no certificate to pin |

- **Pairing is LAN-only.** A desktop pairs once on the engine's LAN, over `wss://`.
  An operator who has never been on the radio's LAN cannot pair; that is
  slice 1's rule, not one this slice relaxes.
- **A tailnet address belongs to an engine already paired.** The operator types
  it into that engine's record. The token goes only to the address it was
  paired at and to tailnet addresses stored in the same engine's record, and to
  nothing else.
- **`ws://` is admitted only over WireGuard.** After the TCP connect and before
  anything is sent, the socket's local address must belong to an up
  `tailscale*`/`wg*` interface on the desktop. This is the client-side mirror of
  the server's `RemoteControlServer::wireGuardInterfaceFor()` check, applied to
  the client's own end. If that fails, the connect fails closed, says why, and
  `hello` is never sent. A plain `ws://` LAN address is never accepted.
- **The hazard.** On the tailnet path, the WireGuard peer identity is the only
  thing that says the far end is the engine. The certificate pin does not
  apply there.

**Connect.** `connectRadio(request)` opens the stored address on one of those
two paths. On `wss://` it checks the presented certificate's SHA-256 against
the stored fingerprint (any mismatch fails closed, with the reason shown, and
nothing is sent); on `ws://` it applies the WireGuard check above. It then sends
`hello` with the token, and subscribes to `radioSession`, `slice` and
`panadapter`.

**The token on the connect path.** `RadioModel::populateFamilyParams()` is
synchronous, because the auto-reconnect timer calls it, while a keychain read
is asynchronous. The family therefore keeps a session cache, the
`IcomCredentials` pattern (`src/core/backends/icom/IcomCredentials.h`). The
connection panel primes it at pairing or at its first keychain read, and
connect and reconnect read only the cache. An empty cache fails the connect
with "not paired on this machine" rather than blocking.

**Capabilities.** `RadioCapabilities` is built from the engine's `radioSession`
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
   - **Latest wins.** One request is in flight per resource, and newer intents
     for it replace the queued one. A conflict re-reads the resource and
     retries once with the latest intent. The one-in-flight rule also bounds
     the burst after a stalled link.
   - **A session-wide client bucket.** Latest-wins alone does not cap the rate:
     at a 1–3 ms LAN round trip, one knob would send 300–1000 requests a second.
     The engine's per-session budget is a token bucket: `requestsPerSecond`
     100 and `requestBurst` 200 in `welcome.limits`
     (`ControlSession.h`, `ControlService.cpp`). It charges every frame after
     `welcome`. The first frame over budget gets `transport.limit_exceeded`
     and the session is **closed**.
   - **What the client bucket charges.** Every frame the backend sends after
     `welcome`: control requests, subscribes, re-reads, conflict retries and
     resync snapshots.
   - **Its size.** Half of each advertised limit, so 50 a second with a burst
     of 50 at today's values. It falls back to exactly that when `welcome`
     carries no limits.
   - **The invariant.** Client burst + client rate × T stays below server
     burst + server rate × T for every interval T. The 150 frames of
     difference in burst absorb frames that the network delivers bunched
     together, since the server measures at receipt. A frame with no token waits
     in the latest-wins queue, which therefore still holds only the newest
     intent per resource. Tuning stays smooth at 50 updates a second, and the
     session is never closed for rate.
4. **Resync.** `resource.resyncRequired` and a sequence gap trigger a fresh
   snapshot, charged to the client bucket. A `resource.changed` event for a new
   slice or panadapter id creates that slice or panadapter in the model, and
   `resource.removed` retires it.

**Mapping.** Engine resources become the seam's existing signals
(`src/core/backends/IRadioBackend.h`); no new delta or signal type:
- **slice:** `sliceChanged(sliceId, SliceDelta)` and `sliceRemoved`.
- **radioSession:** `radioChanged(RadioDelta)`.
- **panadapter:** the per-field pan signals the non-Flex families use
  (`panCenterBandwidthChanged`, `panRfGainChanged`, `panRemoved`), not
  `displayObserved`. `DisplayObservation` is the Flex-shaped report, and
  RadioModel applies it only inside an observation session open for its status
  epoch (`docs/agents/backends.md`, *Display status observations*).

Seam verbs become protocol methods:

| Seam verb | Protocol method | Notes |
|---|---|---|
| `setSliceFrequency` | `slice.setFrequency` | |
| `setSliceMode` | `slice.setMode` | |
| `setSliceFilter` | `slice.setFilter` | |
| `setSliceAgc(mode, threshold)` | `slice.setAgcThreshold` | v1 has no AGC-mode method. `setSliceAgc()` returns `void`, so a call whose mode differs from the engine's is dropped, logged, sends nothing, and the model keeps the engine's mode. `hasAgcThreshold` is declared, and `agcModes` holds only the engine's current mode, so the AGC selectors (`RxApplet`, `VfoWidget`, through `setAgcModeAvailability`) offer no other |
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

**No spectrum, no audio** in this slice. The panadapter carries the engine's
center, bandwidth and RF gain, but no frames, and there is no receive audio.
Both wait for the binary data plane: step 4 of RFC #3849, numbered step 5 in
[`aetherd-headless-engine-design.md`](aetherd-headless-engine-design.md) §10.
The radio list labels the entry so the operator knows.

**Builds without `HAVE_WEBSOCKETS`.** Qt6::WebSockets is optional. For any
family it does not recognize, `RadioModel::makeBackend()` falls through to
`FlexBackend`. So a build without it needs an explicit `aetherd` branch that
returns `nullptr`, like the RTL `#ifdef` there. The connection panel hides the
Remote engine section, and the discovery probe is not sent.

### 3. Pairing and the radio list (desktop UI)

- **Discovery:** the desktop sends `AETHERD-DISCOVER 1` with its existing LAN
  discovery and lists each reply as "<model> — remote engine (<name>)". The
  same radio's raw ANAN entry stays listed as "In use", and
  `src/core/backends/ConnectionSharingPolicy.h` still refuses to connect to it.
- **The entry is never "in use".** For the `aetherd` entry, the engine holding a
  radio is the normal case, not a busy one. Every connect gate refuses a
  non-Flex entry marked in use (`ConnectionPanel.cpp`, `MainWindow_Session.cpp`
  through `familySupportsSharedInUseConnect()`). So the entry never sets
  `RadioInfo::inUse`, from `radio.connected` or anything else, and
  `ConnectionSharingPolicy.h` does not grow. If the engine's network-session
  budget is full, that shows as the connect error.
- **Remote engine section** in the connection panel: an address (filled from
  the advertised entry, or typed), the code from the radio's screen, and Pair.
  Pairing always uses the engine's LAN `wss://` address. Before sending the
  code, the desktop checks the code's last four digits against the certificate
  it sees (slice 1 §3). A mismatch is shown and nothing is sent. A tailnet
  `ws://` address is added to an engine that is already paired; it is never
  used for pairing.
- **Storage:**
  - **The token** goes into the OS keychain through QtKeychain. It needs a
    `ScopedCredential` row in `src/core/SettingsCredentialPolicy.h`, beside
    `kIcomCredential`; `docs/agents/settings.md` keeps one credential table.
  - **The rest** of each engine's record is one Principle V settings object
    under one root key: its name, LAN address, certificate fingerprint and
    optional tailnet address.
  - **Without a keychain backend,** the token lives only in
    `AppSettings::setSessionCredential()`, the pairing lasts for the session,
    and the panel says it will not be remembered. Each launch then pairs
    again, and each `pairing.complete` mints a new credential on the radio. So
    the panel also says that the radio's Remote clients list keeps the old ones
    until they are revoked on the radio's screen.
- **Revocation from the desktop** is "Forget", which deletes the local token
  only; removing the client from the radio is done on the radio's screen.

## Hazards

- **Two controllers:** the backend never speaks Protocol 2 and never sends
  `radio.connect`. The raw ANAN entry for the same radio remains refused while
  in use (`ConnectionSharingPolicy.h`). The desktop's own ANAN/HL2 auto-connect
  must stay off for a radio its engine holds, as today.
- **A changed certificate** (the G2 reinstalled) fails a `wss://` connect
  closed with a message naming the fingerprint change; re-pairing on the LAN
  replaces it. The tailnet path has no certificate to catch this; see *Two
  paths*.
- **The engine restarts or goes away:** the WebSocket closes, the backend
  reports disconnected; reconnect is the existing automatic-reconnect path,
  with no busy probe needed (nothing on the radio is touched).

## Out of scope for slice 2

Spectrum and waterfall frames and audio (the data plane, above), transmit,
pairing over the tailnet, slice and panadapter creation (no methods yet),
`radio.connect`/`radio.disconnect`, remote engines over SmartLink.

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
  - **the rate (rule 3):** frequency intents paced at one per millisecond for
    3 s, against the real server limits (100/s, burst 200). The session must
    stay open throughout, the requests counted at the server must stay within
    the client bucket (burst + rate × elapsed) in every window, and the slice
    must end at the last value. A client without its bucket would send about
    1000 requests a second and be closed by the server within the first
    second, so this test fails if the cap is missing or above the server's;
  - the client bucket alone, socket-free with an injected clock: subscribes,
    re-reads and retries all draw on it, and its size follows `welcome.limits`
    or the fallback;
  - a conflicting change from a second session is retried with the latest
    intent (rule 3);
  - a forced resync and slice add/remove reach the model (rule 4);
  - a wrong certificate fingerprint fails before anything is sent;
  - `ws://` is refused before `hello` unless the local end is on a WireGuard
    interface (socket-free, with an injected interface list), and
    `pairing.complete` is never sent to a `ws://` address;
  - keying is refused and no TX capability is declared.
- **Seam:** the family joins `backend_seam_affinity_test` (rule 1 and a cold
  construct/teardown) and `backend_family_switch_test`.
- **Discovery:** the responder's probe format, reply contents, rate bound,
  source-address filter and WireGuard-interface filter, socket-free where
  possible plus one loopback probe.
- **Live:** this desktop against the G2, with the desktop's FLEX isolation and
  its ANAN/HL2 auto-connect off.
