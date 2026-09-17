# Typed TCI receive audio (RFC #5468 A4)

TCI receives independent pre-monitor slice PCM from `RadioModel` and independent
Flex DAX PCM from `PanadapterStream`. Neither route subscribes to the speaker mix,
so PC Audio, speaker gain and speaker mute do not gate these streams. Native
backend slice IDs resolve through `TciTrxMap`; a sparse slice ID is not a DAX
channel or public receiver index. The Flex DAX route retains its channel gain and
slice binding independently.

## Formats and conversion

Producer metadata travels with each owning `PcmFrame`: source, session, format
generation, receiver instance, slice ID, sample rate and channel layout. Native
slice input supports 24/48 kHz mono or stereo. Flex DAX retains its 24 kHz stereo
contract. A mono producer is duplicated into L/R; stereo channels have independent
converter histories. Only a client explicitly requesting mono is downmixed.

Each client keeps its own rate and per-source converter. Wire negotiation accepts
only 8/12/24/48 kHz, as listed for `AUDIO_SAMPLERATE` in the official
[TCI 2.0 specification](https://github.com/ExpertSDR3/TCI/blob/b081213ff97150fd29f669c633f060f93c81a286/TCI%20Protocol.pdf).
Unsupported requests, including 44.1 kHz, retain and echo the prior accepted rate
without discarding staged audio. The default remains 48 kHz. Internal converter
tests also cover 44.1 kHz; this is not a negotiated wire rate or TX qualification.

RX packets retain the standard 64-byte header, RX_AUDIO type 1 and explicit
sample rate, format and channels. Header `length` counts scalar samples, so
stereo frame count is `length / 2`. Supported output encodings remain float32
(format 3) and saturated PCM16 (format 0).

`TciRxConverter` is continuous: unequal-rate conversion stages fixed 256-frame
input blocks and processes L/R separately with the existing r8brain converter.
Fixed partitioning makes output independent of producer callback sizes. Equal-rate
input bypasses resampling and its staging delay. There is no finite-recording
finish, padded tail or synthetic missing time. Retiring a stream discards its
pending input and filter history.

## Bounds and delay

One input frame contains at most 65,536 source frames (512 KiB for stereo
float32). The server retains at most 32 active or still-live retired source
pins, bounding pinned PCM to 16 MiB. At most eight clients each hold one
converter per admitted route. Unequal-rate converters stage at most 255 source
frames between calls, with two fixed 256-sample input arrays and bounded
filter/scratch storage for the supported rate pairs. Output is handed to the
transport in blocks of at most 1,024 stereo frames: at most 8,256 bytes including
the 64-byte header. There is no accumulating application output queue.

Before each packet, the server checks the client's pending transport bytes.
If the pending-byte count is negative or the packet would exceed 256 KiB, it
stops that client's audio, discards its converters and requests graceful close
of the shared audio/CAT/PTT connection. Existing control and PTT cleanup runs
when disconnect is delivered. This bounds RX packet admission, not all socket
writers, disconnect timing or real-radio unkey time. A short or failed send
stops audio and discards converters without explicitly closing the connection.
Both failures log their reason and byte counts once when audio stops. Other
clients keep their histories. The sent-frame counter sums only full packets
accepted for each client; different negotiated rates do not share an estimated
count.

Equal-rate conversion adds no filter or batching delay. Unequal-rate conversion
adds up to 255 source frames of batching wait (10.625 ms at 24 kHz, 5.3125 ms at
48 kHz), plus the existing filter's acoustic delay below. Impulse tests measure
the peak within one output frame of the declared filter delay. Output frame
counts include startup delay; they follow processed input duration within one
output frame of rounding. Filter delay is not extra retained output to flush.

| Producer rate | Converter output rate | Filter delay, source frames |
|---|---|---|
| 24,000 | 8,000 / 12,000 / 24,000 / 44,100 / 48,000 | 7,129 / 3,388 / 0 / 1,700 / 1,694 |
| 48,000 | 8,000 / 12,000 / 24,000 / 44,100 / 48,000 | 14,273 / 6,797 / 3,388 / 1,668 / 0 |

The 44,100 Hz entries above describe internal converter coverage only.

## Lifetime and admission

Source routes are separately keyed for native slices and DAX channels. A bounded
source pin holds the producer epoch and live slice object identity. A replacement
cannot displace a still-live producer. Removal, DAX unregister and disconnect
retire that route; its live token stays a tombstone until producer revocation,
then the slot can be reclaimed. A1 requires producers to revoke their predecessor
on replacement. A producer that violates that contract is refused rather than
mixed into a new receiver.

A DAX producer identifies a channel, independently of its slice binding. Moving
that same live channel to another live slice resets its conversion history and
uses the model's current owner, while preserving the channel's replay cursor.
This is channel attribution, not a new producer epoch or proof that radio-side
binding has converged. A transient unassigned DAX channel may retain its cached
live slice identity; removal/recreation cannot inherit that cache.

Replay cursors survive client format/subscription resets. Revoked, replayed,
backward and wrong-purpose input is rejected. A forward sample-position gap
retires the affected conversion history and resumes at the new position without
inserting silence for the missing time. A 24→48→24 format cycle has three distinct
epochs, even though the first and last rates match.

Each client/route owns its own converter. Client rate/format changes, stop,
disconnect and source retirement discard conversion and staged data together.
An unrelated receiver or another client's stream keeps its history. Admission is
checked again before handing each output packet to the transport.

The shared text-command handler uses `QPointer` guards and refetches client state
for each command in a batch. Reentrant callbacks can remove a client or grow the
client list; subsequent commands must not reuse an invalidated client reference.

## Evidence boundary

Socket-free tests inject the binary transport below production routing and
encoding. They establish format, identity, isolation and lifecycle behavior;
converter vectors establish stereo and rate response. Sanitizer reports identify
the precise instrumented source, objects and runtime. They do not establish
native-client compatibility, audible playback, real radio convergence or RF/TX.

Bytes already accepted by WebSocket/TCP are committed transport history: a source
revocation cannot recall audio already sent to a client. Application-held staging
and converter output are retired before any later admission. Packet headers keep
their original format so delayed transport cannot reinterpret them at a new rate.

A4 does not enable RTL48 or multiple RTL receivers. `DEFAULT_SAMPLE_RATE` remains
24000; `m_rxOutputRate` remains the sound-device rate. A5 decoder/sample-clock
adapters and whole-track qualification precede enabling the later RTL runtime.
