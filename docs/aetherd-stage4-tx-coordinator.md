# Stage 4: engine TX coordinator and primary desktop paths

This is the first Stage 4 increment of RFC #3849, after the receive-control
milestone (#5563). It does **not** enable daemon transmission or complete the
RFC's multi-client TX arbiter.

The producer-ownership follow-up extends that foundation, not its authority:
commands and TX audio carry their original producer input through local queues
and terminal writers. The desktop compatibility actor remains shared.

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

`finishLocalIntent()` cancels further key-on delivery but retains the preceding
owner in an unconfirmed-completion slot. Only that same actor may reengage before
qualified stop evidence; another actor remains `Busy`. Reengagement creates a new
queue generation, so old cleanup cannot unkey it, while carrying the original
bounded deadline forward. Local release/re-key cannot renew a bounded lease.
`acknowledgeStopped()` alone releases ownership for handoff, and only for the
matching finished or forcibly stopped operation. Active intent and stale earlier
operations cannot be acknowledged. No production radio-status handler supplies
this acknowledgment yet; transport teardown remains the recovery boundary.

Cancellation, revocation and expiry invalidate key-on delivery before invoking
the stop callback. Recovery is published before that callback and remains an
admission barrier even if the callback acknowledges synchronously. A stale
operation cannot acknowledge or complete another operation. Connection reset
also invalidates queued cleanup, including when a transport object survives.
An idle cleanup fence can permit key-up delivery but cannot acquire, complete
or acknowledge an owned operation.

`RadioModel` registers one **transitional desktop compatibility actor**. Producer
lifetimes and individual contribution handles are distinct within that actor;
they are not independent TX grants or permission to hand off after a local tail.
`PttSource` remains display/preflight metadata, never identity or a permission grant.

## Primary paths

### Producer contribution handles

`TxCoordinator::Intent` identifies one contribution to an already admitted
operation. It does not acquire an actor grant or start TX. Repeated admission
with the same live handle is idempotent; independently held handles remain
independent even inside the shared desktop operation. The registry is bounded
to 256 outstanding contributions. Ending a contribution returns its slot, and
session reset, forced stop and destruction retire the remaining handles.

Release is marked before invoking callbacks or enqueueing cleanup. An ensuing
reengagement gets a new handle while the earlier normal tail remains pending.
The queue callback ends its captured handle exactly once, including when a
newer contribution has started. Neither a duplicate callback nor a handle from
another coordinator can retire the current contribution. A pending contribution
blocks `finishLocalIntent()` even if the caller incorrectly attempts completion.
All mutation remains on the engine thread; workers may inspect the atomic fence.

The production MOX, TUNE, ATU, straight-key, CW PTT and CWX paths now retain
these handles. The derived activity mask is only a compatibility view for
existing interlocks/cleanup, not release authority. The historical operation
activity mask still records which radio-buffered paths might need cleanup.

The compatibility slots remain for not-yet-converted local activity entry
points. CAT PTT/CW text and TCI now retain separate accepted-session producer handles.
Serial/PTY CAT uses its configured endpoint lifetime, not a guessed process ID.
AX.25, WSPR, native keyboard/device inputs and bridge actions retain their
controller lifetime and original input. A short-lived MCP request socket is
not a TX producer: the bridge producer follows its trusted authorization.
Normal tail consumption remains local bookkeeping, never radio-stop proof.

`TxController` is an engine-owned facade, not another actor. A compound input
view captures one root before delivery; subsequent button phases and sequence
elements cannot renew that root after cancellation. Native device callbacks
capture the root before their first queued hop. Device close advances an atomic
input epoch; reopening can accept fresh input without rehabilitating an old
queued press or letting its release consume a newer hold.

Packet program roots are captured when the operator or authorized controller
arms receive replies, a beacon, digipeating, PMS or a terminal session. Retries
and generated frames derive from that root, never from the session current at
timer/output time. Stored message history deliberately contains no persistent
TX authority; completed ACK/reject/exhausted records release their live request
cells too. Native-enabled KISS frames use their accepted client's producer.
Generic modem enable and pointer actions use the same scoped receive action as
the explicit bridge command: without a TX grant they enable RX without arming
automatic APRS replies. A bridge-started KISS listener is receive-only, even
when that bridge has a TX grant; granting TX to later KISS peers is not part of
this increment. Port changes preserve that policy. Native operator enable and
startup retain the existing per-client producers and defaults. WSPR retains
its schedule root through PTT, borrowed audio-route cleanup and generation-
matched pump/generator stop; another program cannot overwrite that route.

Explicit TUNE, ATU and CW-text controller routes preserve the existing model
preflight, speed expansion and UI notifications while carrying a captured
request through dispatch. They do not install an ambient caller identity around
a widget callback. TUNE, ATU and the radio-side CW queue are singleton resources:
a different producer cannot replace an active same-kind request. A refusal
closes that input; retry requires fresh intent after the earlier contribution
finishes. Compatible MOX contributions still use their separate holds.

Scoped release must match the request's activity. A PTT off cannot consume a
TUNE request, duplicate release does not write twice, and ATU status completion
retires its captured originating intent. CW-text handoff retains that intent
through all queued segments, not merely until `send()` returns. A CAT client's
disconnect fences its queued text immediately; its stop command cannot clear
another client's CW queue. Synchronous abort notifications cannot insert a
replacement queue into the clear operation still on the stack.

`Producer::request()` captures a bounded request cell and connection generation
at input, before queued admission. The engine binds it once to an original
operation and intent. Closing an unbound request prevents a delayed route
callback from keying. An admitted normal release keeps its media/command permit
through its queued tail; consumption ends that intent exactly once. Release
and teardown of one compatible contributor cannot unkey another. Quindar/RADE
reengagement retires the superseded release's intent, without truncating the
new contribution or allowing the old release callback to target it.

The Metis MOX latch checks the original request before accepting a queued key
command. Its subsequent periodic packets sustain only while an admitted
compatible MOX/CW-PTT contribution is live. This bounded, immutable snapshot is
published by the coordinator; workers consult atomic producer/intent validity,
not mutable models. Thus the most recently admitted client leaving does not
unkey a remaining contributor, but the last producer's death fences the latch
even before its queued cleanup arrives. This sustain permit is never used to
admit a queued key command or ordinary TX audio.
CW carrier holds and backend-owned break-in MOX use a separate CW sustain
permit. Overlapping or successive elements retain the carrier through the
normal hang, while a bare CW hold cannot sustain ordinary manual MOX.

Icom scheduling and retained retries preserve original command and audio
authority. A cancelled CW batch cannot borrow a still-held MOX operation.
Retained keying/ATU state is superseded by a newer command in the same typed
group; CW chunks append, while abort and a subsequent batch supersede each
other's stale retries. Original FIFO delivery is unchanged. Invalid retries
become sequence-preserving protocol Idle packets, not old key-on/unkey/audio.

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
Iambic producer-thread input retains the original paddle root through derived
elements and normal mode-B tails. Reset/reconnect, producer closure and the
scheduled-time floor reject stale RF edges before either Flex or non-Flex
delivery. Cancelled input may still drive local practice sidetone; local
monitoring is not RF authority.

### Primary Flex and CWX command queues

Flex `setKeying`, `setTune`, `setAtu` and `abortCwText` use a dedicated TX
command sink, with no fallback to the generic command sink. Encoding remains
behind the backend seam; the sink's keying/cleanup flag is a classification,
not authority. RadioModel captures its original operation and transport and
checks them at the LAN writer or synchronous WAN dispatch.

Normal primary key-up retains local operation lifetime until the TCP queue
consumes it. This preserves both edges of a short key-down/key-up sequence;
disconnect cancels key-down immediately instead. Idle cleanup cannot unkey a
subsequently acquired operation, and a late queue callback cannot finish a
newer operation or a reengaged local intent. Queue consumption includes a
cancelled write and is **not** transport delivery, a command acknowledgement,
radio readback, or a multi-client handoff permission.

CWX text/macro commands additionally capture a worker-safe batch cancellation
fence. Clear/reset invalidates that fence immediately even while a separately
held MOX keeps the compatibility operation alive. Cancelled queued commands
retire their pending reply callbacks. Worker checks never read CwxModel state
or dereference the model's synchronous admission closure.

An unsynced macro retains its operation until queued local handoff completes.
Appending one to known text abandons the now-incomplete drain index without
cancelling either the text or macro waiting for transport. Clear/reset still
cancels both. Partial `cwx erase` keeps its existing, unqualified semantics;
this increment does not resolve the hardware-dependent drain-index question
documented in `CwxModel::erase`.

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

### Bridge authorization-lifetime ownership

The watchdog observes actual engine admission by its own producer. It neither
adopts another contributor's shared operation nor infers ownership from the
radio's TX flag. Cleanup covers only its captured MOX, TUNE, ATU, straight-key/
CW PTT and CWX contributions; a later compatible CAT/manual hold survives.

The existing 20-second default is unchanged. Its clock is monotonic, repeated
commands cannot extend it, and a pending operation or readback gap does not
disarm it. Normal operator duration remains unbounded. The diagnostic's per-key
observer receives pre-key identity/state after synchronous engine admission,
before its first event-loop wait; it no longer claims a transmission in advance.

Deferred bridge widget invocations capture their original input before queueing
and claim only after engine admission. Revocation, an
observe-only transition, model replacement, bridge stop or destruction fences
queued TX actions; re-enabling permission does not resurrect them.

Registered TX buttons and shortcuts call the same typed controller actions as
the native UI. Pointer press/move retain focus, down state and hit testing;
only an explicit release inside activates. Cancellation, disconnect and gesture
timeout cannot turn into a TX click. Keyboard events carry typed source identity
through the real application filters, separate from native held-key state.
Unknown/unregistered keying controls fail closed rather than borrowing a native
operator callback. The peripheral SPE front-panel TUNE route is not converted
to radio ATU and remains unavailable through these scoped bridge actions.

This is still the shared compatibility actor, not independent actor arbitration.
A normal local completion plus reported TX tail is not qualified stop evidence. The watchdog
continues to use the existing stop entry points, not the coordinator's future
cancel/expiry recovery path; independent client handoff remains disabled.

`automation_tx_watchdog_test` injects a socket-free backend and exercises real
engine admission, typed bridge requests, permission changes and deferred
callbacks. It covers replacement between polls, repeated commands, refused and
nested requests, RX gaps, CW/ATU cleanup and reentrant replacement during stop.
It also waits for the production timer to expire an owned operation after
initial permission enable and after disable/re-enable, without calling the poll
handler directly.

The daemon still advertises no TX permission or method. No credential
provisioning, remote listener, TLS policy, device discovery or transmit default
is changed. The bridge's existing permission gate and watchdog remain intact.

Desktop compatibility completion ends a local intent, **not** a qualified
radio-idle claim. The coordinator now enforces this distinction rather than
relying on callers to avoid treating local completion as handoff authority.
Cancellation, revocation, expiry and reset also retain and stop an unconfirmed
tail, not just an operation whose local intent is still active. The same actor's
unbounded desktop reengagement remains compatible with existing controls.
The remaining work must assign independent trusted actor grants to the now-
identified producers, bind bounded actor expiry to engine scheduling and
complete qualified stop/readback recovery. Peripheral transmit mechanisms need
their own explicit contract; this increment does not grant them radio authority.
Only after that coverage is demonstrated can daemon TX grants be considered.
The separately tracked CW/TUNE UX interlock in #5513 is not replaced here.

Cross-tracker sequencing does not block this Stage 4 work (#3849's
cross-tracker prerequisites comment, 2026-09-11). Capability serialization
must not widen before #5594/M1 and the capability-record convention are ready.
The Flex RX PCM conversion belongs to #5468/#5598; this TX-command increment
does not perform that conversion or begin Stage 5 streaming.

## Verification boundary

`tx_coordinator_test` exercises actor isolation, duration, revocation, recovery,
stale handles, thread affinity, reentrancy and cleanup-only authority. It also
covers unconfirmed ownership, same-actor reengagement, stale acknowledgment,
every unconfirmed-tail stop source and non-renewal of the original deadline.
`tx_operation_integration_test` drives production model entry points with an
injected backend and terminal packet writer. It does not start a radio peer,
bind a socket, discover hardware or transmit RF. It covers typed dispatch,
refusals, deferred release, replacement, reentrant intent, Quindar, CWX and
queued NetCW, primary Flex and CWX delivery. The additional cases cover short
MOX/TUNE/ATU edges, reset-before-write, idle cleanup, reengagement, CWX
clear-and-replace while MOX is held, producer destruction, cancelled reply
retirement, and preservation of unknown-length macro tails.
Production local completion and uncorrelated RX readback are tested not to
authorize another actor; backend teardown clears the retained ownership barrier.
Existing model, ATU, Icom, CAT/TUNE, applet and bridge
watchdog tests remain part of the targeted regression set.
The private test-only terminal writers in `PanadapterStream` and
`RadioConnection` allow these queue tests to exercise production dispatch without
initializing sockets or substituting synthetic radio firmware.

Native Demo/MCP checks with TX disabled can establish launch, identity and
positive receive-path behavior. Non-events and TX refusals belong in the
socket-free injected regressions, not a Demo observation. Neither can establish over-the-air CW
timing, an amplifier's response, or RADE RF tail quality; those require
separately authorized hardware verification.
