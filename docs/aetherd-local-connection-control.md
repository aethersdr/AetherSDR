# AetherD v1 — local connection control

This RFC #3849 connection interface carries non-TX connection intents.
Transmit grants and remote-client authentication have separate control boundaries.
The subsequent [slice frequency slice](aetherd-local-slice-frequency-control.md)
adds frequency tuning, and the [receive-control milestone](aetherd-local-receive-control.md)
adds qualified mode/filter/audio/geometry controls under the same explicit opt-in. The default daemon
remains observe-only.

## Authorization and startup

`--allow-local-control` explicitly grants **every client admitted by the
current-user local endpoint** both `observe` and `control`. It is not
per-application consent or a remote credential verifier. Without it, clients
retain only `observe`. Credential verification and transmit-grant administration
are separate from this connection interface; receive control never implies TX authority.

This flag does not discover or connect radios. The existing `--discover-local`
and `--discover-sim` flags retain their independent meanings. Native discovery
and explicit SmartLink startup load settings before model construction; passive
and simulator-only startup do not load the store. Model construction can still initialize settings paths.
The daemon binds its endpoint before either step, then binds the connection
target once before request dispatch. A failed listen never constructs models
or touches settings. Icom manual setup and external directories remain excluded.
SmartLink is selected only by the local startup options described below, not by
`radio.connect` or the public catalogue.

Endpoint reservation is not protocol readiness. During initialization, early
connections are closed without admitting a session; clients may reconnect and
retry `hello`. Serving begins only after all targets and initial resources are
installed. Settings/model constructors may pump nested Qt event loops, but an
early client must not cause startup binding to fail or observe partial state.
`local_control_server_test` pins this ordering against the production server:
a client arriving on a reserved endpoint is closed without a reply, serving
begins exactly once, and the default listen mode still admits sessions.

## Methods

Both require a negotiated, non-revoked control session and its own `sessionId`.
Authorization precedes parameter validation and resource lookup. Unknown fields
are rejected. Success returns `{"accepted":true}`: intent accepted, not radio
setup completed. Observe resource events for authoritative state.

### `radio.connect`

```json
{"radioSession":"radio-1","radioId":"<64-character catalogue entry id>","catalogueRevision":12}
```

The catalogue must be running and its revision must match. The revision is a
positive exact JSON integer, at most 2^53-1. Refresh and reselect after
`request.conflict`; do not automatically retry an old selection. Any catalogue
change, including an unrelated entry update, invalidates the selection.

The engine target must be idle, the identity must still exist, and `inUse`
must be false. Busy observations are advisory, not exclusive ownership; no
takeover is requested. Clients cannot override address, port, family,
credentials, raw commands or a force flag. The server resolves and copies
validated catalogue data on its owning thread; native RadioInfo translation
stays below the vendor boundary. Supported combinations are Flex/HL2/ANAN over
LAN, available RTL-SDR over USB, and the exact built-in simulator identity.

Connecting, connected and disconnecting targets reject replacement. Connect
is not idempotent across completed lifecycles: request IDs correlate responses,
not persistent deduplication. No wire retry/replay or automatic reconnect exists.

### `radio.disconnect`

```json
{"radioSession":"radio-1"}
```

Any authorized controller can cancel a connect or disconnect the shared engine
session, regardless of who connected it. Requests are idempotent while idle or
disconnecting. Client/grant loss does not implicitly disconnect this shared
non-TX session; already accepted intents are not retroactively undone.
Revocation blocks all subsequent dispatch from that client.

## Lifecycle and observation

An installed connection target adds `connectionControl` to `radioSession.value`:

```json
{"state":"connecting","errorCode":""}
```

States are `idle`, `connecting`, `connected`, and `disconnecting`. Error codes
are empty, `engine.failed`, or `engine.timeout`, never raw backend errors. The
code is the last lifecycle error and persists through `idle` until the next
accepted `radio.connect` clears it; the `disconnecting` observation carries it
first.
Existing connected/identity/capability fields remain model-authoritative.
Transport connection can precede slice enumeration; wait for required resources
instead of inferring RX readiness from the response or connection event alone.

An attempt has a 30-second cancellation timer. Error or unexpected disconnect
also cancels legacy reconnect work. Teardown is deferred out of model callbacks
and retains the busy reservation until queued cleanup settles. The timer bounds
the wait **before requesting cancellation**, not existing backend teardown
execution: some backends clean up synchronously. Unfinished teardown must not
permit replacement. The daemon owns the target and its otherwise idle model;
it never attaches to the desktop session or starts TCI/CAT servers.

## Capabilities, limits and verification

Grants describe authorization, not current state. Controllers see
`radio.connect` only with an idle target and an available supported entry;
`radio.disconnect` appears while the target is non-idle. Refresh capabilities
after lifecycle/catalogue events. The separately bound frequency target can
add `slice.setFrequency` for an eligible existing slice; no TX verbs appear.

Every post-negotiation frame (including malformed/rejected requests and repeated
`hello`) shares a per-client monotonic token bucket of 100/s, burst 200,
advertised as `requestsPerSecond` and `requestBurst`. Exhaustion is terminal:
it returns `transport.limit_exceeded` without an `id` and requests closure;
later frames cannot resume dispatch. Other clients retain independent budgets.
Dispatch requires the engine owning thread.

`control_connection_test` injects normalized discovery and a socket-free target
into the real service. It covers grant matrices, revocation, mixed session IDs,
schema and stale-selection failures, lifecycle conflicts, target loss, rate
isolation and absent TX. Native firmware convergence requires separate hardware
validation; the daemon integration smoke uses only the production simulator.

## Explicit SmartLink startup

`--allow-local-control --smartlink-account ACCOUNT_ID --smartlink-radio SERIAL`
reserves the same connection target before accessing the selected saved account.
`--list-smartlink-accounts` lists IDs and labels without reading vault secrets.
The radio must appear in that account's current SmartLink inventory; no endpoint
or token can be supplied by a control client. Startup does not alter catalogue
IDs or add a remote `radio.connect` route. Existing resource/control clients can
use the connected receiver. Failure requires fresh local startup intent, and
certificate mismatches fail closed. WAN does not acquire headless TX eligibility.

The host must run as the account's operating-system user with access to the
existing OS vault. On Windows, launch from that user's signed-in desktop
session; an SSH or service logon can lack credential decryption access even for
the same username. An unavailable vault is a startup failure, not permission
to copy tokens into settings or use an insecure storage fallback.
