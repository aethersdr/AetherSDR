# AetherD control protocol v1 — observe-only resource catalogue

This catalogue fixes the schema implemented by the first read-only Stage 3
slice of RFC #3849. It supplements
[`aetherd-control-protocol-v1-design.md`](aetherd-control-protocol-v1-design.md);
the envelope, limits, errors, authentication, and TX rules in that document
remain normative.

Only the four resource types below exist in this slice. `meter` and
`transmitState` remain unimplemented. No method in this catalogue mutates a
model or can reach a radio backend intent.

## Resource identities and selectors

An exact identity has one of these shapes:

```json
{"type":"server"}
{"type":"radioSession","id":"radio-1"}
{"type":"slice","radioSession":"radio-1","id":"0"}
{"type":"panadapter","radioSession":"radio-1","id":"0x40000000"}
```

`resource.get` requires an exact identity. `resource.subscribe` also accepts
an omitted `id` as an all-current-and-future selector for `radioSession`,
`slice`, or `panadapter`; `slice` and `panadapter` still require
`radioSession`. Unknown fields and unsupported resource types are rejected.

## Methods

All methods require the negotiated session ID and an active `observe` grant.
The current-user local endpoint supplies observer authorization when it creates
each session. The transport-neutral service defaults to unauthenticated and
refuses negotiation without trusted authorization. Authentication without the
observe grant permits `capabilities.get` only; resource methods return
`auth.grant_denied` before checking parameters or looking up resources.

Revocation is terminal for that session: subscriptions and pending frames are
discarded, future resource events are suppressed, and the local transport
aborts its socket and unwritten output. Bytes already delivered cannot be
recalled. The client must establish a new authorized connection and negotiate
and subscribe again. A `hello` on a revoked session cannot restore access.

### `resource.get`

Parameters:

```json
{"resource":{"type":"slice","radioSession":"radio-1","id":"0"}}
```

Result:

```json
{
  "resource":{"type":"slice","radioSession":"radio-1","id":"0"},
  "revision":3,
  "value":{}
}
```

The complete typed value occupies `value`. A missing exact identity returns
`resource.not_found`.

### `resource.subscribe`

Parameters contain 1–64 selectors:

```json
{"resources":[{"type":"slice","radioSession":"radio-1"}]}
```

The result contains a session-local subscription ID, the last session event
sequence already drained to the transport, and the complete baseline matching
those selectors. Registration and snapshot capture execute as one main-thread
operation. Events still pending for existing subscriptions retain sequences
greater than the returned boundary, and newly generated events advance beyond
them, so an event delivered after the baseline cannot leave a snapshot/event
gap or reuse the baseline sequence.

### `resource.unsubscribe`

Parameters are `{"subscription":"sub-1"}`. Success returns the same ID and
`"removed":true`. An unknown ID returns `resource.not_found`.

## Events, revisions, and resync

`resource.changed` carries the complete new value. `resource.removed` carries
the identity and its next revision but no value. Revisions come from one
store-wide monotonic counter. They are therefore monotonic per identity and
survive removal/recreation. A revision is consumed only when a canonical value
changes or a live identity is removed, but an identity's revisions need not be
consecutive or begin at one.

Event `sequence` is monotonic within one protocol session. Pending events for
the same resource coalesce to the newest sequence, revision, and complete value.
Sequences may therefore have gaps; they never move backward.

If a session's bounded event queue cannot retain its subscribed state, the
service clears that session's subscriptions and emits:

```json
{
  "v":1,
  "sessionId":"...",
  "event":"resource.resyncRequired",
  "sequence":42,
  "subscriptionsInvalidated":true
}
```

The client must call `resource.subscribe` again and replace its cache from the
fresh baseline. The current-user local transport also enforces a hard
socket-output cap. That cap is a separate *check* — the session's pending queue
against the operating-system socket buffer — but not a separate *budget*: both
are the same `maxQueuedOutputBytes` figure advertised in the handshake, so a
client should budget that figure once, not twice. A client whose socket buffer
is already at the cap can be disconnected before a queued resync notice is
written; after reconnecting it must establish a new session and baseline.

## Resource values

### `server`

- `name`: server product name.
- `buildVersion`: AetherSDR build version.
- `protocolVersions`: supported protocol versions.
- `health`: bounded service health token.
- `localTransport`: `idle`, `listening`, or `stopped`. `idle` and `stopped`
  describe in-process lifecycle state before or after socket availability; a
  protocol client can query this resource only while the value is `listening`.

No endpoint path, process environment, hostname, or filesystem value is
exported.

### `radioSession`

- `id`, `connected`, `family`.
- `identity`: `name`, `model`, `serial`, `version`, `manufacturer`.
- `capabilities`:
  - `maxSlices`, `maxPanadapters`, `sampleRatesHz`;
  - `tuningRangeHz` with `minimum` and `maximum`;
  - `declaredBands`, each with `name`, `lowHz`, and `highHz`;
  - `canTransmit`, `maximumTransmitWatts`, `hasTuner`, `hasAmplifier`;
  - `extensions`, containing namespace names only, never extension payloads.

`canTransmit` is observation only. It does not advertise a protocol TX method
or grant and cannot key a radio.

### `slice`

- `id`, `letter`, `panadapterId`, `owned`.
- `frequencyHz`, `mode`, `filter.lowHz`, `filter.highHz`.
- `active`, `txSlice`, `locked`.
- `audio.gain`, `audio.pan`, `audio.muted`.
- `receive.antenna`, `receive.rfGain`.
- `receive.agc.mode`, `receive.agc.threshold`, `receive.agc.offLevel`.
- `receive.squelch.enabled`, `receive.squelch.level`.

Values come from `SliceModel`; radio/backend status remains authoritative.

### `panadapter`

- `id`.
- `centerHz`, `centerKnown`, `bandwidthHz`.
- `dbmRange.minimum`, `dbmRange.maximum`.
- `bandwidthLimitsHz.minimum`, `bandwidthLimitsHz.maximum`; zero means the
  backend has not reported a limit.
- `receive.antenna`, `receive.rfGain`.
- `displayCadence.fps`, `displayCadence.averageFrames`.
- `displayCadence.weightedAverage`, `weightedAverageKnown`.
- `displayCadence.waterfallRate`; `-1` means the backend has not reported a
  value, otherwise this is the normalized 1–100 rate, not milliseconds.

FFT bins, waterfall rows, audio, and other high-rate data never enter these
JSON resources; they belong to the later bounded binary data plane.
