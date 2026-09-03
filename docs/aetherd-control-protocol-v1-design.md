# AetherD control protocol v1 — implementation contract

**Status:** approved Stage 3 implementation contract for RFC #3849.  This
document narrows and makes executable the versioned-protocol portion of
[`aetherd-headless-engine-design.md`](aetherd-headless-engine-design.md).  It
does not change the accepted architecture or authorize Stage 4 transmit work.

## 1. Scope and non-goals

Version 1 provides a typed, transport-neutral control and observation surface
for `libaethercore`, usable in-process by the desktop client and over bounded
local or remote transports by `aetherd`.

It is not:

- the existing `AutomationServer`, which remains a desktop GUI test bridge;
- an installed, packaged, or auto-started daemon in this slice; `aetherd` is a
  build/test skeleton until the later service-lifecycle and packaging work;
- a QObject reflection API, widget tree, arbitrary method invocation surface,
  or pass-through for SmartSDR/CI-V/Metis command strings;
- a promise that every current GUI→engine header becomes a v1 resource;
- a transmit API.  TX intents remain unavailable until the Stage 4 arbiter and
  every keying path have migrated behind the same authorization point.

The authoritative inventory for migration is the generated
[`architecture/aetherd-touchpoints.md`](architecture/aetherd-touchpoints.md).
`architecture/aetherd-touchpoint-tags.json` decides whether each surface is a
canonical resource, a namespaced family extension, or client-only plumbing.

## 2. Component boundary

The implementation is split by responsibility:

| Component | Responsibility | Dependencies |
|---|---|---|
| `ControlProtocolCodec` | Parse, validate, and serialize protocol envelopes | QtCore only |
| `ControlMethodRegistry` | Closed registry of method names, schemas, grants, and handlers | QtCore + engine interfaces |
| `ControlResourceStore` | Typed snapshots, per-resource revisions, ordered change events | QtCore + models |
| `ControlSession` | One authenticated client, grants, subscriptions, quotas, and output queue | QtCore |
| `ControlService` | Session lifecycle and dispatch; no socket code | QtCore + the components above |
| Local transport | `QLocalServer`/`QLocalSocket` framing and same-user endpoint policy | QtNetwork |
| WebSocket transport | Optional authenticated remote framing, disabled by default | QtWebSockets + QtNetwork |
| Desktop adapter | In-process use of the same service and typed contracts | GUI may depend on it; service never depends on GUI |

The method registry is closed at compile time.  A request cannot name a C++
class, QObject, signal, slot, property, file, command string, or executable.
Family-specific operations use a reviewed namespace such as
`extension.hl2.frequencyCalibration.get`; they are still individually typed
registry entries.

## 3. JSON envelope

Text messages are UTF-8 JSON objects.  Local transport uses one JSON object per
newline.  WebSocket transport uses one JSON object per text frame.  Duplicate
object keys, non-finite numbers, invalid UTF-8, and trailing non-whitespace are
rejected.

### 3.1 Negotiation

The first client message is the only request without a session ID:

```json
{
  "v": 1,
  "id": "hello-1",
  "method": "hello",
  "params": {
    "client": {"name": "AetherSDR", "version": "26.8.3"},
    "versions": [1],
    "auth": {"scheme": "bearer", "token": "<redacted>"}
  }
}
```

The server answers with either an ordinary error or:

```json
{
  "v": 1,
  "id": "hello-1",
  "result": {
    "sessionId": "019d2d95-2c8b-7f2a-a17d-cc77031b3040",
    "version": 1,
    "server": {"name": "aetherd", "version": "26.8.3"},
    "grants": ["observe", "control"],
    "capabilities": ["radio.sessions", "slice.read", "slice.control"],
    "limits": {
      "maxMessageBytes": 262144
    }
  }
}
```

The initial `hello` always uses the baseline v1 envelope (`"v": 1`), including
when a client also supports future protocol versions.  Negotiable versions are
advertised only through `params.versions`; a pre-negotiation envelope with any
other `v` is invalid.  The server chooses the highest mutually supported
version from `params.versions`.  Unsupported advertised versions return
`protocol.version_unsupported` and close the connection.  The handshake must
complete within 5 seconds and no other method is accepted first.
Authentication material is write-only to the verifier: it is never included in
diagnostic objects, errors, metrics, settings, support bundles, or logs.

### 3.2 Requests and responses

After negotiation every request carries the assigned session:

```json
{
  "v": 1,
  "id": "req-42",
  "sessionId": "019d2d95-2c8b-7f2a-a17d-cc77031b3040",
  "method": "slice.setFrequency",
  "params": {"radioSession": "radio-1", "slice": "0", "hz": 14225000}
}
```

Success returns exactly one `result`; failure returns exactly one `error`:

```json
{"v":1,"id":"req-42","result":{"accepted":true}}
```

```json
{
  "v": 1,
  "id": "req-42",
  "error": {
    "code": "request.out_of_range",
    "message": "hz is outside the active radio's tunable ranges",
    "data": {"field": "hz"},
    "retryable": false
  }
}
```

`id` is a client-chosen string of 1–64 printable ASCII characters and must be
unique among that session's pending requests.  The server echoes it verbatim.
Errors raised before a valid request ID can be correlated (for example, a
handshake timeout or transport limit) omit `id` entirely.
Unknown top-level or parameter fields are rejected so a misspelled safety field
cannot be silently ignored.  Method success means the intent passed validation
and was accepted by the engine; authoritative state is established by the
subsequent resource revision, not an optimistic response payload.

### 3.3 Events

Server events have no request ID:

```json
{
  "v": 1,
  "sessionId": "019d2d95-2c8b-7f2a-a17d-cc77031b3040",
  "event": "resource.changed",
  "sequence": 184,
  "resource": {"type": "slice", "radioSession": "radio-1", "id": "0"},
  "revision": 27,
  "value": {
    "frequencyHz": 14225000,
    "mode": "USB",
    "filter": {"lowHz": 300, "highHz": 2700}
  }
}
```

`sequence` is monotonically increasing within a protocol session.  `revision`
is monotonically increasing for one resource identity and changes only when
its canonical serialized value changes.  V1 sends complete bounded resource
values rather than JSON Patch; this makes loss recovery and schema validation
unambiguous.

## 4. Resources, snapshots, and subscriptions

V1 begins with the smallest useful canonical set:

- `server` — build/protocol versions, health and transport state;
- `radioSession` — identity, family, connection state and capabilities;
- `slice` — frequency, mode, filter, receive controls and ownership;
- `panadapter` — center, bandwidth, dBm range and display cadence;
- `meter` — definitions and bounded latest values;
- `transmitState` — read-only interlock, PTT/VOX/MOX/tune state and inhibit
  reason.  No v1 method can mutate this resource during Stage 3.

Every field is documented in a method/resource catalogue and covered by a
schema test before it is exported.  Credentials, backend pointers, raw vendor
messages, filesystem paths, unbounded log text, and other clients' private
state are never resources.

`resource.subscribe` accepts explicit resource selectors and returns an atomic
baseline:

```json
{
  "subscription": "sub-7",
  "sequence": 183,
  "resources": [
    {"resource":{"type":"slice","radioSession":"radio-1","id":"0"},
     "revision":26,"value":{"frequencyHz":14224000,"mode":"USB"}}
  ]
}
```

The service registers the subscription, captures the snapshot, and queues all
changes after the returned `sequence` as one main-thread operation.  Therefore
there is no snapshot/event gap.  Reconnect creates a new protocol session;
clients resubscribe and replace their cache from a fresh snapshot.  V1 does not
promise event replay across connections.

High-rate spectrum, waterfall and audio payloads are not embedded in these
control JSON events.  Their later stream contract must use bounded binary
frames, explicit format/rate negotiation, and independent backpressure.

## 5. Methods and capability discovery

The registry exposes metadata for each method:

- name and protocol version introduced;
- required grant (`observe`, `control`, or `transmit`);
- request and result schema IDs;
- required radio capability and allowed connection states;
- whether the method is idempotent;
- fixed execution timeout.

Initial non-TX methods are `capabilities.get`, `resource.get`,
`resource.subscribe`, `resource.unsubscribe`, `radio.connect`,
`radio.disconnect`, `slice.setFrequency`, `slice.setMode`, `slice.setFilter`,
`slice.setAudioGain`, `slice.setAudioMute`, `panadapter.setCenter`, and
`panadapter.setBandwidth`.  Connect requests use typed family-neutral fields;
backend-specific connection data lives in a namespaced validated object and
never includes a persistent plaintext credential.

Advertised capabilities are the intersection of protocol support, build-time
features, authenticated grants, active backend capabilities, and current
engine state.  A client must not infer support from radio family names.

## 6. Authentication and grants

There are three independent grants:

- `observe` — snapshots and subscriptions;
- `control` — non-transmitting radio/session/slice/pan intents;
- `transmit` — keying or emission-capable intents, absent throughout Stage 3.

Local endpoints are created with current-user-only filesystem/ACL permissions.
Same-user local clients may be configured for `observe` and `control`; this
does not imply `transmit`.  Remote clients authenticate with an independently
generated high-entropy token stored only in the OS credential vault.  Tokens
map to explicit grants and can be revoked without changing the protocol.

The initial Stage 3 local endpoint authenticates implicitly through that
current-user boundary, so its `hello` omits `auth`. Until a remote transport
supplies a verifier, any supplied `auth` field is rejected with `auth.invalid`;
credentials are never accepted and ignored. The `auth` shape above is reserved
for a transport wired to the verifier described here.

Remote WebSocket serving is disabled by default.  When enabled it may bind to
loopback, an explicitly selected WireGuard interface, or a TLS endpoint with
certificate validation.  Wildcard/plain-LAN binding is rejected.  Origin
headers are not authentication.  Bearer credentials are accepted only over a
confidential transport or a current-user local endpoint.

## 7. Transmit actor and arbitration contract

Stage 3 exports read-only transmit state and no transmitting methods.  Stage 4
must satisfy all of the following before a `transmit` grant or verb can exist:

1. Every software path capable of RF emission—MOX/PTT, tune, ATU, CW/CWX,
   packet/APRS, CAT/rigctl, TCI, DAX/voice audio and family extensions—submits a
   typed TX intent before any vendor command or sample emission.
2. One engine-owned arbiter validates the grant, per-radio TX lock, interlock,
   current capability, expiry, and local operator override.
3. A protocol client never adopts hardware PTT or VOX.  Those are external
   operator actors and remain authoritative observations.
4. Local operator actions outrank remote clients and agents.  Emergency stop,
   disconnect, session expiry, auth revocation and client loss release the lock
   and issue a best-effort unkey through every active backend.
5. Restored state, reconnect, subscription replay, and retries cannot key TX.
6. Live-radio TX validation requires separate explicit operator authorization;
   ordinary Stage 3 tests are RX-only with TX disabled.

The lock holder is one authenticated protocol session, never merely a socket
address.  Acquisition is explicit, lease-bounded, non-transferable and audited
without recording credentials or transmitted content.

## 8. Limits and backpressure

The table below defines the complete v1 target defaults. A server advertises
only limits that apply to capabilities it currently exposes; clients must not
infer an unadvertised limit or capability. The initial observe-only slice
advertises `maxMessageBytes` and enforces the handshake, structural JSON,
client-count, and queued-output limits. Subscription, pending-request, and
request-rate limits become live and advertised only with those protocol
surfaces. Once advertised, a default is part of the v1 contract and may only
be tightened at runtime:

| Limit | Default |
|---|---:|
| Handshake time | 5 s |
| JSON message | 256 KiB |
| JSON nesting | 32 |
| String value | 64 KiB |
| Array entries | 4,096 |
| Concurrent clients | 8 |
| Subscriptions per client | 64 |
| Pending requests per client | 128 |
| Requests | 100/s, burst 200 |
| Queued output per client | 4 MiB |

Input is size-capped before JSON parsing.  Resource selectors and subscription
fan-out are validated before registration.  Coalescible telemetry keeps only
the newest resource revision; state transitions and request responses are not
dropped.  If the output limit is still exceeded, the service emits
`transport.backpressure` when possible and closes that client without blocking
the engine or other sessions.

All dispatch runs on the engine/model owning thread.  Transport callbacks only
frame and enqueue bounded work; they never mutate models from a socket thread.

## 9. Error registry

Error codes are stable machine-readable strings.  V1 reserves:

| Code | Meaning |
|---|---|
| `protocol.invalid_json` | Invalid UTF-8/JSON, duplicate key, or forbidden numeric value |
| `protocol.invalid_envelope` | Missing, unknown, or ill-typed envelope field |
| `protocol.version_unsupported` | No common protocol version |
| `auth.required` | Authentication missing |
| `auth.invalid` | Credential rejected |
| `auth.grant_denied` | Session lacks the method's grant |
| `request.unknown_method` | Method is not in the closed registry |
| `request.invalid_params` | Parameters fail the method schema |
| `request.out_of_range` | Typed value is outside its contextual bounds |
| `request.conflict` | State/revision/lock conflict |
| `resource.not_found` | Resource identity does not exist or is not visible |
| `capability.unavailable` | Build/backend/current state cannot perform the method |
| `session.invalid` | Session ID is absent, expired, or belongs to another connection |
| `transport.limit_exceeded` | A declared input/session limit was exceeded |
| `transport.backpressure` | Client cannot consume queued output |
| `engine.timeout` | Bounded engine operation did not complete |
| `engine.failed` | Operation failed without safely exposable backend detail |

Messages are diagnostic, not API.  They must not contain secrets, raw vendor
frames, absolute paths, or state belonging to another authenticated client.

## 10. Threat model and abuse cases

Protected assets are control of radio state, prevention of unintended RF
emission, radio and service credentials, operator privacy, availability of the
RX/audio engine, and isolation between protocol clients.

Trust boundaries are: client→transport, transport→codec, authenticated
session→method registry, registry→engine models, engine→vendor backend, and
process→credential vault/filesystem.  The principal abuse cases and required
controls are:

| Abuse case | Required control |
|---|---|
| Unauthenticated LAN client changes frequency or connects a radio | Remote disabled by default; confidential transport; token verification; explicit grants |
| Client invokes hidden C++/vendor functionality | Closed typed registry; no reflection or command-string pass-through |
| Oversized/nested JSON exhausts memory or parser time | Pre-parse byte cap, structural caps, request rate and pending limits |
| Slow subscriber stalls the radio/audio engine | Per-client queues, coalescing, hard output cap and disconnect |
| Snapshot race leaves client with stale authoritative state | Atomic subscribe/snapshot sequence and monotonic revisions |
| Error/log leaks a password or token | Vault-only persistence, redaction at ingress, stable safe errors |
| Compromised observer keys the radio | Separate grants; no Stage 3 TX verbs; Stage 4 single arbiter and lease |
| Remote client mistakes hardware PTT for its own lock | Physical PTT/VOX modeled as external operator actors |
| Reconnect/retry repeats a dangerous action | No TX during Stage 3; Stage 4 nonces/leases and no keying restoration |
| One client reads another client's private state | Resource visibility filter keyed by authenticated session |

Security defects in this boundary follow [`../SECURITY.md`](../SECURITY.md)
and are reported privately rather than demonstrated against public radios.

## 11. Implementation and acceptance sequence

1. **Contract and catalogue:** land this document, complete every current
   touchpoint tag, enforce generated-manifest freshness in CI, and add schema
   fixtures/tests for envelopes, errors and limits.
2. **Headless boundary:** remove all remaining QtWidgets dependencies from
   `aethercore`, split desktop-only resources, and add a `QCoreApplication`
   `aetherd` target with a CI assertion that it cannot link QtWidgets.
3. **Read-only service:** implement negotiation, auth, snapshots, revisions,
   subscriptions, limits and read-only resources over the local transport.
4. **Non-TX control:** add authenticated typed connect/slice/pan methods and
   prove authoritative echo behavior across every supported backend, currently
   Flex, HL2, Icom, Sim, ANAN and RTL-SDR.
5. **Stage 4 TX funnel:** migrate every emission-capable path to the arbiter,
   with deterministic denial/unkey tests and TX disabled in normal automation.
6. **Remote and packaging:** enable the bounded WebSocket transport only after
   authentication, isolation, backpressure and Stage 4 safety tests pass.

Stage 3 is complete only when the desktop can consume the same typed service,
`aetherd` runs without QtWidgets, schema/negative/backpressure tests pass, RX
slice 0 is proven unchanged, and no TX method is advertised.
