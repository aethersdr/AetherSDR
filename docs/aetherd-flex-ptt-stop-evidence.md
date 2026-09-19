# Stage 4: Flex software-PTT stop evidence

Tracking: #5690, #3849. `FlexPttStopTracker` is a bounded candidate sequence
recognizer below the vendor seam. `FlexPttWireSession` now composes it with the
actual LAN writer in `RadioConnection`, and `FlexBackend` returns revocable
typed evidence to the model. Recognition tests alone do not qualify the full
daemon/client/backend path; the complete-increment hardware evidence below is
separate from the socket-free recognition tests.

## Evidence and its limits

The official [xmit command documentation](https://github.com/flexradio/smartsdr-api-docs/wiki/TCPIP-xmit)
describes starting an interlock transition, followed by status messages for the
transitions. A successful command reply is not completion of that transition.
The [interlock reference](https://github.com/flexradio/smartsdr-api-docs/wiki/TCPIP-interlock)
and FlexLib `Radio.cs` for 4.2.18.41174 distinguish state, source and TX client
ownership. Neither reference supplies an operation ID for an asynchronous
interlock status. We must not manufacture one by tagging the current stop token
onto an arbitrary receiving/idle notification.

The [TCP/IP API's ordering rule](https://github.com/flexradio/smartsdr-api-docs/wiki/SmartSDR-TCPIP-API)
states that statuses reported by an earlier executed command precede the first
status reported by a later command. Combined with the per-transition xmit
contract, this is the source-backed basis for the candidate sequence below.
Applying that rule to this complete software-PTT lifecycle is an implementation
inference, constrained by actual terminal-write order and by admitting only one
PTT lifecycle at a time. It does not make a queued command or a replayed model
notification into a transport barrier.

Three separately authorized, approximately 350-ms software-PTT cycles were
captured on a FLEX-6700 running 4.2.18.41174 on 2026-09-18. The native automation
bridge drove the operator's already-running 26.9.2 desktop application, **not
this branch's binary**. The observed settings were ANT1, 7.244000 MHz, LSB,
RF power 10 and microphone gain 0; no test tone or audio was injected. Firmware
reported TRANSMITTING and subsequently receiving. This is a command/status
trace, not a measurement of radiated power or independent-client validation.

The first two captures had this order on the same TCP connection (sequences and
client handle are normalized in the socket-free fixture):

1. Actual `xmit 1` write, then its successful matching reply.
2. `PTT_REQUESTED`, then `TRANSMITTING`, both with our handle and `source=SW`.
3. Actual `xmit 0` write, then its successful matching reply.
4. `UNKEY_REQUESTED`, then `READY`, both still retaining our handle.
5. A later `READY` with `tx_client_handle=0x00000000`.

The gap from the first READY to owner-clear was approximately **428 ms** and
**427 ms**. These are observations, **not timer values for inferring release**.
The third cycle reached READY with our handle; restoring the original receive
frequency then produced owner-clear in `NOT_READY / OUT_OF_BAND`. The tracker
deliberately rejects that as an eligible handoff even though the radio reports
receiving. Receive frequency and mic gain were restored and RF power left at 10.

These traces cover one model, firmware, LAN session and software-keying path.
They do not qualify FLEX-8600, WAN/SmartLink, physical PTT, VOX, external clients,
CW/CWX, ATU, TUNE, audio transport, or arbitrary firmware versions.

## Candidate recognition contract

The adapter provides one transport-ordered stream of actual write
receipts and raw input lines, with a strictly increasing ordinal and a nonzero,
never-reused connection generation. Model callback arrival time is not that
ordering. A receipt means the full exact command reached the terminal socket
writer; queue completion, a successful fence check, or a partial write is not a
receipt. Pending command sequences must remain unique for the connection; the
tracker rejects sequence reuse/wrap for successive PTT attempts.

The tracker starts with a complete eligible idle observation. It then binds the
original coordinator operation and key-on command sequence before accepting a
write/reply. It may bind an already-stopping operation's exact `StopRequest`
and a new key-off sequence while key-on readback is still queued, so rapid
release never waits for a key-on observation before sending unkey. The full
own-key sequence must nevertheless be observed before unkey-state proof.
That request must belong to the same operation and coordinator. Only the full
ordered sequence above yields the same original token as a candidate result.

Relevant interlock observations must contain state, source, client handle,
TX-allowed and reason together; fields are never carried forward across partial
messages. Malformed numbers, duplicate fields, foreign ownership, physical keying,
missing/reordered transitions, failed/partial writes and failed replies reject
the attempt. Timing-only and band-configuration messages are not state samples.
In particular, the tolerant display parser's default-zero conversion is not
used to interpret evidence. An invalid reply cannot become a successful reply
and an invalid handle cannot become the unowned handle.

A five-second transition deadline bounds how long a candidate attempt can wait;
it is checked at input and result consumption as well as explicit polling.
Expiry refuses evidence; it never means idle and never releases ownership.
There is no timeout while the observed operation remains TRANSMITTING: the
coordinator/grant deadlines are responsible for initiating stop. The tracker
does not replace those deadlines or a physical radio watchdog.

Unconsumed evidence is invalidated by later contradictory status, a superseded
stop attempt, coordinator/transport lifetime change or the transition deadline.
Even a candidate result cannot cross the coordinator's entered-writer barrier.
Tests retain it while a writer exits and then acknowledge that exact token. No
request refused as busy/recovering is automatically retried.

## Production composition and qualification

`RadioConnection` constructs the wire session on its existing worker thread.
The session observes raw lines before presentation parsing, uses that
transport's connection generation, and records complete writes from the common
terminal socket writer. It does not use model notifications or queue completion
as receipts. Sequences come from the existing shared command counter.

`FlexBackend::independentTxControl()` restricts this implementation to MOX on
FLEX-6700 firmware 4.2.18.41174 with a connected non-synthetic LAN transport.
Other models/firmware, WAN, other families and other activities return no
qualification. A failed/ambiguous sequence still attempts authorized unkey,
but does not acknowledge stop; recovery requires an explicit operator action.

A queued canceled key command records the exact operation without entering a
writer. Its subsequent ordered stop can produce local no-dispatch evidence
without sending an off command that might affect unrelated hardware. This path
is permanently excluded as soon as the key writer is entered, even if it returns
a partial or failed write. It is not a radio-idle observation.

Evidence holds an atomic validity fence. Later contradiction or disconnect
invalidates it on the source thread before a queued backend/model receiver can
consume it. The model also checks receiver generation, stop-attempt identity
and the coordinator's entered-writer barrier. It retries only that bookkeeping,
with the same revocable evidence, never with a replacement token or a timer as
proof.

Socket-free tests cover the complete ordered rapid-release sequence, local
no-dispatch completion, partial writer failure, stale delivery, reconnect,
contradictory observations and a subsequent fresh actor. The model integration
test also checks six independent cleanup routes preserve the original operation
and send exactly one qualified unkey, rather than an additional legacy command.

On 2026-09-19 this branch's actual daemon completed A-stop-B qualification on
the same FLEX-6700/4.2.18.41174 LAN configuration, using two distinct native-vault
client credentials and a separate administrator. A's command/status sequence ran
from key at 08:55:33.002 (daemon log time) through unkey at 08:55:33.256 to READY/owner-clear at
08:55:33.691. B's old busy intent stayed rejected. Fresh B acquisition then keyed
at 08:55:33.795, unkeyed at 08:55:34.123 and reached READY/owner-clear at
08:55:34.557. Settings were ANT1, 7.244000 MHz LSB, RF power 10 and mic gain 0;
no audio or tone was injected. This is ordered command/status proof, not a
measurement of radiated power. The independent stop-only watchdog did not fire.
Final idle, disconnect, guarded receive-setting restoration and test-credential
deletion were verified.

An earlier attempt sent a second anonymous legacy unkey and correctly remained
in recovery. Preserving the original independent operation in model cleanup
fixed that cause; the recognizer was not loosened. Mac/Linux/Windows native
builds, focused tests and isolated Demo/MCP/native-vault checks are recorded in
the PR with their tested revisions. These results do not expand the qualified
model, firmware, transport or activity envelope described above.
