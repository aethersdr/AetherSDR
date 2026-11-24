# Flex receive data behind the backend seam

This M4 increment implements #6873 under #3849/#5262. It moves Flex spectrum,
waterfall, meter normalization and transport telemetry through `IRadioBackend`.
It does not add remote streaming, a new thread, a capability, or a radio command.

## Delivery and lifetime

`PanadapterStream` assembles complete frames on its existing UDP worker.
`FlexReceiveData` owns bounded pending observations before the first Qt queued
hop. `FlexBackend` emits typed signals on its owner thread; `RadioModel` retains
its existing `panFeed*` signals and derived meter behavior. The native-backend
`spectrumFrameReady`/synthesized-waterfall route stays separate. Demo's legacy
producer and Flex audio, DAX and IQ transport stay on their existing paths.

A session lease parents stream and meter-definition leases. Removal, definition
identity/unit changes, session retirement and destruction revoke old payloads,
including a batch already detached from the queue. Identical definitions retain
queued samples. Full-width unsigned stream IDs remain opaque. A late wire packet
that is indistinguishable from a reused wire ID cannot be identified as old by
this client-only fence; it protects locally admitted work, not invented wire
session identifiers.

| Pending work | Limit and overflow behavior |
|---|---|
| Spectrum | Latest complete frame per stream |
| Waterfall | Up to 32 ordered rows per stream; discard oldest rows, retain radio timecodes |
| Display total | 256 entries / 8 MiB retained vector allocations |
| Meter total | 128 whole packets / 1 MiB; evict oldest complete batches |
| Observation age | Discard queued display/meter data older than 500 ms |
| Owner drain | Up to 16 entries, alternating display/meters, plus one latest telemetry snapshot |
| Admission | 64 streams per display plane, 65,535 bins, 4,096 meter definitions |

The queue posts at most one pending wake, tagged with its session generation.
Nested event loops cannot start a second drain before the detached batch ends.
No queue mutex is held while emitting signals. Consumers recheck leases after
reentrant callbacks. MeterModel also ends a batch when a callback clears or
redefines its metadata. Receipt time is captured before queueing: monotonic time
controls freshness; UTC remains the public observation timestamp. The daemon
projection preserves the original age instead of restarting it at delivery.

## Diagnostics and compatibility

A 250 ms worker snapshot preserves cumulative transport bytes, packets, VITA
sequence errors and traffic-category counters. Endpoint and timing observations
are retired with the session. It does not enter the TCP-ping/heartbeat path.
The troubleshooting snapshot adds `network.receive_delivery` queue replacement,
drop, rejection and current/peak byte counters. Those are application delivery
statistics, never added to VITA loss or used to change adaptive FPS policy.

FFT dBm calibration, decode-scale generation, waterfall intensity/frequency
bounds, radio auto-black values, meter scaling and derived-meter thresholds are
preserved. The decoder accepts signed near-DC waterfall overhang. Live firmware
qualification is RX-only; testing one Flex model does not limit the shared Flex
protocol implementation to that model.

## Verification

`flex_receive_data_test` covers capacity and count bounds, service fairness,
normalization, definition revision, retired producer leases and wake generations.
`flex_receive_path_test` drives the production decoder on its worker without
opening a radio transport, observes the owner-thread seam/model relays, and checks
reentrant retirement, ID reuse, receipt age and telemetry/ping separation.
`control_telemetry_test` covers receipt-aware daemon expiry. The focused tests
join the normal full/sanitizer graph without expanding the frozen PR test gate.

Positive session/display/meter/audio convergence is qualified using the private
MCP automation bridge and slice-0 RX harness against real Flex firmware on macOS,
Linux and Windows; build/test results and limitations belong in the PR evidence.
