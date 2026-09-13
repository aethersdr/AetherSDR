# Producer PCM contract (RFC #5468 A1)

`PcmFrame` in `src/core/PcmFrame.h` is the in-process RX producer envelope.
It is not an aetherd wire resource or a sound-device format. A1 keeps the current
24 kHz stereo audio paths active. The new RTL WDSP 48 kHz producer remains disabled;
A2–A5 must qualify playback, recording, TCI and fixed-rate decoders before M1/S2
activate it. No radio capability or default changes in this step.

## Format, identity and ownership

A frame carries owning, native-endian interleaved float32 samples, an immutable
24/48 kHz mono/stereo format, and separate source, connection session,
format-generation, receiver-instance and stable slice-slot identities.
Speaker mixes and auxiliary sources have no slice slot. A slice stream accepts
F4 `Handle::session`, `Handle::instance` and `Handle::slot` as separate values;
legacy adapters use process-local identities until the native producer lands.
These identifiers are not persistence keys. DAX channels and Kiwi source IDs
travel alongside their frame and retain their existing meanings.

`firstSample` counts sample frames (LR pairs for stereo), not floats or bytes.
It advances only on accepted input. A start or accepted format change resets its
origin and marks the first frame discontinuous. Backward positions, overflow and
repeated or older explicit
sessions are refused. Legacy adapters count accepted output samples; this does
not reconstruct missing capture timestamps or establish multi-receiver alignment.

Only `PcmProducer` constructs valid frames. It rejects empty, oversized (more than
65,536 sample frames), incomplete-channel and nonfinite input. Finite values,
including signed zero and peaks outside ±1, are preserved without clipping.
Qt queued delivery copies the samples and metadata together with shared ownership;
borrowed buffers are detached at publication. Consumers have no mutable access.

An epoch token contains immutable metadata and an atomic live bit, with no radio,
worker, DSP or QObject ownership. Stop, reconnect, format change and slice retirement
revoke old tokens. A queued frame stays invalid even after the same slot or rate is
reused. `PcmFrameGate` independently rejects stale, replayed and out-of-order frames
for each consumer, with at most 32 live stream cursors. Inactive entries are reused.

The gate is deliberately one-sided: it refuses a frame at or behind the cursor and
admits one ahead of it. A forward gap means the consumer missed frames, which every
consumer that can be detached from a running producer does legitimately — playback
mute detaches the Flex speaker feed and short-circuits the seam-backend feed while
the producer keeps counting. Because a cursor only advances on an accepted frame,
refusing that gap would strand the consumer behind a live epoch with no way back,
silencing RX until the next reconnect. Replay protection comes from the backward
check plus epoch liveness, neither of which a forward gap weakens.

Each producer and consumer gate has one execution context. Only token revocation
may overlap production/delivery; start, format change and destruction remain
serialized with the producer, and its caller joins it before destruction. A
revocation rejects a later admission; it cannot retract samples already admitted
into an existing AudioEngine processing/device queue. Queue flushing and rate-domain
transitions are A2 work. A1 does not claim bounded end-to-end latency or lock-free
real-time performance: it adds ownership copies and finite-sample validation.

## Compatibility boundaries

`legacyStereo24()` refuses revoked frames and every format except 24 kHz stereo.
It copies sample bits unchanged; it never converts or relabels 48 kHz or mono input.
Fixed-rate consumers unwrap at the receiving callback, after queued delivery.

| Producer / route | A1 adapter and retained behavior |
|---|---|
| Flex LAN float/reduced-bandwidth/Opus | `PanadapterStream` publishes typed speaker PCM after the existing decode/concealment. Float alignment/finiteness is checked before concealment history. DAX registration owns revocable per-stream tokens. |
| Hermes-Lite | The current DSP instance is checked before accepting queued worker output. Per-slice validation precedes the existing mixer. Its unity fast path, gain/balance, alignment and sum/clamp behavior stay unchanged. |
| ANAN | `AnanRxDsp` tags its actual configured output rate before worker-to-owner delivery and revokes old tokens on channel installation. The production backend still requests 24 kHz, with the existing single-receiver speaker and slice outputs. |
| Icom | The current session instance is checked; malformed/nonfinite mono input is refused before the existing 48 kHz mono to 24 kHz stereo converter. |
| RTL | The current legacy worker is checked before publication. Existing DDC/demodulation, 24 kHz output, WFM behavior and slice-0 runtime remain in use. F4 preparation/registry is not activated as the audio producer. |
| Simulator | Worker output has a connection epoch before queueing; the backend checks that epoch against its session before publishing. Existing pacing and speaker/slice samples remain unchanged. |
| Kiwi | Existing resampling, silence, squelch and loss padding precede typed publication. Socket cleanup revokes queued audio; AudioEngine has an independent auxiliary ingress gate. |

`IRadioBackend` publishes typed speaker and per-slice signals. Its compatibility
publishers initialize on `connected`, revoke on `disconnected`/slice removal, and
refuse further publication from retired streams. `RadioModel` retires them before
backend teardown and uses the same guarded PCM bindings for production and test
injection. No new backend-family decision is introduced.

The existing single-producer choices remain: Flex stream playback, backend-owned
speaker playback, and the simulator's existing direct speaker route. The normalized
`rxDemodAudioReady` bus remains a separate subscriber for recording/CW/RTTY. Per-slice
TCI/AetherClock, Flex DAX/RADE and concurrent Kiwi routes retain their attribution.
Every production route uses the typed signals. `PanadapterStream`'s byte-valued
`audioDataReady`/`daxAudioReady` are removed rather than retained: after the
migration nothing in the tree connected to them, their arguments were still being
deep-copied per audio block, and they bypassed the gate — so any later consumer
wired to them would silently have skipped admission control.

No A1 adapter sets `discontinuity` after the first frame of an epoch: each one
publishes contiguous positions, so a lost Flex UDP audio packet is presented as
continuous even though packet-loss concealment detected it. Propagating that would
make AudioEngine retire and reset the chain on every lost packet, which is an
audible-behaviour change rather than a wiring fix, so it belongs with the later
milestones that own playback policy. Today the field is exercised by tests and by
explicit-position producers only.

AudioEngine's original byte APIs remain for legacy internal/playback callers. Its
new typed entry points reject incompatible formats and duplicate/stale frames,
then call the existing processing functions. The main RX L/R resamplers, TX CW
sidetone adapter, decoded RADE speech adapter and device-rate state are unchanged.

## Validation scope

`pcm_frame_test` covers format, ownership, bounds, identity, continuity, queued
revocation and replay/capacity rules without sockets or devices.
`pcm_compatibility_test` injects the production backend/model/AudioEngine route,
Flex decoders/DAX registration, and the Hermes mixer without a firmware peer or
hardware connection. Existing simulator and ANAN DSP tests cover paired output
and actual producer rates. Tests are registered in `tests/tests.cmake`; no frozen
PR CI allow-list is expanded. Build, sanitizer and mutation results belong in the
A1 execution report, with their host/configuration and hardware limits.
