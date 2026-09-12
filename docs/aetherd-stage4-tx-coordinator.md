# Stage 4: engine TX coordinator and primary desktop paths

This is the first Stage 4 increment of RFC #3849, after the receive-control
milestone (#5563). It does **not** enable daemon transmission or complete the
RFC's multi-client TX arbiter.

## Authority and ownership

`TxCoordinator` lives in `libaethercore`. Trusted engine composition registers
opaque actor handles with a transmit policy. Handles belong to one coordinator;
client strings, `PttSource` values and handles from another coordinator cannot
mint authority. Mutations are confined to the coordinator's owning thread.

There are at most 64 live actor registrations. An operation has one owner,
an opaque identity and a cancellation fence. Repeated acquisition by that owner
keeps the operation and its original duration limit; another actor is refused.
Zero maximum duration preserves the existing unbounded local-operator workflow.
The optional bounded policy is tested in isolation but is not granted to daemon
clients by this increment.

Cancellation, revocation and expiry invalidate key-on delivery before invoking
the stop callback. Recovery is published before that callback and remains an
admission barrier even if the callback acknowledges synchronously. A stale
operation cannot acknowledge or complete another operation. Connection reset
also invalidates queued cleanup, including when a transport object survives.
An idle cleanup fence can permit key-up delivery but cannot acquire, complete
or acknowledge an owned operation.

`RadioModel` registers one **transitional desktop compatibility actor**. Existing
desktop, CAT, TCI, MIDI, keyer and bridge calls still share their existing entry
points. This does not establish individual client identity. `PttSource` remains
display/preflight metadata, never the owner or a permission grant.

## Primary paths

| Intent | Engine route |
| --- | --- |
| MOX/PTT | TransmitModel admission, RadioModel, `IRadioBackend::setKeying` |
| TUNE / two-tone | TransmitModel admission, RadioModel, `IRadioBackend::setTune` |
| Internal ATU | TransmitModel admission, RadioModel, `IRadioBackend::setAtu` |
| Straight key / paddle / timed CW / CW PTT | RadioModel admission and existing backend or NetCW transport |
| CWX text, characters and macros | Actual-send admission before commands or local-keyer delivery |

MOX, TUNE and ATU no longer emit parallel raw keying commands from
`TransmitModel`. Flex encodes the same FlexLib 4.2.18 commands behind its typed
backend methods. The receive-only backend, receive-only mode, pan inhibit and
existing operator preflight checks remain in force. Admission refusals carry a
distinct operator message and notification key per `Refusal` reason; only
`Recovering` is reachable while a single desktop actor exists, but the per-client
actors of the next increment make the rest reachable. Key-up, bypass and abort
are not subject to key-on permission checks.

Admission precedes optimistic model state. The resulting fence is checked
again after synchronous notifications. Intent epochs prevent a reentrant new
key from being followed by the preceding intent's stale key-up. This does not
turn local intent into radio readback: existing Flex interlock, Icom PTT
readback and command-edge fallback provenance are unchanged.

Direct `TransmitModel::setMox(true)` now runs the same source-aware preflight
as the operator PTT path, before optimistic state or admission. This intentionally
closes the former direct-call bypass: non-DAX voice MOX needs an assigned TX
slice. A connection attempt is not a connected session; admission stays closed
until connection completion. Tests of dispatch use an injected backend with
the explicit slice/admission prerequisites instead of an unanswered connection.

The compatibility actor tracks active primary intent kinds. CWX queue drain
does not end a separately held MOX intent. QSK interlock gaps do not release
the CWX batch. CWX clear/reset fences remaining segments and late replies
through its existing drain epoch as well as the engine operation.
Unsupported radio-side CWX and tunerless ATU are refused before acquisition.
Terminal ATU status closes local intent even when an in-progress report was
missed; synchronous status observers cannot use that old completion to end a
new ATU command. This status has no operation ID and is not qualified readback
for multi-client arbitration.

CWX rejection or invalid reply cancels the current batch and disarms its drain
watch; stale replies are fenced by both epoch and operation. Cancelled speed
expansion restores the base WPM without sending later text. Neutral radio-side
text dispatch happens before sidetone notification, so a rejected batch does
not start a misleading local playback. Non-Flex acceptance completes the local
handoff after all segments, not RF transmission. An unsynced Flex macro likewise
completes only its local handoff: its text length is unknown, so it cannot arm
the indexed drain watch. Both remain explicitly unsuitable as another client's
TX-admission evidence.

## Normal release versus cancellation

Normal PTT release retains Quindar outro and RADE end-of-over sequencing.
Each deferred release carries its original atomic cancellation fence. New
key-on, explicit stop, reset or destruction invalidates it. Duplicate releases
do not truncate an in-flight outro. Release execution stays on the owning
thread; workers may inspect the fence but cannot mutate the model through it.

RADE completion carries the original request ID. Its queued audio-gate closure
and delayed PTT release check the original release fence. Re-engagement during
EOO also has an explicit intent notification, because optimistic MOX can remain
true throughout that interval and emit no state edge. The hardware/interlock
fallback may finish its own audio tail but gains no authority to release a
later carrier.

NetCW retains the existing timestamp, packet-count, dedup index, four UDP
copies and TCP backstop. UDP copies, the TCP backstop and the no-stream TCP
fallback capture the original transport and check their fences at dispatch.
Normal key-up retains the operation until both participating transport queues
consume it (including the final UDP copy), so a short element
does not lose its already-queued key-down. This is transport completion, not
proof of RF reception or radio-idle state.
Iambic producer-thread input captures a session generation before queueing onto
the model; reset/reconnect and the scheduled-time floor reject old-session edges
before either Flex or non-Flex delivery. This is session isolation, not yet
per-producer authorization within a shared desktop operation.

Disconnect, forced disconnect and backend replacement cancel before transport
reuse. Session admission closes before any cancellation, pending-command reply
or model-removal notification, even when no operation was active. It stays
closed through the disconnect gap and reopens only on the new connection edge.
Stop cleanup remains in recovery until transport loss/teardown is
acknowledged. Every stop source therefore needs a matching acknowledgment: an
unacknowledged stop keeps admission closed for the rest of the session. `reset()`
is the only production stop source in this increment and the disconnect/teardown
paths acknowledge it; `cancel()`, `revoke()`, `expire()` and `emergencyStop()`
have no production callers yet, so the increment that gives one of them a caller
must land its acknowledgment path in the same change. A refusal that reaches the
coordinator outside a disconnect gap is logged, because the session latch
short-circuits the normal case before admission is attempted. Destruction does
not call presentation observers while the aggregate is partially destroyed.

## Deliberate limits and next increment

The daemon still advertises no TX permission or method. No credential
provisioning, remote listener, TLS policy, device discovery or transmit default
is changed. The bridge's existing permission gate and watchdog remain intact.

Desktop compatibility completion ends a local intent, **not** a qualified
radio-idle claim. It must not be reused to authorize another client's TX.
The next increment must propagate per-client actors through all integration
and audio producers, bind bounded actor expiry to engine scheduling, complete
qualified stop/readback recovery and fence remaining queued audio/wire paths.
Only after that coverage is demonstrated can daemon TX grants be considered.
The separately tracked CW/TUNE UX interlock in #5513 is not replaced here.

## Verification boundary

`tx_coordinator_test` exercises actor isolation, duration, revocation, recovery,
stale handles, thread affinity, reentrancy and cleanup-only authority.
`tx_operation_integration_test` drives production model entry points with an
injected backend and terminal packet writer. It does not start a radio peer,
bind a socket, discover hardware or transmit RF. It covers typed dispatch,
refusals, deferred release, replacement, reentrant intent, Quindar, CWX and
queued NetCW delivery. Existing model, ATU, Icom, CAT/TUNE, applet and bridge
watchdog tests remain part of the targeted regression set.
The private test-only terminal writers in `PanadapterStream` and
`RadioConnection` allow these queue tests to exercise production dispatch without
initializing sockets or substituting synthetic radio firmware.

Native Demo/MCP checks with TX disabled can establish launch, identity,
receive-path and refusal behavior. They cannot establish over-the-air CW
timing, an amplifier's response, or RADE RF tail quality; those require
separately authorized hardware verification.
