# AetherD control protocol v1 — observe-only resource catalogue

This catalogue fixes the schema implemented by the first read-only Stage 3
slice of RFC #3849. It supplements
[`aetherd-control-protocol-v1-design.md`](aetherd-control-protocol-v1-design.md);
the envelope, limits, errors, authentication, and TX rules in that document
remain normative.

The seven resource types below are observational. No method in this catalogue
mutates a model or can reach a backend intent. Typed receive intents are
specified separately in [local receive control](aetherd-local-receive-control.md).

## Resource identities and selectors

An exact identity has one of these shapes:

```json
{"type":"server"}
{"type":"radioCatalogue"}
{"type":"radioSession","id":"radio-1"}
{"type":"slice","radioSession":"radio-1","id":"0"}
{"type":"panadapter","radioSession":"radio-1","id":"0x40000000"}
{"type":"meter","radioSession":"radio-1","id":"0"}
{"type":"transmitState","radioSession":"radio-1"}
```

`resource.get` requires an exact identity. `resource.subscribe` also accepts
an omitted `id` as an all-current-and-future selector for `radioSession`,
`slice`, `panadapter`, or `meter`; these children still require
`radioSession`. Unknown fields and unsupported resource types are rejected.
`server` and `radioCatalogue` are singletons: neither accepts `id` or
`radioSession`, including empty or null values.
`transmitState` is one singleton per radio session: `radioSession` is required
and `id` is forbidden.

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

A baseline exceeding the 256 KiB message limit (including reserved envelope
space) returns `transport.limit_exceeded` before installing any subscription.
Use narrower selectors; no partial baseline or silent truncation is returned.

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

### `radioCatalogue`

The daemon owns this singleton through `RadioCatalogue`. It is readable with
the same `observe` grant as the other resources; `radioCatalogue.read` is
advertised only while an adapter has published the singleton. Other service
embedders need not provide a discovery source. No new wire method is added.

- `running`: the catalogue's started/not-stopped lifecycle, **not** a claim
  that every native source successfully bound a socket or found a device.
- `sources`: sorted enabled source families. This records startup configuration
  and build availability, not scan health or the families currently visible.
- `limited`: a valid new identity was dropped at capacity. It stays true until
  a new catalogue instance is created, even after entries are lost or stopped.
- `maxEntries`: 64.
- `entries`: complete list, sorted by family then serial, of objects with:
  - `id`: opaque stable identity derived from family plus serial; independent
    of address, nickname, and arrival order. It is not a resource selector or
    a connection authorization. Native sources with no unique serial (such as
    RTL-SDR's USB-index fallback) retain their existing identity limitations.
  - `family`: 1–16 ASCII lowercase letters/digits, starting with a letter.
  - `serial`: nonempty, at most 128 UTF-16 code units.
  - `name`, `model`, `nickname`, `version`: display observations, each at most
    128 UTF-16 code units; empty means unavailable. Native discovery's existing
    client-owned nickname behavior is retained for families that use it. Which
    of these a family populates differs — a Flex publishes an empty `name`, and
    the simulator an empty `nickname` — so a client renders the first nonempty
    of `nickname`, `name`, `model`, and falls back to `serial`.
  - `transport`: `lan`, `usb`, or `sim`.
  - `address`, `port`: numeric IP address (at most 64 code units) and port
    1–65535 for LAN; empty address and zero port for USB/simulator.
  - `inUse`: native discovery's advisory busy observation, not ownership or a
    control grant.

Text must be valid UTF-16 without NUL or Unicode control characters. Invalid
observations are discarded, not truncated into potentially colliding identities.
No credentials, raw packets, connected-client identities, or saved connection
entries are projected. The entry and string bounds keep the complete catalogue
within the existing message limit. Unchanged duplicate announcements consume
no revision; a changed value or loss publishes the complete new catalogue.
Existing identities can still update at capacity, and newly freed capacity can
accept another observation. `limited` discloses that the list may be incomplete.

Daemon startup is passive by default (`sources: []`, `entries: []`).
`--discover-local` explicitly enables existing Flex/HL2/ANAN LAN discovery and
RTL-SDR USB enumeration when built in. `--discover-sim` independently publishes
the existing simulator identity without constructing a radio backend or scanning
LAN/USB. The flags may be combined. No discovered radio is connected. Icom's
manual address/credentials, SmartLink and external directories are outside this
slice; desktop discovery and autoconnect remain unchanged.

Only `--discover-local` initializes the daemon's `AppSettings` store, before
model/discovery consumers are constructed, so client-owned HL2/ANAN Identity
nicknames remain available. This uses the normal settings load, migration,
recovery and read-only protections; it does not create discovery entries from
saved configuration. The flag is not inert against the store, though: the
daemon and the desktop share one store, so on a machine where the desktop has
never run, `--discover-local` is what creates `AetherSDR.db` and claims the
one-shot legacy migration. The store is loaded only after the daemon owns its
local endpoint, so a daemon that fails to listen never touches it. Passive and
simulator-only startup do not load the store at all.

One adapter/source instance has one lifecycle. `start()` is idempotent;
`stop()` is terminal, clears observations and ignores late callbacks. Disposal
removes the singleton. These are internal lifecycle calls, never protocol
commands. Create a new instance to restart discovery.

### `radioSession`

- `id`, `connected`, `family`.
- `meterDelivery`: `maxEntries` (64), `publishIntervalMs` (100),
  `staleAfterMs` (2000), and `limited`. The latter stays true until meter clear
  or connection reset after an invalid/over-capacity definition is excluded.
- `identity`: `name`, `model`, `serial`, `version`, `manufacturer`.
- `capabilities`:
  - `maxSlices`, `maxPanadapters`, `sampleRatesHz`;
  - `tuningRangeHz` with `minimum` and `maximum`;
  - `declaredBands`, each with `name`, `lowHz`, and `highHz`;
  - `sliceFrequencyControl` with `authority` (`radio`, `engine`, `unknown`),
    `minimumHz` and `maximumHz`; zero bounds mean unavailable, not unlimited;
  - `canTransmit`, `maximumTransmitWatts`, `hasTuner`, `hasAmplifier`;
  - `extensions`, containing namespace names only, never extension payloads.
  - Optional `receiveModeControl`, `receiveFilterControl`, `receiveAudioControl`,
    `receivePanCenterControl`, `receivePanBandwidthControl`: null when
    unqualified. Records carry `authority` (`radio`, `engine`, `unknown`). Mode
    carries `modes`; filter carries per-mode minimum/maximum low cut, high cut
    and width; audio carries gain bounds 0–100; pan records carry inclusive
    `minimumHz`/`maximumHz`. See the receive-control contract for eligibility.

`canTransmit` is observation only. It does not advertise a protocol TX method
or grant and cannot key a radio.

When the optional local connection target is installed, `connectionControl`
adds bounded `state` and `errorCode` observations. Their schema and the separate
control methods are specified in
[`aetherd-local-connection-control.md`](aetherd-local-connection-control.md).

### `slice`

- `id`, `letter`, `panadapterId`, `owned`.
- `frequencyHz`, `mode`, `filter.lowHz`, `filter.highHz`.
- `frequencyObservation.known`, `.hz`, `.authority`: last backend publication,
  with `hz:null` while unknown. Authority is `radio`, `engine` or `unknown`;
  engine configuration is not a hardware acknowledgement or DSP completion.
- `active`, `txSlice`, `locked`.
- `audio.gain`, `audio.pan`, `audio.muted`.
- `receiveObservation.mode` and `receiveObservation.audio.gain` / `.muted`:
  `{known, value, authority}` with null value when unknown. Mode is a string,
  gain an integer 0–100, mute a boolean. `receiveObservation.filter` carries
  `known`, nullable `lowHz`/`highHz`, and `authority`; both cuts and a mode are
  required for a known ordered passband. These are backend publications,
  separate from the legacy optimistic mode/filter/audio values above. A changed
  mode invalidates previous cuts; partial fields do not invent missing partners.
- `receive.antenna`, `receive.rfGain`.
- `receive.agc.mode`, `receive.agc.threshold`, `receive.agc.offLevel`.
- `receive.squelch.enabled`, `receive.squelch.level`.

Values come from `SliceModel`; radio/backend status remains authoritative.
`frequencyHz` retains the existing effective (potentially optimistic desktop)
value. The separate observation is invalidated across disconnect/reconnect and
updates even when a backend echo equals that effective value. See
[`aetherd-local-slice-frequency-control.md`](aetherd-local-slice-frequency-control.md)
for control admission, observation and concurrent-intent semantics.

### `panadapter`

- `id`, `owned` (exact current-session ownership, unknown is false).
- `geometryObservation`: nullable `centerHz`/`bandwidthHz`, `centerKnown` /
  `bandwidthKnown`, `centerAuthority` / `bandwidthAuthority`. Only normalized
  backend geometry updates these fields. Reconnect or an ownership change
  invalidates them, including when legacy visible geometry is retained.
- `centerHz`, `centerKnown`, `bandwidthHz`.
- `dbmRange.minimum`, `dbmRange.maximum`.
- `bandwidthLimitsHz.minimum`, `bandwidthLimitsHz.maximum`; zero means the
  backend has not reported a limit.
- `receive.antenna`, `receive.rfGain`.
- `displayCadence.fps`, `displayCadence.averageFrames`: effective model values.
- `displayCadence.fpsIsRequest`, `displayCadence.averageIsRequest`: true when
  the effective value is dispatched intent awaiting a subsequent radio publication.
- `displayCadence.radioReportedFps`, `displayCadence.radioReportedAverage`: last
  radio publications, or `-1` before any publication. Same-value requests and
  confirmations also update provenance; later radio publications always win.
- `displayCadence.weightedAverage`, `weightedAverageKnown`.
- `displayCadence.waterfallRate`; `-1` means the backend has not reported a
  value, otherwise this is the normalized 1–100 rate, not milliseconds.

### `meter`

At most 64 valid observed definitions per session are projected; new identities
at capacity are dropped and `radioSession.meterDelivery.limited` discloses the
incomplete view. Existing identities continue updating. Removal or invalidation
of a selected definition fills the vacancy from remaining valid definitions in
index order, without importing retained samples. Initial admission skips invalid
definitions before reaching the 64-entry bound. The limited flag remains latched
until clear/reconnect. This is a latest-value diagnostic projection, not lossless metering.

- `id`: decimal normalized meter index (0–65535).
- `definition`: `source`, `sourceIndex`, `name`, `unit`, `minimum`, `maximum`,
  `description`. Source/name are nonempty and bounded to 32 UTF-16 code units;
  unit to 16 and description to 128. Text must be valid UTF-16 without NUL or
  Unicode control or format characters (including supplementary code points).
  Bounds must be finite and ordered. Invalid definitions are
  excluded, not truncated. No raw frames or proprietary extension payloads.
- `sample`: `known` (a sample was observed in this connection), `fresh` (at most
  2000 ms old), `valid`, `ageMs`, `value`. Age is monotonic, -1 before a sample,
  capped at 2001 once stale so an idle resource stops churning. `value` is a
  finite converted physical number in the definition's units only when valid;
  otherwise null. SWR additionally uses MeterModel's existing sample/power
  validity gate; stale/inapplicable SWR is never a healthy-looking ratio.

The adapter retains one latest sample per admitted meter, publishes at most
every 100 ms, and refreshes liveness on unchanged samples. Metadata changes
invalidate old samples (including units); identical definitions do not. Clear,
disconnect and backend replacement remove resources and discard samples. A new
connection needs new samples, even if a legacy model retained an old value.
Meter updates never touch slice/pan command revisions. Existing per-client
coalescing, resync and hard transport caps still apply.

### `transmitState`

- `connected`, `canTransmit`: backend/session observations only, never grants.
- `state`: `unknown`, `unsupported` (connected RX-only backend), `idle`, or
  `transmitting`. Only explicit normalized transmit readback establishes idle
  or transmitting. Local command activity, backend/capability changes and
  disconnect invalidate prior idle knowledge. False construction defaults or a
  falling command edge cannot establish idle. Many backends currently have no
  normalized readback and correctly remain unknown.
- `localRequests`: `mox`, `tune`, `transmitting`. These are explicitly local
  model/request values, not evidence of RF, physical PTT or idle state.

This resource is readable with `observe`, including on an observe-only daemon.
It introduces no TX verb, grant, lease, policy decision or safety guarantee.

FFT bins, waterfall rows, audio, and other high-rate streams never enter these
JSON resources; they belong to the later bounded binary data plane.
