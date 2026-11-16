# Local DAX IQ measurement transport

This development path supplies raw feedback for the PR 4770 analyzer. It is
implemented in the DSP, its owning Go process, the authenticated HTTP relay
and the desktop capture adapter/controller. The measurement model composes
the production TX lease; the development operator GUI exposes the composed workflow. It has local C/Go/HTTP and adapter/controller/analyzer integration evidence.
The installed FLEX-8600 completed two finite, four-frame corrected GUI runs
with FDX disabled; see [the qualification record](GUI-FOUR-FRAME-QUALIFICATION.json).
This is not production or FLEX-6000 qualification.

The DSP observes its existing guarded local DAX IQ stream before decimation
or feedback equalization. It creates no stream, changes no panadapter or
slice, and has no keying operation. DAX audio is not a measurement source.
An unkeyed capture is allowed for receive readiness checks.

Each request collects 16,384 complex float32 samples in a fixed 128 KiB
buffer in the DSP event-loop state. Collection allocates nothing, takes no
lock and starts no thread. Only one snapshot exists per owned DSP process.
A new monotonically increasing capture ID replaces the previous snapshot;
repeated or delayed older requests cannot restart it.

A partial snapshot is rejected after a continuity reset, nonfinite sample,
change of stream/rate/center/acquisition generation/key/correction state,
measurement source identity or guarded tone-window state,
backward clock, or receipt gap exceeding 100 ms. Collection expires after
one second. Complete snapshots remain immutable across subsequent unkey or
reset for retrieval, and expire five seconds after the request. Metadata
records the original state. A snapshot does not certify that excitation or
the PA had settled: the measurement controller must enforce settling windows.

## Private protocol

The existing unnamed, connected, inherited Unix datagram socket carries:

| Request | Bytes | Meaning |
|---|---|---|
| `?` + nonce | 9 | Existing JSON status, unchanged |
| `B` + nonce + capture ID | 17 | Begin or retry snapshot; return first chunk/status |
| `R` + nonce + capture ID + sample offset | 21 | Read snapshot chunk/status |

Nonce and capture ID are unsigned 64-bit big-endian integers; offset is
unsigned 32-bit big-endian. Go uses the begin request's nonce as the capture
ID. All requests share the existing serialization lock and response nonce
check. Capture never falls back to a shared TCP port or another DSP process.
The DSP services at most four requests per event-loop iteration and sends
each reply without blocking. Lost replies are errors or bounded read retries;
they cannot trigger RF or replace a snapshot with the same ID.

Every response begins with the eight-byte request nonce. Capture bodies use
the following fixed header, followed by zero or 1,024 raw complex samples:

| Body offset | Type | Field |
|---|---|---|
| 0 | 8 bytes | `PDIQ0002` |
| 8 | u64 | Requested capture ID |
| 16 | u32 | State: 1 pending, 2 ready, 3 invalid, 4 expired, 5 unavailable |
| 20 | u32 | Requested sample offset, a multiple of 1,024 below 16,384 |
| 24 | u32 | Returned sample count: zero or 1,024 |
| 28 | u32 | Sample rate in Hz |
| 32 | u64 | Panadapter center in Hz |
| 40 | u64 | Acquisition generation |
| 48 | u64 | First packet consumption time, monotonic ns |
| 56 | u64 | Last packet consumption time, monotonic ns |
| 64 | u32 | DAX IQ stream ID |
| 68 | u32 | Bit 0 keyed; bit 1 DSP correction active; bit 2 guarded measurement tones |
| 72 | u64 | Accepted measurement source ID, or zero |
| 80 | bytes | Unchanged little-endian float32 I,Q pairs |

The Go reader also accepts the legacy `PDIQ0001` 72-byte header, which has
no measurement provenance and cannot satisfy the composed measurement workflow.
All header integers are big-endian. Timestamps are local process consumption
times, not FPGA timestamps or calibrated latency measurements. Flags describe
DSP state during receipt; RF propagation and filter delay still require
settling guards. Sample values are not calibrated antenna-side watts.

Go bounds retrieval to two seconds, checks every chunk's ID, offset, size,
metadata, finite samples and original process lifetime, and returns a complete
128 KiB frame or an error. Partial frames never reach the caller. A newer
concurrent capture supersedes the older one, which must fail retrieval.

## Authenticated transfer

`POST /v1/aetherpd/capture` accepts an empty body and no query parameters.
It collects a new snapshot and returns a manifest containing `capture_id`,
`format` (`f32le-iq-v1`), `sample_rate_hz`, `center_hz`, `sample_count` (16,384),
`chunk_samples` (512), `keyed`, `correcting`, and `first_receive_ns` /
`last_receive_ns`. Nanosecond timestamps are decimal strings to avoid loss of
integer precision in JSON clients. The v1 endpoint exposes no stream or process
identifiers. Native `PDIQ0002` captures also carry
`measurement: {"source_id": "000000000000001f", "tones": true}`. The ID is a
canonical 16-character lowercase hexadecimal string scoped to the original
owned DSP; zero means no measurement source. The same optional object appears
in status. A true tone flag requires a nonzero source and keyed capture.
Legacy absence remains distinguishable from observed provenance.

`POST /v2/aetherpd/capture` has the same admission, capture size and lifetime,
and adds `source: {"kind":"local_dax_iq","stream_id":1073741825,"generation":"7"}`.
These values come from the immutable native capture header. The stream ID is
a nonzero unsigned 32-bit JSON integer; acquisition generation is a nonzero
unsigned 64-bit canonical decimal string. They identify the sampled stream,
not a requested setting. They do not establish a DAX channel, panadapter ID,
antenna, current ownership after the capture, or a calibrated RF level.
Generation is scoped to the original DSP lifetime and can change on a normal
reset; it is not a persistent radio identity. Existing v1 responses remain
unchanged, and v2 captures use the same v1 chunk retrieval endpoint.

The v2 request maps to fixed private opcode `J`, advertised by `PDTALK06`.
The primary returns 501 for an older private session without sending the new
opcode; the old session remains available for v1 capture. New primary/private
session binaries ship in one atomic release. An older primary cannot attach
to a v6 private process. No new native DSP command, socket, privilege or
radio mutation is introduced. The desktop requests v2 first. Only an initial
404 or 501 permits one v1 retry within the original capture deadline. Once v2
is observed on that session, a later response cannot downgrade it. Legacy
captures remain usable with stream identity explicitly unknown.

Receive preparation retains the verified stream ID. Keyed captures must match
that stream; generation may change across RX to TX. Every frame within one
averaged measurement must agree on stream ID and generation, including whether
identity is known. Results retain each capture's identity in JSON and CSV;
the result summary and PNG description identify the reported stream. Neither
channel number nor panadapter ownership is inferred from a stream ID.

`GET /v1/aetherpd/capture/{capture_id}/{chunk}` accepts a canonical lowercase
32-character hex ID and chunk index 0–31. Each JSON response contains the
same `capture_id`, a sample `offset`, and `iq_f32le`: base64 of 4,096 raw bytes
(512 complex samples). Both endpoints require the current admin credential
and return `Cache-Control: no-store`. Requests accept no paths to files,
arbitrary sample counts, rate settings, stream selections or radio commands.

The session owner retains one complete snapshot in RAM for ten seconds after
native retrieval. Beginning another capture discards the old snapshot.
Concurrent begin returns 409; missing, expired or superseded snapshots return
503. Stop closes the snapshot store immediately, and completion of a pending
native read cannot repopulate it. Every public response rechecks credentials
and the original session; a revoked token returns 401 and a replaced/stopped
session returns 503. Old capture IDs never retrieve a newer frame.

The fixed private session command remains 16 bytes with an 8 KiB reply cap.
Opcode `I` collects a snapshot; opcodes 0x80–0x9f retrieve its 32 chunks.
Readiness `PDTALK02` or `PDTALK03` advertises these additions. The caller still accepts
`PDTALK01` for existing control operations but refuses capture locally for
that peer. A newer session returns a fresh random 128-bit public capture ID,
so reused DSP counters cannot make a prior session's ID valid again. The
outer relay validates the requested ID and offset on every chunk response.

Canceling a capture HTTP request leaves the correction session running. Its
bounded pending slot remains until the private reply is drained, preventing
a late reply from being mistaken for a later operation. Calibration/control
cancellation retains its existing session-retirement behavior. Transport
framing errors and explicit session/credential revocation still close the
session.

## Correction selection (development)

`POST /v1/aetherpd/correction/enable` and `/v1/aetherpd/correction/bypass`
accept no body or query parameters and require the current admin credential.
They select correction for the current owned DSP only; no startup environment,
path, gain, frequency, duration or arbitrary radio command is accepted.
`correction_control: true` advertises support. Older native processes and private
sessions are refused without dispatching these commands.

The native private commands are nine bytes: `1` (enable) or `0` (bypass), then
BE64 nonce on the existing source-control channel. They share its monotonic
replay counter and return the same bounded result codes. Admission requires
fresh local IQ, the bound GUI's idle interlock and slice ownership, no keyed
state, no active test source, and stock APD off. An armed/running source refuses
selection. A changed selection invalidates the fit; it never arms or keys.
Repeated commands cannot extend a source lifetime. The default on a new DSP
session remains correction enabled.

Private session `PDTALK05` adds fixed opcodes `0` and `1`; older versions cannot
receive them. HTTP mutation admission serializes with source arm/cancel, and
an uncertain native reply or credential revocation retires that owned lifetime.
Desktop requests invalidate their original capture fence immediately. A receipt
is followed by status readback and never presented as observed correction state.
This control requires matching updated desktop/shim/native packages; installed
radio qualification remains pending until recorded separately.

## Bounded test cleanup API (development)

These authenticated operations create and observe a separate unprivileged
cleanup helper for the current session. They never key the transmitter or
arm the calibration source. The desktop must still obtain its original-operation
TX lease, enforce exclusive admission and preserve qualified RF stop evidence.

| Request | Body | Success |
| --- | --- | --- |
| `POST /v1/aetherpd/test-unkey` | Exactly `{"cleanup_after_ms":15000}`, with an integer from 1 through 15000 | 200, helper observing |
| `GET /v1/aetherpd/test-unkey/{test_id}` | Empty | 200, current observation |
| `POST /v1/aetherpd/test-unkey/{test_id}/stop` | Empty | 202, cleanup requested or helper ended |

Queries are refused. The response contains exactly `test_id` (random 128-bit
lowercase hexadecimal), `state` (`observing`, `retiring`, or `ended`),
`cleanup_due_ms` (canonical decimal string in the radio's CLOCK_MONOTONIC domain)
and `remaining_ms` (integer 0..15000, zero unless observing). Neither `ended`
nor HTTP202 confirms RF release. The native deadline starts before helper
readiness; the public request includes connection and startup time. Polling
never renews the deadline. A desktop cannot compare the radio's absolute clock
directly with its own; it must subtract transport/setup uncertainty and the
cleanup protocol/scheduler budget from the usable interval.

Only one helper may start or remain unreaped at a time (409 when busy). The
server retains only its latest test observation. A stale/foreign test ID returns
404 and cannot stop a successor. Unavailable/stopped sessions return 503 and
revoked credentials return 401. Every response rechecks the original session
and credentials. Canceled or undeliverable begin requests retire that owned
session, ensuring uncertain startup cannot escape its owner. Canceled status
and ID-scoped stop requests drain their private response without closing
correction. A stop can arrive after its original run has been replaced, so its
cancellation must not retire the current session before the run ID is checked.

`PDTALK03` advertises test operations; older peers are refused locally. Legacy
control/capture requests retain their 16-byte `PDCMD001` framing. Test requests
use `PDCMD002` with the same 16-byte header, opcode `B` (begin), `G` (status), or
`E` (stop), followed by a 16-byte binary test ID and a big-endian uint16 duration.
Begin requires an all-zero ID and duration 1..15000; the server generates the
returned ID. Status/stop require a nonzero ID and zero duration. Reserved header
bytes remain zero; the existing request sequence, pending limit and reply cap
are unchanged. Invalid requests close the private session. Request completion
detaches startup cancellation after successful admission; session Stop retains
the run lifetime, so returning a begin response does not cancel or renew it.
Primary shim builds that accept only `PDTALK01`/`PDTALK02` cannot attach to a
version-3 session. Ship the primary and private-session program together in the
same atomic release; mixed-version live upgrade compatibility is not qualified.

HTTP/private-channel integration and cancellation/identity boundaries are tested
with in-memory transport and helper fixtures. These endpoints are not deployed
or live-qualified. The development lease uses the client for these operations;
the complete operator measurement workflow remains to integrate.

### Desktop helper client

`AetherPDTestClient` is an owner-thread, one-test client with an immutable session
binding after begin. It uses the same LAN/token policy as capture, bounded replies,
manual redirect handling and request deadlines. A begin response is followed by
a separate status round trip; long startup cannot supply the fresh observation
needed before TX admission. `remainingMs()` rechecks the original session, the
monotonic clock, the fixed local deadline and a 500 ms observation-age limit,
including when the event-loop timer has not run. Once rejected, this test cannot
become available again. There is no automatic rearm or retry.

The conservative local deadline is request-send time plus the returned remaining
interval. Subsequent status can only shorten it. The radio deadline and test ID
must stay identical, and the reported remaining interval cannot increase. This
early local expiry does not mean the native helper has already begun cleanup.
`cleanupRemainingUpperMs()` separately uses reply-receipt time plus the radio's
reported remaining interval and one millisecond for quantization. Later replies
can only shorten this upper deadline estimate; stale or retired observations
return zero. The lease must fit this later estimate plus cleanup protocol,
clock-rate uncertainty and scheduling allowance inside its requested deadline.
Neither value is an RF-cessation guarantee. Availability checks
are owner-thread only, so the final queued TX gate must use an independently
safe deadline/retirement fence rather than calling this QObject from a worker.

Stop withdraws availability before canceling a pending read. Cleanup retains the
original test ID, address and credentials even after the ownership predicate is
lost; it cannot bind to a replacement test. A valid late begin response retains
only enough identity for that cleanup. `Retiring` is not `Ended`, and neither
state confirms RF release. The owning lease requests stop before destruction;
destruction abandons outstanding HTTP while the native timer remains effective.
Protocol, lifetime, deadline, callback-destruction and transport-failure tests use
injected HTTP/time. Model binding is component-tested. The TX adapter has component coverage; GUI/export and final hardware
qualification remain incomplete.

## Desktop adapter

`AetherPDCaptureSource` implements `ITxCaptureSource` on its QObject's owning
thread, without another worker thread. The owner supplies the selected LAN
address, volatile admin token and a read-only predicate for the original
connection/GUI/credential lifetime. Rebinding, even to the same address,
stops capture and invalidates queued requests. Credentials are neither logged
nor persisted by this source. It uses the same destination/token policy as
the existing AetherPD control client and refuses HTTP redirects.

`RadioModel::bindAetherPDCapture` requires the caller's current control session;
a retired panel cannot bind or disturb capture on a replacement connection.
It delegates to `AetherPDModel::bindCaptureSource`, which binds credentials without exposing
them to the measurement caller and returns the same read-only lifetime fence
for the controller. It requires the registered GUI identity, an explicit
original-session predicate and fresh, available, registered PD status.
RadioModel supplies that predicate from its actual GUI/connection identity.
Control requests, status failures/expiry, credential or session replacement,
and model destruction retire the old binding. Fresh status after an expired
interval cannot revive it, even if the freshness timer was delayed. A caller
must bind again explicitly; binding itself sends no capture or RF request.
The caller must use the returned fence as the controller's session predicate.

`start` validates the fixed frame length and requested center/rate, then waits
with the capture gate closed. A successful `start` is local validation, not
proof of live IQ readiness. The controller must separately prove receive-only
readiness before permitting a transmit test. `checkReceiveReady` starts a
one-shot download while the gate is closed. Its return value only reports
acceptance; its callback reports success after all 32 chunks of a fresh,
unkeyed snapshot pass the same bounds, metadata and finite-sample checks as
measurement data. It does not deliver a measurement frame, open the gate,
key the radio or grant transmit authority. Stop, session replacement or any
gate command cancels a pending check. Errors stop the source and report once.
The owner must still revalidate session, telemetry, operator intent and the
age of the readiness observation before keying; the snapshot does not prove
that the radio remained unkeyed during the subsequent download.

The controller opens the measurement gate after settling.
Each complete transfer must finish within six seconds, match the expected
center/rate, and report that it was captured while keyed. A frame assembled
across a gate close, stop or session change is discarded. Gate reopen begins
a fresh snapshot. Terminal failure reports once and does not retry; successful
frames schedule the next snapshot while the same gate/session remains open.

Only a complete, finite 16,384-sample frame reaches the analyzer callback.
The frame preserves raw amplitudes, capture ID, radio monotonic timestamp
and correction-state evidence. It has no reference channel. Raw full-scale
calibration remains unqualified, so `clippingKnown` is false and must be
carried into `measureTwoTone` and subsequent results; a valid relative IMD
measurement is not proof of absence of ADC overload.

## Measurement source fencing

The measurement model requires an observed armed measurement source before
Prepare. It binds readiness and keyed captures to that accepted source ID;
a rejected or busy arm request cannot change the ID. It accepts only the
fixed 48 kHz capture configuration and 710/1900 Hz tones relative to center,
and requires an allowed allocation covering the alignment-noise band from
center minus 4.5 kHz to center plus 7.5 kHz. Changing or canceling the source
retires the original preparation and prevents later key admission.

After original-owner TX readback, the adapter polls status within the same
eight-second measurement capture deadline until the bound source reports guarded tones.
It then requires matching provenance on the complete captured frame. Polling
cannot extend the deadline or arm/key/retry a source. Stop and replacement
fence delayed polls and responses.

The native tone window begins after an emitted tone packet plus the configured
maximum alignment search delay (400 ms by default) and a 100 ms settling guard.
It compares that boundary against IQ kernel-arrival time, and requires fresh,
aligned, nonfaulted local IQ while the original source is running. Noise,
the final fade to silence, stale/queued IQ and source expiry are excluded.
These are local timing guards, not FPGA timestamps or independent analog
settling/ADC-headroom proof; the guard needs live hardware qualification.
A partial frame crossing a boundary is rejected; a previously complete valid
frame remains available for bounded retrieval after unkey.

The controller and composed model cap transmit authorization at ten seconds
from the operator's start, including helper startup. A telemetry-age override
cannot raise that ceiling. The helper cleanup interval is at most nine seconds;
the generated source remains bounded to eight seconds. Cleanup, command
acceptance and confirmed release are separate: the controller waits for
qualified release without extending authorization or collecting more IQ.
This is not a ten-second physical RF-off guarantee under process scheduling
stalls, delayed API responses or unknown ownership. The exact candidate's
capture completion and physical release timing still require live qualification.

## Verification and remaining integration

`pd_capture_test` is a socket-free CTest using the actual waveform packet
handler plus continuity/expiry/metadata cases. `status_test.c` separately
checks private socket admission and exact command lengths. Go's capture and
status tests exercise parsing, cancellation and chunk consistency under the
race detector.

For explicit C/Go integration, compile `capture_fixture.c`, `pd_capture.c` and
`pd_status.c` into a host executable. Set `AETHER_PD_CAPTURE_FIXTURE` to its
absolute path and run `go test -race -run TestPrivateAetherPDCaptureNativeFixture`
in `tools/flex-tailnet-shim`. The fixture uses only an inherited socketpair
and generated sample bytes; it is never included in an installed package.

`TestPrivateAetherPDCaptureNativePublicFixture` exercises the real C capture
protocol, Go reader, private session server/channel and authenticated HTTP
handler through all 32 chunks. It uses generated data and explicit fixture
lifetimes, not a running radio or a proof of real DSP process supervision.
Separate tests cover revocation, replacement, expiry, stop during collection,
concurrent begin, malformed public requests and legacy-peer refusal.

The desktop `AetherPDProtocol` parses manifests and chunks with fixed size,
field, capture-ID, offset, canonical base64 and finite-sample checks. Its
socket-free test can consume the actual Go HTTP response fixtures by setting
`AETHER_PD_CAPTURE_FIXTURE_DIR`; the same variable on the native public Go
fixture test writes the synthetic responses to an existing directory. This
verifies C/Go/desktop wire agreement without claiming a live desktop download.

The socket-free adapter test injects only HTTP replies and exercises complete
frame delivery, late replies, gate/rebind/stop, invalid metadata, authorization
failure, bounded responses, deadline, transport destruction and callback
destruction. Receive readiness tests require the final chunk before success,
reject keyed snapshots, and cancel on stop, gate commands or session changes,
including loss of ownership immediately before the final reply. Downloaded
synthetic two-tone IQ with known -40 dBc IMD3 is fed
through the actual spectrum/IMD analyzer, preserving the unknown-overload
flag. This does not replace measurement on the radio.

## Measurement controller

`TxLinearityController` implements the local measurement lifecycle, with
injected session ownership, monotonic time and per-reading telemetry. Prepare
does configuration/band/cooldown checks and the receive-only download; it
never keys. An explicit `startRun` must arrive within two seconds of readiness
completion. It rechecks ownership, fresh telemetry and idle power, and requires
an `ITxLinearityRunLease` for that operator-authorized transmission. The lease
must arm a non-renewable independent cutoff before dispatching through the
existing TX gate; the controller's 25 ms timer is not that cutoff.

**The development `AetherPDRunLease` has component tests and two successful
normal-release runs in the installed FLEX-8600 measurement UI. Final keyed
failure and independent-cutoff qualification remain required.** The controller unit tests inject a
lease; the model integration test exercises the real adapter and TX gate. Its
`Released` observation must confirm release of the original
TX operation; successful enqueueing of an unkey command is insufficient. The
controller retains the lease and stays in Releasing until confirmation, even
after a source/session failure. Production integration must expose that pending
state and keep the independent cutoff effective across GUI stalls or destruction.
The installed `pd-unkey-guard` uses its session lifetime pipe. A development
[bounded mode](guardian/README.md#bounded-measurement-mode-development) adds
a fixed cleanup-initiation timer, exposed through the development API above and exercised by the installed
FLEX-8600 measurement workflow. It does not
by itself establish an RF-cessation deadline. The automation watchdog polls
on the GUI thread. Neither alone satisfies this independent cutoff contract.
The existing independent TX coordinator and Flex ordered stop evidence are
the ownership/release paths used by the adapter; a global
MOX bit or an unkey-command acknowledgment cannot replace that evidence.
`TxCoordinator::Operation::hasConfirmedStop()` retains the result of the
qualified stop-confirmation path for that exact operation. Local completion,
transport teardown, expired/revoked certificates and an entered terminal writer
cannot publish it. Copies retain the historical result across later operations;
it grants no authority and says nothing about a later transmission. The
measurement lease consumes this exact-operation evidence when reporting Released.

The Go per-test launcher validates bounded readiness in the radio's monotonic
clock domain and owns a separate lifetime pipe. It cancels startup before
readiness and requests cleanup on later cancellation. Ended or expired helpers
cannot become available again. The session supervisor serializes startup through
child termination, closes pending/active test lifetimes on session Stop, and
includes them in shutdown waiting. Its production start method prepares a fixed
loopback API connection bound to the original GUI identity. These components
are host-tested through authenticated test operations; the connection factory has also been exercised by the two installed
FLEX-8600 runs, without fault injection. The lease keeps the original intent through startup and must
budget cleanup and scheduling time, and refuse replacement while any previous
helper remains unaccounted for.

`AetherPDRunLease::create` binds a fresh helper without starting it. The explicit
`start` call admits one bounded MOX operation through the existing independent
TX grant manager and captures its `TxController::Input` before helper startup.
The deadline uses `TxCoordinator::monotonicMs()`; the controller environment must
use that clock too. The helper must pass its separate verification round trip
before that original input is dispatched. There is no retry or source rearm.
The lease does not create tones: source selection/arming remains a separate,
explicit operation before measurement preparation.

The lease rejects an authorization deadline more than ten seconds away.
It reserves one second for startup and requires at least a one-second usable
keying window. After a separate verification request, the conservative upper
estimate of the helper's cleanup start must fit before the original deadline.
The dispatch gate additionally expires at the earlier conservative usable
helper deadline. Native ownership refresh and unkey writes remain best effort
after expiry; they cannot authorize a new key or certify RF cessation.
Actual startup, capture completion and release timing require qualification,
especially on Linux 2.6.

The worker-safe gate captures only atomic lifetime/deadline state. After helper
verification its deadline can only shorten to the helper's conservative usable
window, so a stalled GUI cannot dispatch late keying or extend the original
operation. Session/helper loss requests cleanup early. Unknown ownership stays
AwaitingKey; a reported foreign owner aborts. Keyed requires the original active
operation and radio TX readback naming the original GUI owner. Helper termination,
unkey requests and connection teardown never substitute for qualified release.
The controller keeps release pending when that proof is unavailable.

The measurement gate opens only after observed keying and settling. Per-reading
telemetry, selected forward-power limit, deadline and original ownership are
checked during polling and before accepting each frame. Capture metadata must
remain consistent; duplicate IDs, non-increasing timestamps, correction-state
changes and malformed samples abort the run. Memory holds at most 32 fixed
16,384-sample feedback frames (4 MiB of IQ); reference buffers are not copied.
The controller requests release before stopping capture and publishes nothing
until release is confirmed. It then performs FFT/IMD analysis on the owner
thread, with no RF interval held open for analysis. The bounded analysis cost
and UI responsiveness still need measurement in the complete desktop workflow.

Telemetry refusals write one `TX linearity safety abort` JSON record under
`aether.waveform.measurement`, after requesting release and stopping capture.
It preserves the decision's reading values, original sample timestamps, ages,
limits and pre-key/awaiting-key/keyed phase. Missing/nonfinite values and
unavailable ages remain explicit; release callbacks cannot replace the captured
sample with a later meter value. The record does not confirm RF has stopped
or establish whether a radio-reported SWR value is physically accurate.

Results retain capture IDs/timestamps, correction state, any observed clipping
and uncertainty about clipping. `fullScaleKnown` remains false for this raw
path; absolute levels must not be labeled calibrated dBFS or watts. The two
PEP conventions remain separate. Measured fundamentals must match the requested
test tones within two FFT bins, otherwise the measurement is marked invalid.

After confirmed release, completed results also retain the bounded feedback
frames used by the analyzer. The JSON export adds `raw_iq_format: f32le-iq-v1`
and per-capture `sample_count`, base64 `iq_f32le` and `sha256` of the decoded
bytes. Samples are interleaved little-endian float32 I,Q, without normalization
or windowing. The checksum detects changed bytes; it does not authenticate a
measurement. The model discards its previous result on a new preparation.
Serialization occurs only on explicit export, after the RF operation is over.

`python3 tools/pd-waveform/recheck_iq_export.py result.json` independently
recomputes the BH7 spectrum and IMD ratios with NumPy. Older exports without
IQ are refused. Optional `--baseline off.json` requires matching station and
analysis settings, opposite correction states and fundamental power within
0.5 dB before reporting improvement. This numerical check does not establish
ADC headroom, absolute RF power, receiver linearity or sustained correction.

Socket-free controller tests cover confirmed-key settling, stale per-reading
telemetry, power/deadline/ownership loss, late frames, reentrant stop/release,
destruction, cooldown and metadata consistency. An integration test drives
the real HTTP adapter through unkeyed readiness and a synthetic -40 dBc IMD3
capture into the controller/analyzer; results remain withheld until the
injected lease confirms release. These checks do not prove a hardware cutoff.

Both capture and test-helper clients bind through RadioModel's original
control-session guard. AetherPDModel supplies the same session/status fence
construction to each client and its caller; credentials stay inside those
components. Binding an unused helper sends no request. Binding is refused
without fresh registered DSP status. A used helper cannot be rebound or
stopped by a binding attempt, and a stale panel cannot change a successor's
binding. Session replacement, GUI identity loss, control changes and status
expiry invalidate existing fences permanently. Cleanup after invalidation
retains only the original test ID, address and credential.

`AetherPDMeasurementModel`, owned lazily by RadioModel, composes those bindings,
the capture adapter, radio-meter telemetry, controller and real TX lease. The
controller persists across preparations so closing/reopening a panel cannot
reset its cooldown or discard pending release. Prepare requires an explicit
dummy-load confirmation, FDX off, a current TX slice and radio-confirmed nominal
power within the selected limit. It snapshots the frequency, mode, TX antenna
and power; changes cancel preparation or the running test. Even an unchanged
power request retires preparation when its radio-confirmed provenance is lost.
The helper callback rechecks the original preparation before dispatching TX.
Neither Prepare nor construction starts a helper or arms a tone source.

The composed workflow is exercised with actual RadioModel/controller/lease code
and injected HTTP, meter samples and terminal writers. Tests pass a complete
unkeyed snapshot, explicitly start, inject original-owner TX readback, download
a synthetic -40 dBc IMD3 frame, and withhold its analysis until qualified stop
evidence. They also exercise frequency and power changes, cancellation during
helper startup, destruction during capture, over-power abort and retained
cooldown. These are component results, not firmware or RF-cutoff qualification.

The development operator window now composes explicit source arm, receive
preparation, scoped transmission, stop and atomic JSON export. LP-100A integration
is deferred by the operator;
the current model consumes radio telemetry only. The two installed FLEX-8600
runs completed four captures each, with logged key-to-unkey command intervals
of 7.789 seconds cold and 6.104 seconds warm. Both used a 5 W setting; sampled
forward power peaked near 3.41 W. These timings do not establish an independent
RF-cessation bound or power calibration. Matched correction-off/on measurements,
raw-IQ cross-checks and final failure tests remain;
actual Linux 2.6/Cortex-A8 qualification remains required. Validate the raw
DAX IQ full-scale convention before interpreting magnitude 1.0 as ADC
clipping; relative spectral ratios alone do not establish that calibration.


## Development operator window

**Measure linearity…** opens a separate canon window from AetherPD. It uses the
radio-owned measurement controller, retains pending release across window close,
and snapshots JSON before opening the asynchronous save dialog. QSaveFile commits
the result atomically. Export contains raw-IQ spectrum units and explicitly named
per-tone, fundamental-derived PEP and sampled-peak references, with noise-floor
and clipping uncertainty. Nominal radio power is separate from the measured
forward-power abort limit. No IQ absolute-scale calibration is inferred.

Preparation derives fixed source tones and center from the current TX slice,
and snapshots the selected window, averages, IMD order and authorization timeout.
The per-radio `AetherPDMeasurement` schema2 document stores those client choices.
Schema1 documents retain their ten-second default until an explicit save migrates
atomically to schema2; unknown fields survive. Schema2 requires an integral
`timeoutMs` from3000 through10000 in1000ms steps. Missing/malformed values and
future schemas refuse use without overwriting data. Older schema1-only clients
refuse schema2 instead of silently ignoring a shorter stored timeout.

The timeout control locks during preparation, capture and release. The controller
uses the shorter of the selected timeout and its safety-policy limit; callers
cannot extend an in-flight snapshot. JSON, CSV and PNG descriptions retain the
selected timeout separately from confirmed release. Short choices can expire
before calibration or capture completes and do not authorize another attempt.
Allowed regions come from the selected BandPlanManager plan
and operator license-class filter. This is a software preflight, not permission
to transmit; the normal radio TX gate still applies. Changing plan selection or
license filter invalidates the original preparation.

The marked Transmit control supplies a scoped automation action. It captures
both the original input and preparation ID before queueing. Revocation or a new
preparation rejects the old callback. That input lifetime remains part of the
model/lease fence through helper startup and the run. The independent helper's
bounded cleanup remains required; the bridge's desktop watchdog alone does not
qualify the independently granted measurement operation. Closing requests stop
but does not claim RF release or stop correction. An unused arm remains bounded
by its original 30-second expiry unless explicitly canceled.

The two finite installed FLEX-8600 runs completed four-frame transfer within
the tone window and reached confirmed backend release. Cold completion was
close to the eight-second source limit; scheduling/network variability and
actual FLEX-6300 behavior remain unqualified. The record preserves the failed
cleanup audit: its single-primary expectation was incorrect because Stop
retains an unprivileged status channel. The DSP/capture workers and temporary
ARP entry were gone; three control processes remained with zero capabilities.

After all frames have been accepted, normal unkey cancels the bounded source.
The model therefore permits the same source to be ended while waiting for
qualified release and analyzing the retained capture. Its ID, radio session
and station metadata must still match. Ended sources remain inadmissible for
preparation, keying and further capture.
