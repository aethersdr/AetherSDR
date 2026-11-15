# RTL prepared receiver ownership (RFC #5468 F4)

`RtlReceiverRegistry` is a compiled engine foundation for the
[approved RTL multi-RX RFC](https://github.com/aethersdr/AetherSDR/issues/5468).
F4 is the local plan ID for the RFC's Foundation-stage
"testable capture/processing lifecycle" bullet.
Its default preparation function creates real `WdspChannel` receivers and
preallocates their output buffers. It represents multiple receivers from the
start and enforces their admission, session identity, publication and retirement.

The live `RtlSdrBackend` still selects `RtlSdrDdc`, with its existing single
receiver, WFM behavior and audio rate. F4 does not enable another receiver or
replace the current demodulator. M1 supplies RF extraction, paired I/Q conversion,
fixed-block accumulation and aligned mixing before selecting this foundation;
the A-series audio changes must precede a new 48 kHz producer. Viewport plans
and filters also remain with their later feature. This boundary avoids shipping
a partially converted receive path. There is no persistence-owner cutover,
new UI, new dependency or new Qt/acquisition thread per slice here. Each
`WdspChannel` still owns WDSP's existing internal execution threads.

## Owners and identity

Each registry owns one capture endpoint per session. `beginSession()` returns
a nonzero process-unique session token. The control side reserves stable UI
slots through `reserveSlot()`, optionally naming a particular slot. A handle
contains three distinct fields: session token, slot number and slot-instance
generation. WDSP's process-global channel number is private to `WdspChannel`
and is never a UI or settings key.

Capture identity and capture generation remain the F2 descriptor fields.
A capture generation identifies the complete center/rate/usable-interval
readback, not an independently mutable collection of controls. Reusing a
generation with different metadata is refused. Slot/session counter exhaustion
refuses allocation rather than wrapping identities.

A prepared target can precede M1's hardware transaction. A failed future-capture
preparation therefore does not invalidate the capture still in use: an older
descriptor is accepted only when it exactly matches an `Active` bank in the
same session. Every other older or modified descriptor is refused. The newest
requested generation remains the high-water mark even when that exception is
used. M1 remains responsible for actual hardware readback and transaction order;
preparing a target is not evidence that hardware has adopted it.

A complete desired set is submitted as bounded `ReceiverSpec` values. Each
spec carries its reserved handle, F2 guarded RF passband and WDSP configuration.
Pending and live reservations together respect the receiver capacity, even
when the addressable UI slot range is larger. A removed live handle remains
charged until its bank is actually retired and destroyed, so failed-removal
retries retain the complete still-active set.
Admission validates all handles, distinct slots, bounded DSP buffers and
parameters, and whole-set passband containment within the fixed capture using
the production `SharedCapturePolicy` helper. Its center domain is deliberately
the singleton supplied center; this does not validate tuner range or grid
legality. It never recenters a capture or adjusts a sibling.
Derived DSP input/output block sizes must be positive and integral, and the
exchange input/output rates must have an integral ratio. This prevents WDSP
integer truncation from creating zero-sized internal buffers or incorrect
exchange lengths.
WDSP blocking-output mode is refused because these receivers run in the
acquisition context.

An omitted handle remains valid while an old bank references it. This matters
when preparing a removal fails: the old bank and its complete set of handles
remain usable for a retry. An omitted slot can be reused only after its last
pending/preparing/offered/active/retiring reference is gone. Reuse advances its
instance generation; siblings keep their handles. `cancelReservation()` is only
for an unused reservation. Removing a live receiver is a new complete desired
set, not destruction through that method.

## Bounded process-wide preparation

All registries share one executor over the existing Qt global worker pool.
It retains at most one executing preparation/destruction operation and one
coalesced pending desired set. A newer request from the pending set's owner
replaces that set. A different owner receives `Busy`, preserving the other
capture's work. Independent sessions are never silently coalesced together.
Callers retain their desired request and retry a `Busy` refusal through their
control event loop; the registry does not create an unbounded retry queue.

The executor retains at most four registry contexts, including closed owners
whose jobs or sample readers have not acknowledged retirement. A fifth
constructor returns an invalid registry. This is deliberate backpressure under
repeated family swaps or stalled readers. There is no detached-task list that
grows each time a backend is destroyed.

A job owns its state and preparation callable; it never captures a backend
QObject or accesses a `QPointer` across threads. Destroying the registry or
canceling a session invalidates pending/results immediately and returns without
waiting for FFTW. An already running FFTW call is allowed to finish, then its
obsolete result is destroyed on the worker pool. The same executor survives
replacement registry objects, even when endpoint metadata is identical.

A preparation callable that throws a standard or non-standard exception reports
`PreparationFailed`. Its partial bank and reservations are retired off the
callback, while the active bank remains usable and later requests can proceed.

Each registry has three fixed bank positions: preparing/offered, active, and
retiring. It cannot accumulate another prepared bank behind an unconsumed
offer. With a reader attached, the next block acknowledges the stale offer;
without a reader, the control path may retire it directly. Thus startup may
wait for the newest prepared result before acquisition begins. `service()`
on the model/control event loop reaps acknowledgment pressure and advances
pending work. No callback posts an event or schedules a task.

## Resource accounting

Limits are constructor inputs: addressable slots, receiver capacity, and total
resident receiver capacity. The latter includes preparing, offered, active,
retired and currently destroying receivers. Slots do not become free merely
because destruction was queued. Replacing an active bank requires sufficient
capacity for old plus new; refusal preserves the old bank. For example, a
capacity of four and a resident budget of eight permit a complete four-channel
replacement after earlier retirements have actually drained.

Eight UI slots and four retained registry contexts are representation ceilings,
not advertised receiver counts or performance claims. Architecture-specific
limits still require the RFC's measured integrated evidence. Buffer/rate/filter
ceilings also bound individual receiver allocations; they do not qualify every
accepted DSP configuration for real-time operation.

`WdspChannel::reserveChannels()` atomically reserves the entire new bank from
the **same 32-slot allocator** used by ordinary `WdspChannel::create()`.
It performs no FFTW planning. Other radio backends and TX channels continue to
compete for those actual slots. A failed batch takes none; unconsumed slots
return by RAII; successful reserved creation transfers one slot into the
channel's existing lifetime. Destruction returns that slot only after the
channel has really closed. There is no independent RTL pool counter that can
overcommit WDSP. Local resident-limit and shared-pool failures are distinct
observable preparation errors.

This is a deliberate public API addition to the shared DSP class. A caller can
reserve all 32 slots without creating channels, and the reservation has no
timeout or per-backend quota. Callers must release reservations promptly when
preparation is canceled or fails; other backends receive pool exhaustion while
those slots are held. Maintainer review must explicitly consider that API scope.

The normal WDSP setup lock, production wisdom import and atomic export remain
unchanged. F4 sets no planner time limit. The repository's existing isolated,
bounded test-planner setup applies to the registered correctness tests; those
results are not production cold-start measurements.

## Sample publication and retirement

`attachReader()` creates the sole acquisition-side reader before acquisition
starts. It can outlive the registry object. `processBlock()` consumes a borrowed
normalized complex-float IQ span with an explicit session token, full capture
descriptor, first-sample position and discontinuity flag. The source captures
these tokens with the acquisition configuration; it must never relabel old IQ
using the registry's latest session or capture values.

Block delivery validates the session, every descriptor field, bounded nonempty
length, sample-position arithmetic and finite I/Q values. A prepared bank can
be adopted only at entry to a valid block whose descriptor matches exactly.
A bank for a future capture generation waits while the old bank continues
processing matching old-capture blocks. After adoption, old-capture blocks
are refused. First delivery, bank changes, explicit discontinuity, position
gaps and refused input all mark the next delivery discontinuous; M1 uses this
to reset extraction/accumulation histories and align output.

The synchronous `BlockProcessor` receives the block plus borrowed receiver
views. A view may be used only during that call and never by another thread.
`Receiver::processIq()` accepts the prepared WDSP configuration's exact planar
input block size and returns borrowed preallocated L/R output spans. F4 does
not mistake full-rate RTL IQ for already extracted WDSP input; M1's extraction
step belongs between the block and that processor.

The sample method uses fixed stack storage and lock-free atomic state only.
It does not allocate, take a mutex, copy/release a shared pointer, plan, change
filters, invoke a preparation callable or destroy a receiver. Preparing a
replacement cannot mutate the bank a callback currently borrows.
Finite-value validation performs an O(N) read of up to 65,536 complex samples
after the metadata checks, even if no active bank or usable offer remains.
M1's extraction will read that IQ again; integrated callback-budget measurements
must include both passes. Allocation freedom alone does not establish headroom.

On the next sample boundary, the reader adopts the complete ready bank and
marks the previous bank `Retired` only after its preceding synchronous call
has returned. The pool observes that release/acquire acknowledgment before
moving the old bank to `Destroying`. Its actual destructors run off the sample
path, and only then does the position become `Free`.

If no next callback will arrive, the acquisition owner must stop/join its
sample context and call `SampleReader::stop()` outside the callback. `stop()`
acknowledges the active and offered banks and cancels the session, including
pending/running preparation results. The reader's destructor follows this same
path. Reader moves, stop and destruction must not race `processBlock()`; control
methods may run concurrently with processing but must never be called from the
processor. A stopped or destroyed reader cannot silently leave a bank waiting
for a nonexistent next block. A still-running reader must never be destroyed
as a timeout workaround, just as a live librtlsdr handle must never be closed.

## Validation and integration obligations

The default graph registers two socket-free targets even in an RTL-disabled
build:

- `wdsp_channel_reservation_test`: atomic batch refusal, moves, rollback,
  ordinary allocator competition, real reserved/ordinary channel creation,
  and exact slot return.
- `rtl_receiver_registry_test`: injected delayed/failing preparation, immutable
  held sample views, stable slot reuse, failed-removal retry, session/capture
  rejection, coalescing and independent-owner refusal, no-reader startup,
  publication pressure, resident/pool bounds, owner destruction/recreation,
  same-owner reconnect, stopped acquisition, retained-context ceiling, and
  actual default WDSP preparation/processing, and recovery after standard and
  non-standard preparation exceptions. CTest bounds each target to 120 seconds.

The registry tests establish these ownership contracts, not reception quality,
GUI convergence or race freedom on their own. Run the production units under
the sanitizer lane; TSan needs the project's instrumented Qt build. M1 must
add production caller/wiring tests, actual RF extraction and variable-block
DSP tests, normalized lifecycle/error routing, USB readback transactions,
audio integration and the approved hardware/bridge/radiocert gates. Native
Linux aarch64 runtime/performance and per-architecture advertised limits remain
independent release requirements.

M1 must validate requested and returned capture centers against the actual
tuner's supported center domains and grid before supplying descriptors here.
The registry's singleton center domain is not hardware-domain validation or
evidence of a successful tune; actual USB readback and capture transactions
remain the integration owner's responsibility.
