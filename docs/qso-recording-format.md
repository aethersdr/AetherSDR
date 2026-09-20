# QSO recording formats (RFC #5468 A3)

`QsoRecorder` consumes typed normalized RX frames and records each file at one
immutable 24 or 48 kHz rate. Fixed24 voice and CW feeds use separate conversion
histories. Playback parses the actual WAV format before converting to the
negotiated sink. These are software contracts; native device negotiation,
audible output and live radio qualification are separate.

## Immutable file format

`QsoRecordingFormat` chooses PCM16 little-endian stereo, once, before the initial
header is written. A current normalized speaker RX frame supplies its 24 or
48 kHz rate. Absent, revoked, or unrelated slice/auxiliary metadata selects the
existing 24 kHz default. The output layout stays stereo even for a mono source.
The format object has no rate setter and cannot be assigned a replacement rate.

| Start / subsequent input | File contract |
|---|---|
| Legacy RX or typed24 known at start | 24 kHz for the whole file |
| Current typed48 known at start | 48 kHz for the whole file |
| Start before RX metadata arrives | 24 kHz, including a later first RX48 frame |
| TX-first start without current RX metadata | 24 kHz |
| TX-first start with current RX48 metadata | 48 kHz; convert fixed24 voice/CW |
| RX changes 24 → 48 → 24 during a file | Keep the file rate; convert each source segment |
| Prior RX48 epoch revoked before the next start | Its metadata cannot select the next file |

A file already opened at 24 kHz explicitly downsamples later 48 kHz input.
It does not preserve bandwidth above 12 kHz. A new recording started with live
48 kHz metadata can preserve the wider source band. No global recording default,
settings key or UI choice is added to obtain this behavior.

The pure header encoder accepts only whole stereo frames and sizes that fit
classic RIFF, including the 36 bytes of container overhead. Duration is the
integer number of seconds represented by accepted PCM bytes at the immutable
rate. Wall time remains useful for the existing empty-capture diagnostic but
cannot stand in for recorded duration. Exclusive file creation, partial-write
accounting, finalization, cleanup and errors remain recorder responsibilities.
`recordingDurationSecs()` reports written PCM progress while recording. The
`stopRecording()` result and `recordingStopped` signal include the finite
conversion tail; the automation stop reply uses that finalized result. Stopping
an idle recorder returns zero. Live progress may lag the final duration until
the converter drains.
After a short write, the finalizer patches the exact accepted byte count even
if it ends inside a stereo frame, and keeps that failed recording unavailable
for playback. It does not round the count to make an incomplete file look valid.

## Source conversion

`QsoPcmConverter` represents one serialized input segment with fixed source and
destination formats. Each segment owns separate left and right `Resampler`
instances. RX, voice and CW must never alternate through one filter history,
even when their rates match. Mono input duplicates to L/R; a mono playback sink
receives the arithmetic mean of independently converted channels.

Float32 RX supports 24/48 kHz, mono/stereo; PCM16 file input supports
24/44.1/48 kHz. Voice and CW remain 24 kHz PCM16. PCM16 WAV samples are decoded
and encoded explicitly little-endian; native in-process and sink formats remain
native-endian. Equal-rate float recording preserves the legacy saturation and
multiply-by-32767, truncate-to-zero quantization. Equal-rate PCM16 preserves all
sample bits, including -32768. Float input is clipped to [-1,1] before filtering;
filter overshoot is clipped on output packing. Nonfinite float input is rejected.

Each input call accepts at most 65,536 complete sample frames. Invalid format,
alignment, size or finite-sample checks fail before advancing the accepted-input
clock. Unequal-rate input is staged in fixed 256-frame blocks, with at most 255
source frames pending, so the result is independent of caller block boundaries.
Short or variable input blocks share the same stream history. `finish()`
returns delayed samples for the finite stream and seals it; `discard()` abandons
delayed samples and seals it. A replacement receives a fresh instance. Neither
operation permits appending an old filter tail to a replacement's converter.
Total completed output is the nearest integer to accepted source frames times
the rate ratio, with half-frame ties rounded up. The converter removes its
bounded startup prefix and drains enough padding to expose the short-file tail,
then caps output to that sample-derived duration. The complete startup prefix
aligns to an exact rational output-frame boundary, preserving very short tails.
Padding never extends the file.

Destination playback rates are bounded to 8–192 kHz so a negotiated preferred
rate such as 96 kHz remains usable. The primary validation matrix is 24/48 kHz
input against 24/44.1/48 kHz output, with additional endpoint checks. These rates
are conversion support, not a guarantee that a native audio device accepts them.

## WAV parsing and playback preparation

`parseQsoWav()` inspects an open seekable binary `QIODevice`. It validates RIFF,
WAVE, chunk bounds, odd-byte padding, the PCM encoding, sample rate, channels,
byte rate, block alignment and complete data frames. It accepts PCM16 mono/stereo
at 24, 44.1 or 48 kHz. Unknown chunks and bounded PCM fmt extensions are skipped.
Required chunks may be separated or reordered; duplicate fmt/data chunks,
unsupported encoding and empty/truncated PCM fail explicitly. Bytes after the
declared RIFF container are ignored. Metadata after `data` is never played.

The parser scans at most 4,096 chunks using fixed-size reads and wide length
arithmetic. It does not allocate or read the audio payload merely to inspect a
file. Its chunk and PCM rules follow Microsoft's [RIFF description](https://learn.microsoft.com/en-us/windows/win32/xaudio2/resource-interchange-file-format--riff-)
and [WAVEFORMATEX fields](https://learn.microsoft.com/en-us/windows/win32/api/mmeapi/ns-mmeapi-waveformatex).
The source must remain stable during parsing and conversion.

`prepareQsoWavPlayback()` composes the parser and converter without opening an
audio device. It reads only the declared PCM in blocks of 4,096 frames, accepting
short positive reads and stopping on an error or no progress, and
produces native Int16 or Float payload for the supplied sink format. Its existing
in-memory playback model has a 256 MiB output budget, checked using actual file
duration before audio reads or allocation; callers may supply a smaller budget.
Over-budget or unsupported files return a reason rather than over-allocating.
Native Int16 and Float payloads require different memory, so the maximum playable
duration depends on the negotiated format. Larger-file streaming is outside A3.
This also caps equal-rate playback: stereo output fits approximately 46.60
minutes at 24 kHz Int16, 23.30 minutes at 48 kHz Int16, or 11.65 minutes at
48 kHz Float. A refusal emits the existing recorder error notice, leaves RX
unmuted and the finalized file available, and reconciles Play with actual
recorder state. Observers may retry, cancel, or destroy playback during signal
delivery; a retired operation cannot announce a replacement's state.

Device selection and negotiation stay with the current recorder: configured
device if still present, then default device and the existing format ladder.
The recorder preserves failed-start cleanup, completion/cancellation, replay,
and RX mute/unmute ordering. It tries the entire existing stereo format ladder
before a mono fallback. A synchronous sink failure is cleaned up before RX can
be muted; stop and destruction retire the sink before its PCM buffer. The
injected-sink lifecycle test provides no claim about
WASAPI/CoreAudio/PipeWire negotiation or audible output.

## Recorder admission and lifetime

The recorder receives typed RX from the existing normalized `rxDemodAudioReady`
subscriber, while CW/RTTY stay on their existing compatibility routes until A5.
Metadata observation and capture admission are separate: a stopped recorder can
remember a current RX format without recording or replaying that observed block.
No speaker-output tap or second producer feed is introduced.
Typed RX therefore still observes metadata under the recorder lock while
stopped; the inactive fast path for fixed24 voice/CW must not be copied onto
typed ingress. Invalid fixed24 alignment or oversized blocks are rejected
before conversion and cannot advance another source's history.

Recorder ingress serializes the per-consumer replay gate, format selection,
converter state and accepted file writes with its existing write lifecycle.
The final acquire-load of the epoch token admits each converted PCM write. A
revocation before that load rejects pending output; a later revocation cannot
undo a write that has already been admitted. File bytes
already accepted are committed history and cannot be erased by later revocation.
A rejected source must not consume another source's cursor. One current normalized
RX source is admitted; a competing live producer cannot replace it silently.
Forward gaps are admitted after retiring that segment's conversion history, with
no invented silence for unknown missing capture time and no permanent cursor stall.

Normal RX/voice/CW changes finish the departing admitted segment before beginning
the next, preserving each segment's own duration and attribution. MOX element
edges within an active CW over keep that over's history. An unrelated CW edge
while MOX still admits voice keeps the voice history until actual CW PCM takes
over. Revocation drops
uncommitted pending conversion without contaminating the replacement. The
production rates test covers stop/start, reconnect and stream replacement,
format epochs, forward gaps, replay, concurrent feeds and concurrent feed/stop
with epoch revocation. Sanitizer results qualify the specific executed build;
the existence of a concurrency test alone is not a TSan result.

Existing MOX/CW over gates, local TX ownership, CW over-hang, auto/manual start
policy, metadata slice lifetime, filename collision safety, and short-write/error
cleanup remain authoritative. Playback cannot key TX. `DEFAULT_SAMPLE_RATE` stays
24000 and `m_rxOutputRate` stays the device rate. RTL48/multi-receiver runtime,
A4 TCI, A5 decoder/clock, hardware integration, and #5554 whole-track sign-off are
separate from A3 software evidence.
