# Stage 4 client grants and local transmit control

Tracking: #3849 and #5690. The local-only implementation composes credentials,
explicit live grants, the existing arbiter, and operation-bound backend stop
evidence. It is disabled unless `--allow-local-tx`, `--allow-local-control` and
`--credential-authority` are all selected. Startup never arms a client. Existing
desktop operation remains under its compatibility actor. The software-PTT path
supports compatible Flex radios over LAN using SmartSDR TCP API 1.4 and complete
live interlock evidence; it is not restricted to a model or firmware build.
Hardware verification was performed on FLEX-6700 firmware 4.2.18.41174, not on
every compatible model. See the separate support contract and evidence record
in [Flex PTT stop evidence](aetherd-flex-ptt-stop-evidence.md).

## Authority and lifetime

`TxGrantManager` shares the radio's existing `TxCoordinator`; it is not another
ownership lock. Trusted engine composition supplies an already authenticated
principal when registering a client and explicitly issues a bounded policy.
Neither this API nor a client name performs authentication. The production
`TransmitControlService` enforces the separate grant-admin role described below.

## Explicit local credentials, not automatic arming

`ControlCredentials` verifies an optional bearer identity for a local session.
The canonical token is 32 random bytes encoded as exactly 64 lowercase hexadecimal
characters. `hello.auth` contains only `scheme: "bearer"` and `token`; client
names and claimed roles are never authority. The verifier keeps SHA-256 digests,
checks every bounded record, and binds an opaque principal to the negotiated
connection. A supplied invalid credential closes the connection; it never falls
back to implicit current-user observe/control access.

`client` and `grant-admin` are separate credential roles. Neither authenticating
nor creating an administrative credential issues a TX grant, reserves ownership,
or keys the radio. The existing `--allow-local-control` meaning is unchanged.
Ordinary current-user observation remains available without a credential; a
credential is required for independent TX/admin composition, not for
legacy local observation.

The daemon supports these explicit **offline** operator actions:

```sh
aetherd --initialize-credentials
aetherd --credential-authority <authority-id> --add-credential client
aetherd --credential-authority <authority-id> --add-credential grant-admin
aetherd --credential-authority <authority-id> --list-credentials
aetherd --credential-authority <authority-id> --remove-credential <credential-id>
```

Initialization generates a nonsecret 32-character authority ID and one grant-admin
identity. It refuses to overwrite any existing authority, including an empty
one. Subsequent actions require the selected authority; IDs and roles are the
only output. Secrets are never accepted in argv or exported to stdout, ordinary
settings or files. The initial local-client integration will retrieve its selected
credential through the native-vault adapter; no token export command is provided.
Combining an offline action with discovery/control/TX or another action is rejected
before storage access or radio construction.

Serving with `--credential-authority <authority-id>` only loads existing records
and enables their verifier; it never creates or writes credentials. Missing,
denied, malformed or unavailable storage refuses that explicitly selected startup
before clients are admitted. Without the option, observe/control behavior is
unchanged and `hello.auth` is still rejected. Credential verification alone
does not enable any daemon TX methods.

Authentication and control permission remain separate: explicitly selecting an
authority without `--allow-local-control` permits credential verification and
negotiation, not receive mutations or TX. `capabilities.get` describes only the
session's existing grants; the credential itself adds no observe/control grant.
The daemon's ordinary current-user observe permission remains in effect, and
TX/admin services are constructed only with the separate TX/control opt-ins.

Both serving and offline administration reserve the same current-user authority
lock, independently of the socket name, for their complete lifetime. Stop the
daemon before adding/removing credentials: a second endpoint cannot mutate the
vault while the first process keeps old verified identities alive. The lock and
OS-vault access are coordination within the current OS account, not isolation
against another compromised process in that account.

`ControlCredentialVault` uses QtKeychain's native store with insecure fallback
explicitly disabled. Its bounded binary record set accepts at most 32 distinct
identities and secrets. No supported vault means no credential feature, not a
plaintext fallback. One provisioner performs read-modify-write-readback under
the reservation and reports success only when readback matches exactly. If a
write is submitted but confirmation/readback fails, its durable outcome may be
uncertain: inspect with `--list-credentials` after resolving the storage problem;
the tool does not blindly roll back. Closing a native credential prompt or
terminating setup never creates a live TX grant.

### Credential memory limits

Credential persistence is restricted to the native vault with insecure fallback
disabled; this increment does **not** guarantee secure erasure of process memory.
Vault records, QtKeychain jobs, bearer-token strings, JSON serialization and local
socket buffers can retain raw secrets or encoded tokens in transient heap copies.
`QByteArray::clear()` releases a reference, not a guaranteed zeroization. Clearing
one visible buffer would not erase those other copies. The serving verifier keeps
digests, but loading and authenticating still materialize transient secret bytes.

Comprehensive memory scrubbing, locked/nonpageable secret buffers and protection
against process-memory/crash-dump inspection are outside this increment's scope.
Treat dumps of credential-using processes as potentially secret-bearing; do not
publish them unredacted. This limit does not permit credentials in application
logs, ordinary settings, exported support data or a plaintext storage fallback.

On macOS, only runs requesting credential initialization or a credential
authority select the CoreFoundation main-loop dispatcher needed by QtKeychain.
Ordinary observe/control runs retain their default dispatcher. The temporary
selection is restored after application construction so worker defaults and an
existing dispatcher environment setting are preserved.

### Credential and grant retirement

Credential retirement invalidates atomic principal handles **before** notifying
sessions. Each granted actor captures that immutable credential fence, so worker
keying/media requests are already refused even if an earlier revocation listener
re-enters before the session callback. Matching stop-only cleanup remains valid.
Reloading the vault retires all old principal handles even if their records are
unchanged; revoking one record does not retire unrelated clients. Session teardown
then synchronously cancels that connection's grants through its bound lifetime.

Client and grant handles are opaque, in-memory objects. A grant belongs to one
manager, client connection and coordinator radio-session generation. Its actor
has an immutable absolute deadline, operation budget and activity mask. Its
producer is bound to that actor. Disconnect/revocation retires only the affected
client's authority; radio reset invalidates every independent actor. Old handles
cannot be reused after renewal, reconnect or reset.

The manager accepts at most eight TX clients and one live grant per client.
If those optional registrations are full, a valid credential hello still
negotiates with its existing observe/control permissions, but no `tx.*` methods,
TX client ID or grant. Freeing a slot does not automatically register that
connection; a fresh connection/hello is required. Credential revocation still
terminates the unregistered session. Administrator sessions consume no TX client
slot and remain available to revoke grants or stop operations.
Issuance requires explicit positive durations, bounded by internal ceilings:
one hour per operation, 24 hours per grant and 60 seconds per keepalive interval.
These are protocol ceilings, not defaults; all three durations are explicit.
The coordinator's existing actor, producer, request and intent limits still
apply. Revoked registrations are reclaimed even while stale handles are retained.

Three clocks serve different purposes in this implementation:

- The operation budget starts at acquisition and is not extended by repeats.
- The absolute grant deadline also expires idle grants and spans multiple bursts.
- The keepalive deadline measures client liveness, not operator presence.

A precise single-shot engine timer schedules the earliest deadline. The actual
worker-side command/media fences independently check the same monotonic clock,
so a late timer wake cannot admit an expired write. Keepalive cannot renew an
expired grant or extend its absolute lifetime. An operator-presence policy is
not implemented and must not be inferred from traffic.

Acquisition consumes a grant-scoped increasing intent serial even when refused
as busy or recovering. Retrying that old intent cannot turn into a delayed
transmission. This engine API is not a wire idempotency-result cache. Release
and cancel require the captured operation as well as the original client and
grant, preventing an old stop from cancelling a later operation under that grant.

Revocation fences producers before callbacks. Actor/producer/operation binding
is checked together; an invalid captured request never falls back to the desktop
actor. Independent actors cannot borrow the desktop continuous-microphone
privilege, and an already entered microphone writer prevents independent
admission. That desktop media path stays fenced while independent ownership or
its unresolved stop remains retained.

## Independent model and backend-media binding

`TxController::fromGrantedPtt()` wraps a manager-acquired MOX request and its
matching operation for the same radio. It cannot issue a grant, relabel desktop
authority or another activity, or key a backend by itself. The manager retains
the authority lifetime; the returned input cannot derive new program intents.

Starting the input traverses the existing `TransmitModel` and `RadioModel`
preflight, including transmit capability, receive-only mode, panadapter inhibit
and reentrant cancellation. Independent backend-generated media carries that
captured input's actor/operation context, never the desktop backend producer.
The compatible desktop multi-contributor media path remains unchanged.

Socket-free model tests exercise positive injected handoff, disconnect before
queued keying, revocation during admission, retained input/media fencing,
backend replacement and late cleanup from the previous client/session. These
tests do not qualify a production backend or expose any new wire method.

## Terminal control-session lifetime and fair input processing

`ControlSession::bindAuthorityLifetime()` is trusted engine wiring, not a
credential verifier or a grant factory. It binds one context-owned retirement
callback to the session. `endAuthorization()` makes the session terminal,
clears pending observations and runs that callback synchronously. Final protocol
errors may then drain before closing the transport. Explicit revocation also
aborts transport-owned output. Late binding cannot miss a prior termination,
and repeated end/revoke/destruction retires a bound lifetime only once.

The local server calls this terminal path on disconnect, handshake timeout,
input/output hard limits, write failure and shutdown before deferring client
destruction. Destroying a session or its bound output transport ends authority
too. An undeliverable resync notice is terminal immediately; its retirement no
longer waits for a queued socket-abort callback. Ordinary recoverable request
errors do not end an otherwise usable connection.

The production `ControlInputPump` separates framing and scheduling from the
socket. Each turn reads at most 64 KiB and dispatches at most 16 frames before
yielding to the engine event loop. The existing per-session 100/s, burst-200
budget and message-size bounds remain unchanged. Pending continuations cannot
dispatch after termination; final error delivery occurs after authority is
retired. Tests inject byte-reader/writer callbacks into this same pump rather
than substituting a radio peer or merely testing a second parser.

Every credential-authenticated client connection admitted to TX registration
binds its own manager lifetime at successful hello. Observer, overflow and
administrator sessions do not acquire client actors. The lifecycle tests also
inject terminal transport failures to prove synchronous owner-scoped retirement
without opening a socket.

## Stop-attempt identity is not radio evidence

`TxCoordinator::StopRequest` identifies an exact stopping/unconfirmed operation,
radio-session generation and stop attempt. Superseding the attempt, resetting
the radio, or acknowledging the operation invalidates the old token. A matching
confirmation still cannot release ownership while a terminal writer is entered.

The token does **not** certify radio idle. Production code must establish the
causal relationship between the requested stop and the observed backend state
before returning it. Ordinary RX telemetry, a command being queued, a successful
write, local queue completion and a fixed timeout are not interchangeable with
that evidence. Independent actors cannot use the desktop actor's same-owner
reengagement exception while completion is unconfirmed.

The Flex production transport binding uses the
[xmit contract](https://github.com/flexradio/smartsdr-api-docs/wiki/TCPIP-xmit),
which describes starting interlock transitions; `UNKEY_REQUESTED` is still transitional.
The [TCP/IP contract](https://github.com/flexradio/smartsdr-api-docs/wiki/SmartSDR-TCPIP-API)
provides command sequence replies and command-associated status ordering, but
does not by itself identify an idle status with one stop attempt. FlexLib 4.2.18's
interlock state, source and TX client handle provide useful attribution, not an
operation identifier. No resubscribe-as-barrier or undocumented query is assumed.

The operator's available station is a **FLEX-6700**, not the RFC's reference
FLEX-8600. Separately authorized native bridge checks captured three brief PTT
cycles on firmware **4.2.18.41174**, ANT1, 7.244000 MHz, with RF power set to 10.
The first READY retained the TX client handle; later status cleared it. The
captured sequence now feeds the socket-free `FlexPttStopTracker` tests, including
the third capture's NOT_READY/out-of-band owner-clear refusal. The tracker
now has a production terminal-write adapter. See
[the evidence contract and remaining gaps](aetherd-flex-ptt-stop-evidence.md).
Socket-free or Demo tests do not substitute for hardware evidence. The shared
API's documented semantics, rather than a per-model hardware allowlist, ground
broader compatibility; missing or contradictory release evidence still blocks
handoff on every model.

`FlexPttWireSession` stamps raw input and actual complete socket writes on the
existing connection thread. A queued certificate has an atomic validity fence,
invalidated at that source before disconnect/contradiction reaches the model.
Canceled-before-write operations use separate exact-operation no-dispatch proof;
any entered key writer, including a partial write, excludes that shortcut.
Timeout or ambiguous firmware status leaves recovery closed, with explicit
operator disconnect/reconnect required. There is no automatic reconnect.

## Local v1 methods and private state

All methods use the existing v1 request envelope, negotiated `sessionId` and
shared request budget. Unknown fields are rejected. Opaque IDs are 32 lowercase
hexadecimal characters. `radioSession` is exactly `radio-1`. `radioGeneration`
is server-issued, changes on radio invalidation and emergency stop, and must be
copied explicitly into every mutation. Status bootstraps it without arming.
No grant/lease IDs enter shared resources or subscriptions.

| Method | Exact params, in addition to the envelope |
|---|---|
| `tx.status` | `radioSession` |
| `tx.acquire` | `radioSession`, `radioGeneration`, `grantId`, `intentSerial` |
| `tx.setKeying` | `radioSession`, `radioGeneration`, `grantId`, `operationId`, boolean `key` |
| `tx.release`, `tx.cancel` | `radioSession`, `radioGeneration`, `grantId`, `operationId` |
| `tx.keepAlive` | `radioSession`, `radioGeneration`, `grantId` |
| `txAdmin.listClients` | none |
| `txAdmin.issueGrant` | `radioSession`, `radioGeneration`, `clientId`, `activities: ["mox"]`, integer `maximumOperationMs`, `lifetimeMs`, `keepAliveMs` |
| `txAdmin.revokeGrant` | `radioSession`, `radioGeneration`, `grantId` |
| `txAdmin.emergencyStop` | `radioSession`, `radioGeneration` |

`intentSerial` is a canonical positive decimal string (up to uint64, not a
JSON floating-point number), strictly increasing per grant, including refused
Busy/Recovering attempts. Acquire never keys. The acquired operation accepts
one true key edge; a repeated true returns `tx.replay`. Key-off, release or
cancel ends that input immediately. Any later burst needs a fresh acquire.
`accepted` means the engine accepted the intent, not that RF was observed.

Status returns this connection's `clientId`, `grantId`, `operationId`,
`grantLive`, `intentActive`, `ready` and `recovering`, plus the radio identity.
These are authority/engine states, not measured transmitter state; telemetry
remains in `transmitState`. An absent or expired grant has an empty grant ID.
Only administrators may list other clients (including nonsecret principal IDs)
or issue/revoke grants. A grant-admin credential cannot key TX itself. Reusing
one client credential on two connections still creates distinct lifetimes.

There is no automatic grant renewal, waiting queue, repeated-key result cache,
startup grant, audio transport, TUNE/ATU/CW/CWX method, or physical-PTT adoption.
RX mutations also refuse while the coordinator retains any ownership, including
the acquired-but-not-keyed window and unconfirmed cleanup. An unqualified stop
therefore blocks these existing non-TX mutation paths as well as later TX
acquisition; explicit transport disconnect/reconnect is the recovery boundary
when no qualified completion arrives. A timeout never clears this fence.

### Operator grant administration

The separate one-action CLI reads the chosen admin credential from the same
native vault and connects to the already-serving private endpoint. It does not
take the daemon's authority lock, modify credentials, start discovery or open a
radio. No secret is accepted in argv, exported or logged. IDs/policy are public
selection parameters, not credentials.

Before sending the bearer hello, the client verifies the connected native
server handle belongs to the current OS account. Windows checks both the server
process token and the connected pipe's owner SID; a globally named pipe or a
successful connection is not proof of its server's identity. macOS/FreeBSD use
`getpeereid`, and Linux uses `SO_PEERCRED`. Unavailable or mismatching identity
fails closed without a credential-bearing write. This is an OS-account boundary,
not an identity guarantee for individual processes within that account.

On macOS the daemon selects Qt's CoreFoundation main-thread event dispatcher
before constructing `QCoreApplication`. QtKeychain's Apple implementation posts
its completions to the native main queue, which the default UNIX dispatcher
does not service. This keeps the daemon QtWidgets-free and does not create a
GUI application. Provisioning and startup reads time out after 30 seconds with
the service still closed; an accepted write that times out remains uncertain
and must be inspected, never blindly retried or rolled back. Native OS access
prompts remain authoritative and are not bypassed. Source references:
[Qt dispatcher selection](https://github.com/qt/qtbase/blob/v6.8.3/src/corelib/thread/qthread_unix.cpp)
and [QtKeychain Apple delivery](https://github.com/frankosterfeld/qtkeychain/blob/main/qtkeychain/keychain_apple.mm).

```sh
aetherd --socket aetherd-v1 --credential-authority <authority-id> \
  --admin-credential <admin-id> --tx-admin list
aetherd --socket aetherd-v1 --credential-authority <authority-id> \
  --admin-credential <admin-id> --tx-admin grant --tx-client <client-id> \
  --radio-generation <generation-from-list> --tx-operation-ms 10000 \
  --tx-lifetime-ms 60000 --tx-keepalive-ms 5000
aetherd --socket aetherd-v1 --credential-authority <authority-id> \
  --admin-credential <admin-id> --tx-admin revoke --tx-grant <grant-id> \
  --radio-generation <generation-from-list>
aetherd --socket aetherd-v1 --credential-authority <authority-id> \
  --admin-credential <admin-id> --tx-admin stop \
  --radio-generation <generation-from-list>
```

List does not refresh client liveness. Grant creation requires the client to
already be connected; keepalive is that client's responsibility. Emergency stop
retires every independent grant before requesting the global operator stop, so
even idle clients need new explicit authorization afterward.

## Verification and limits

`tx_grant_manager_test` exercises positive injected A-to-stop-to-B handoff,
fresh-intent requirements, stale/cross-client releases, independent expiry,
worker fences, continuous-microphone exclusion, stop-attempt replacement,
entered writers, reentrancy, limits, churn and radio/client lifetime changes.
One real event-loop timer case expires active work without another request.
Additional cases cross the operation deadline between expiry processing and
timer scheduling, and reschedule locally completed but unconfirmed ownership.
Both retain the original stop deadline even after dispatch permission ends.
These tests are socket-free; their injected confirmation is state-machine
evidence, not firmware qualification. Existing coordinator, model-operation and
audio-context regressions remain applicable.

`control_transport_lifecycle_test` covers the production input pump, terminal
session-to-grant lifetime, event-loop yields, malformed/oversized/budget failures,
write failures, destruction and reentrant disconnects without binding sockets.
The existing `local_control_server_test` separately exercises our own local
server over its real private socket; it is not a radio-firmware simulation.

`flex_ptt_wire_session_test` exercises the real writer/tracker composition with
captured inputs, including rapid release and partial-write refusal.
`control_transmit_service_test` composes the real protocol, session, credentials,
grants and coordinator with an injected typed target; it proves ownership,
replay, generation, expiration, revocation and emergency behavior without RF.

Complete-increment qualification on 2026-09-19 used this branch's actual daemon,
native-vault credentials for two independent clients and a separate administrator,
and a FLEX-6700 running 4.2.18.41174 over LAN. Client A keyed and stopped; client
B's previously refused intent remained rejected, and a fresh B intent keyed only
after qualified owner-clear. Both cycles used ANT1, 7.244000 MHz LSB, RF power 10
and microphone gain 0, without an injected tone or audio. Idle, disconnect,
guarded receive-setting restoration and deletion of the test credentials were
verified. This is command/status evidence, not a radiated-power measurement.

An earlier attempt correctly refused handoff after finding a second, anonymous
unkey command. Model cleanup now retains its independent operation identity.
`tx_operation_integration_test` covers release, rapid release, input stop,
disconnect, emergency stop and expiry through the production model/wire seam.
No timeout or relaxed recognition rule was used to obtain the successful result.

Native Mac/Linux/Windows builds, focused tests, isolated Demo/MCP checks and
native-vault/actual-daemon checks have been exercised. The PR records final
revision-specific results and limitations; unit-test injection is never a
substitute for the hardware evidence above.

SIP, remote listeners, Stage 5 audio transport and new backend feature parity
remain out of scope. No timer, client grant or local ownership claim guarantees
exclusivity against physical PTT or other radio clients.
