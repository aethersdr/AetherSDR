# Local Digital Voice With ThumbDV

AetherSDR runs local digital voice as one `AetherDV` helper process. The helper
talks to the radio over the Flex waveform network API and uses a ThumbDV or
DV3000U over the operating system serial device for AMBE voice encode/decode.
The ThumbDV stays attached to the computer running AetherSDR; it does not need
to be plugged into the radio.

Software AMBE encode/decode is intentionally not bundled at this time because
of patent review concerns.

## App-To-Helper Contract

The service currently exposes one complete mode engine: D-STAR. Adding another
mode requires a mode descriptor, a complete modem engine, and tests; incomplete
modes are not registered with the radio or shown in the UI. One process owns the
ThumbDV serial device, and the mode registry permits only one active digital-
voice mode and slice at a time.

The default helper executable name is `aether-dv-waveform` (or
`aether-dv-waveform.exe` on Windows). AetherSDR builds this helper from the
bundled `third_party/smartsdr-dsp` SmartSDR/ThumbDV transport and the independent
`third_party/crdv` protocol core, then places it beside the main executable. The
removed OpenDV-derived framing/state implementation and its GPL-2.0-only
definitions are not built or distributed. Clean-room provenance, public-source
citations, the integration contract, tests, and a source manifest ship with
`crdv`. The local D-STAR controls and client-side DSTR mode affordances are
enabled only in builds that include the helper.

### Device discovery and access

`DStarModel` is the single serial-device authority for both AetherModem and the
Waveforms window. It ranks USB metadata and stable device names, but it does not
save a candidate automatically until the helper receives valid DV3000 reset and
product-ID responses. On Linux, `/dev/serial/by-id/...` is preferred over a
renumberable `/dev/ttyUSB*` alias for the same device. Generic serial devices
remain manual candidates and are never auto-selected.

Official Linux packages install
`packaging/linux/70-aethersdr-thumbdv.rules`, which tags FTDI `0403:6015` tty
devices for `uaccess`; the active desktop user then receives access without
running AetherSDR as root. After installing the rule, reconnect the ThumbDV (or
reload udev rules). On systems without systemd-logind/uaccess, add the user to
the distribution's serial-device group, commonly `dialout` or `uucp`, then log
out and back in. A helper open failure preserves the operating-system reason so
the UI can distinguish a missing device from denied access or another process
holding the port.

Windows discovery uses Qt serial metadata and accepts both `COMn` and extended
`\\.\COMn` names. The helper opens the selected port exclusively so another
program cannot interleave DV3000 commands; a sharing violation is reported as
an in-use device rather than as a generic missing-device error.

On macOS, discovery presents the callout `/dev/cu.*` endpoint rather than its
paired `/dev/tty.*` endpoint. The helper verifies both custom baud-rate changes
through `IOSSIOSPEED`; the native serial API's lack of an FTDI latency-timer
control is reported once and treated as nonfatal.

AetherSDR starts the helper with the radio address, configured ThumbDV serial
device, and the validated D-STAR station/routing fields:

```text
--host <radio-ip> --vocoder thumbdv --serial <thumbdv-device> \
  --mode DSTR --underlying-mode DFM --waveform-name AetherDStar \
  --mycall <MYCALL> --mycall-suffix <suffix> --urcall <URCALL> \
  --rpt1 <RPT1> --rpt2 <RPT2> --message <20-character-message>
```

AetherSDR runs the helper from its executable directory and sets the equivalent
environment variables:

```text
SSDR_RADIO_ADDRESS=<radio-ip>
AETHER_DV_VOCODER=thumbdv
AETHER_DV_THUMBDV_SERIAL=<thumbdv-device>
AETHER_DV_FAIL_FAST=1
AETHER_DV_MODE=DSTR
AETHER_DV_UNDERLYING_MODE=DFM
AETHER_DV_WAVEFORM_NAME=AetherDStar
AETHER_DSTAR_MYCALL=<MYCALL>
AETHER_DSTAR_MYCALL_SUFFIX=<suffix>
AETHER_DSTAR_URCALL=<URCALL>
AETHER_DSTAR_RPT1=<RPT1>
AETHER_DSTAR_RPT2=<RPT2>
AETHER_DSTAR_MESSAGE=<20-character-message>
```

Two diagnostic overrides are intentionally left as environment variables so
on-air TX tests can isolate polarity and level without adding user-facing
controls:

```text
AETHER_DSTAR_TX_INVERT=0   # disable the historical Flex TX polarity mapping
AETHER_DSTAR_TX_GAIN=0.8   # override the default 1.0 generated-waveform gain
AETHER_DSTAR_TX_PREAMBLE_MS=500   # override the default 250 ms bit sync
AETHER_DSTAR_TX_NULL_AMBE=1   # transmit only known D-STAR silence frames
AETHER_DSTAR_VERBOSE_RX_IDLE_DIAG=1   # include high-rate idle RX buffer logs
AETHER_DSTAR_DIAG_SYNC_TIMING=1   # report samples between D-STAR data syncs
AETHER_DSTAR_RX_SYNC_MAX_ERRORS=0   # strict sync; compatibility default is 2
AETHER_DSTAR_RX_SYNC_MISS_LIMIT=0   # never unlock on misses; default is 3
AETHER_DSTAR_RX_SYNC_REALIGN_BITS=0   # disable sliding recovery; default is 24
AETHER_DV_DIAG_SETUP=1   # report helper TCP commands and radio responses
AETHER_DSTAR_CAPTURE_RX_PATH=/tmp/dstar-rx.f32   # raw 24 kHz pre-demod samples
AETHER_DSTAR_CAPTURE_RX_META_PATH=/tmp/dstar-rx.csv   # packet metadata sidecar
```

The helper registers the selected descriptor through the Flex waveform API. It
first removes its own `AetherDStar` registration, then creates and configures it:

```text
waveform remove AetherDStar
waveform create name=AetherDStar mode=DSTR underlying_mode=DFM version=1.2.0
waveform set AetherDStar tx=1
waveform set AetherDStar rx_filter low_cut=-3500
waveform set AetherDStar rx_filter high_cut=3500
waveform set AetherDStar rx_filter depth=256
waveform set AetherDStar tx_filter low_cut=0
waveform set AetherDStar tx_filter high_cut=4800
waveform set AetherDStar tx_filter depth=256
waveform set AetherDStar udpport=<bound-port>
```

Current firmware accepts filter depths of 128, 256, 512, 1024, or 2048 and
rejects the historical value `2` with `0x50000016`. The bundled configuration
uses depth 256 to match the maintained FreeDV Flex integration. Filter depth is
independent of the helper's 128-sample waveform VITA packet contract.

The pre-create removal is intentionally scoped to the descriptor's current
name. The helper contains no `ThumbDV` registration fallback or old-name cleanup
path. `AetherDV.cfg` is a bundled informational manifest; registration behavior
is owned by the validated mode registry rather than an executable command script.

The TCP receive loop starts before registration. Every command has a bounded
response wait, every non-removal command must return code zero, and the app does
not report the service as Running until the helper emits a verified ready record.
The remove response must be received and parsed, but an absent current
registration is harmless because the following create transaction remains the
authority.

The `waveform create` response supplies named input and output stream IDs.
Current FlexRadio `waveform-sdk` source records all four IDs but explicitly notes
that packets cannot be sent successfully on the advertised output IDs; its VITA
sender replies on the corresponding input stream ID. The maintained FreeDV Flex
integration and this helper follow that same behavior. The helper rejects a
create response unless all four required IDs are present, nonzero, and distinct,
then dispatches packets by the registered stream map rather than inferring RX/TX
from an ID's numeric pattern. With the reference radio,
the assigned D-STAR streams were `0x81000001` (TX input), `0x81000000` (RX input),
`0x01000001` (TX output), and `0x01000000` (RX output).

## Station And Routing Configuration

The Waveforms window stores shared service configuration in the
`DigitalVoiceWaveform` AppSettings JSON object. Backend, executable, ThumbDV
serial device, and auto-start are service-level fields. `MYCALL`, the four-
character MYCALL suffix, `URCALL`, `RPT1`, `RPT2`, and the optional 20-character
message are nested under `DStar`.
The old flat `DStarWaveform` object is migrated once, after which the old key is
removed. An old bundled `aether-dstar-waveform` path is intentionally discarded
so migration cannot become a runtime binary fallback; a genuinely custom path
is preserved.
The default routing is `CQCQCQ` / `DIRECT` / `DIRECT`.

If no MYCALL override has been saved, startup uses the connected radio's
callsign. The helper will not start unless the effective MYCALL is 3-8 uppercase
letters/digits and includes both a letter and a digit. Suffix, routing, and
message fields are length- and character-validated before launch. This prevents
the historical `CALLSIGN` placeholder or malformed routing bytes from being
transmitted. `URCALL` accepts the D-STAR leading-slash gateway form while
`MYCALL`, `RPT1`, and `RPT2` remain callsign/module fields. The helper still pads
fixed-width D-STAR header fields with spaces on the wire.
The helper repeats these checks at its process boundary, so direct CLI or
environment-variable launches cannot bypass the application's validation.

## Data Path

With `underlying_mode=DFM`, the radio performs the FM discriminator stage and
the helper receives demodulated L/R sample packets. The helper then performs
D-STAR symbol timing with the retained Flex GPLv3 modem, performs framing/FEC/
slow-data handling through `crdv`, passes AMBE voice frames to the ThumbDV, and
sends decoded PCM back to the radio as speaker data.

Using `RAW` would move FM/GMSK demodulation into the helper and require I/Q
data. That is not required for the DFM D-STAR path above.

On transmit, the radio supplies microphone sample packets when the operator
intentionally keys the DSTR slice. The helper sends speech to the ThumbDV for
voice encoding, builds D-STAR/GMSK transmitter samples, and sends transmitter
data packets back to the radio. AetherSDR does not key the transmitter to start
the helper. The helper inhibits generated D-STAR waveform output when the radio
interlock source is `TUNE`; tune carrier requests are operator intent to tune,
not intent to transmit a D-STAR data stream.

The 20-character slow-data message begins immediately after a data-sync frame
and uses the current JARL section 6.3 block numbers `0x40` through `0x43`.
Message and 41-byte header-repeat superframes alternate; unused fragments carry
scrambled uninterpreted data. Receive accepts the current JARL numbering only.
`0x44` is not a short-message block and is rejected. When the radio accepts an
eligible D-STAR transmit transition, the helper emits one TX waveform-status
record from the immutable header/message snapshot used for that transmission;
AetherModem uses that record for its outbound traffic entry.

The helper treats the radio interlock as the TX/RX boundary. While TX or the
D-STAR end-of-transmission tail is active, RX buffers from the waveform API are
zero-filled and returned without touching the D-STAR TX modulator. Once the tail
has flushed, the helper resets the RX GMSK/D-STAR state and reasserts the
radio-side DSTR discriminator settings on the active D-STAR slice. This avoids
RX-buffer cadence during half-duplex TX from restarting the TX frame stream or
canceling D-STAR end bits.

Receive synchronization uses caller-selected `crdv` policy rather than hidden
parser constants. AetherSDR defaults to the clean core's all-zero, exact-only
policy. The three environment overrides above permit controlled tolerance and
bounded realignment experiments, but nonzero production defaults remain gated
on the C11 RF/noise/false-lock evidence documented in
`third_party/crdv/INTEGRATION_CONTRACT.md`.

TX/RX transition state is centralized in `dstar_transmit_state.c`. Repeated
`READY`/receive statuses while already receiving do not reset the demodulator;
a reset is consumed only after a real TX-to-RX transition, explicit request, or
completed end-of-transmission tail. Active, draining, and end-pattern phases are
distinct. The tail is sent in 128-sample packets paced at the 24 kHz stream rate
instead of being dumped into UDP in a tight loop.

ThumbDV encoded and decoded queues are purged independently of their playback
buffering gates. Every serial vocoder request is tagged with a generation and a
monotonic TX audio-frame sequence. A response from before a TX/RX queue flush is
discarded rather than entering the new direction's audio queue, and current
responses retain their sequence through asynchronous serial completion.
Generation validation and response insertion share the flush lock order, so an
old response cannot pass validation and then enter a queue after that queue has
been purged. The scheduler reads pending plus available encode work atomically,
preventing a final response from moving between those states while EOT overtakes
it. Device reconnect waits for the old read thread, purges queued frames, and
resets the old device's pending request generations.

VITA packet counters are maintained per output stream ID and wrap modulo 16, as
required by the four-bit VITA packet-count field. RX and TX stream traffic no
longer consume one shared sequence. The UDP listener rejects packets shorter
than the VITA header and waveform payloads that do not contain the required 128
complex samples, preventing partial buffers from reaching the modem.
Output packets use UTC integer timestamps with real-time fractional
picoseconds, matching the current Flex waveform transport and the radio's input
packet timestamp mode; the adopted helper's zero-valued sample-count timestamp
was retained from an obsolete transport implementation.
The listener also tracks the radio's packet count independently for each input
stream and reports gaps or duplicates as `AETHER_DSTAR_DIAG` records. The
optional RX metadata sidecar records one CSV row per raw-capture block after
the corresponding output packet has already been returned to the radio. It
contains the raw-file sample offset, VITA packet counter/header and timestamp,
and local monotonic arrival time, allowing RF-source discontinuities to be
distinguished from UDP or scheduler loss without extending waveform turnaround.
The
`AETHER_DSTAR_TX_NULL_AMBE` diagnostic bypasses ThumbDV encoding and fills every
voice frame with the standard D-STAR silence vector, isolating RF framing from
the microphone and vocoder path during an operator-requested test.

### Live RX Continuity Findings

Live FLEX-6700/4.2.18 receive tests localize the remaining intermittent garble
ahead of ThumbDV decoding and audio playback. During affected transmissions,
the helper's receive turnaround remained in the tens of microseconds with no
signal-period deadline misses, ThumbDV requests and responses remained
balanced, and VITA packet counters remained continuous. Nevertheless,
successive mandatory D-STAR data-sync markers were commonly separated by
`10080 - N*128` input samples, where `N` was typically one through five. The
same shortened spacing is visible in fixed-phase replay of raw pre-demod
captures, not only in the live clock-recovery path.

Changing output packets to the current Flex UTC/real-time timestamp format,
changing waveform filter depth from 128 to the current-client value 256, adding
a 40 ms decoded-audio playout buffer, and forcing WNB/NB/NR/ANF off did not
change the objective spacing errors or subjective garble. The radio accepted
all non-empty setup commands. Its assigned input stream IDs match the received
VITA streams, and replying on those input IDs agrees with both the official
FlexRadio `waveform-sdk` and the maintained FreeDV integration. The config
reader previously sent a blank line as one malformed command; that startup-only
error is fixed and was unrelated to recurring receive corruption.

Independent fixed-phase scanning rules out false early-sync matches. Each early
24-bit marker appears at the same raw-sample location on four or five symbol
phases, no competing marker appears at the nominal boundary, and successive
marker deficits remain quantized in 128-sample blocks. Across a representative
capture interval, 385,568 payload samples span 16.3762 seconds of both radio
timestamps and local arrival time: an effective payload rate of about 23,544
samples/second instead of 24,000.

The adopted helper received waveform VITA on its announced UDP port 5000 but
created a second, effectively unbound socket for replies. FlexRadio's official
`waveform-sdk` and the maintained FreeDV integration both receive and send on
the same announced socket. The helper now sends waveform responses through its
bound listener socket as well. A focused transport test verifies that response
packets use that shared sender, radio destination port 4991, the incoming stream
ID, the expected packet size, and per-stream sequence. Live testing with the
same helper build before and after this correction still showed the shortened
source stream while another SmartSDR GUI client was connected, so the socket
error was real but was not the cause of the recurring receive corruption.

A subsequent controlled run disconnected the other SmartSDR GUI client while
leaving the radio, laptop, helper build, network, and ThumbDV unchanged. With
AetherSDR as the only GUI client, 67 of 69 consecutive 188-packet timing windows
were centered near the expected 5,333.333 microseconds per 128-sample packet;
the two remaining windows were isolated scheduling bursts rather than a
sustained rate deficit. VITA packet counters remained continuous, receive
turnaround was normally 14-21 microseconds, and the processing queue stayed at
zero or one packet. During an IC-91 D-STAR transmission the helper sustained 50
vocoder requests and 50 decoded audio blocks per second, and the received audio
was subjectively reported as substantially improved.

This A/B result strongly implicates radio-side waveform scheduling or resource
contention associated with the additional Multi-Flex client, its slices, or its
panadapters. FlexLib confirms that the radio status fields `slices=` and
`panadapters=` are remaining-resource counts. The affected run reported `5/6`
and exposed three foreign slices on two foreign panadapters; after that client
disconnected, the radio reported its full `8/8` capacity before AetherSDR
created its own objects. It does not yet distinguish active Multi-Flex load from
stale radio state left by that client. The next controlled test is: power-cycle
the radio, run AetherSDR alone and capture one IC-91 transmission, then reconnect
the other GUI client and repeat without changing anything else. A laptop reboot
is not useful unless the clean-radio, single-client baseline fails.
Packet-boundary erasure recovery should remain a defensive fallback, not the
primary fix, until that comparison identifies whether the radio resumes
dropping source blocks.

### Runtime Delivery Diagnostics

The helper continuously aggregates observations already made in its RX and TX
paths; it does not add a capture, worker, socket, or high-rate timer. After 188
valid VITA timestamp intervals (about one second at 24 kHz), it emits one
directional, versioned `AETHER_DV_METRIC` line. RX reports contain effective
sample rate, true VITA sequence gaps, inferred missing 128-sample source blocks,
mean/maximum turnaround, and maximum scheduler queue depth. TX reports contain
the mic-stream rate and VITA gaps; the final partial report at unkey adds null
AMBE frames, PCM clips/invalid samples, VITA send failures, bounded voice/tail
queue depth, tail samples/time, pre-roll frames/delay, AMBE queue underflow,
overflow and sequence faults, pending/submission vocoder observations, and drain
deadline/discard counters. Individual packets never produce telemetry lines.
VITA gaps and inferred source deficits remain separate because continuous packet
counters do not rule out the shortened source stream observed above.

`DigitalVoiceWaveformProcess` validates and parses those records across arbitrary
stdout/stderr read boundaries. Cadence warns only after five consecutive
windows below 99 percent of 24 kHz (23,760 samples/second), inferred source
deficits require two consecutive windows, and transport loss warns immediately.
A degraded state clears after ten healthy windows. Network Diagnostics retains
the data through its existing one-second sampler and history compaction, showing
an RX/TX-rate graph against 24.00 ksps and a separate errors graph for RX/TX
VITA gaps and inferred RX source blocks. TX encoder and tail counters are shown
as summary values. No-data periods are omitted rather than graphed as zero. The
Waveforms card shows the persistent degraded reason, while a single
nonmodal status message is emitted only when delivery first enters a degraded
state.

## D-STAR Framing Conformance

The framing reference is the JARL D-STAR Technical Requirements, version 5.0a:
<https://www.jarl.com/d-star/STD5_0a.pdf>. The helper emits the 64-bit bit-sync
preamble followed by the 15-bit frame sync, a 660-bit protected radio header
(328 header/CRC bits plus two convolutional-code tail bits), and 72-bit AMBE
voice plus 24-bit slow-data frames. Data sync is inserted on the first voice
frame and every 21 frames thereafter.

The adopted source previously iterated the preamble as if each bit still needed
five loop iterations even though `gmsk_encode()` already emits five samples per
symbol. The helper now sends 250 ms of alternating bit sync by default before
the 15-bit frame sync, allowing the radio TX ramp and receiver timing recovery
to settle before the one-time header. The diagnostic preamble override is
bounded from the 64-bit protocol minimum through one second. Header FEC loops are
bounded to 330 input and 660 output bits, endian reversal stops at the 328 data
bits, short interleave/deinterleave inputs fail closed, and the overrun-prone
partial-byte accesses are removed. The 41-byte radio
header is also carried completely in slow data, and malformed message segment
indices above three and malformed 1-5 byte header chunk lengths are rejected.
PFCS calculation is byte-order independent,
and the final state emits exactly the specified 48-bit last frame without the
adopted source's undocumented 22-symbol suffix. RX also rejects undersized AMBE
output buffers instead of copying a nine-byte vocoder frame past the caller's
capacity.

The adopted modulator used a 41-tap Gaussian filter generated for ten samples
per symbol even though the Flex waveform stream carries five samples per D-STAR
symbol. It also applied a 4.8 kHz peaking IIR after pulse shaping. The local
helper now uses the openly licensed MMDVM modem's 15-tap BT 0.35 Gaussian
interpolator at its native 24 kHz / 4.8 ksym/s operating point and does not apply
the non-standard peaking stage. The FIR history is zero-initialized and cleared
on reset.

The historical Flex helper's TX inversion remains enabled by default: in the
helper this maps logical `1` to positive waveform samples, matching the JARL
positive-deviation requirement at the Flex DFM boundary. Set
`AETHER_DSTAR_TX_INVERT=0` only for deliberate polarity diagnostics. The
normalized pulse shaper defaults to a 1.0 output gain and is hard-limited to
+/-0.98 before VITA serialization, while `AETHER_DSTAR_TX_GAIN` remains
available for deliberate level adjustment. Non-finite samples and gain
overrides are converted to silence rather than entering the VITA stream. The
24-to-8 kHz encoder input is independently finite-checked and saturated to
signed 16-bit PCM before it is passed to ThumbDV.

TX control is phase-driven. The interlock reducer requires the active D-STAR
slice to be the radio's TX slice, remembers `source=TUNE` across source-less
UNKEY statuses, and makes duplicate/terminal states idempotent. Slice status is
reduced atomically so a removal such as `in_use=0 tx=1` cannot leave stale TX
ownership because of field order. During the roughly 390 ms preamble/header,
sequence-tagged ThumbDV responses enter a fixed 32-frame FIFO instead of
collapsing to the latest frame, so speech at the PTT edge is emitted first when
voice framing begins. Missing response sequences become legal null AMBE frames;
duplicates and stale sequences are rejected, and overflow drops the oldest frame
to bound latency.

Firmware 4.2.18 on the reference FLEX-6700 changes `UNKEY_REQUESTED` to `READY`
about 345 ms after `xmit 0`, regardless of a longer helper queue. The helper
therefore drains oldest speech for 280 ms, preserving the PTT-edge audio, then
discards only any newest excess tail and reserves the remaining radio window for
the specified 10 ms / 48-bit end pattern. A measured three-second live run
drained in 297.7 ms, discarded two newest 20 ms frames, and completed EOT 81 ms
before `READY`; the independent replay demodulator recovered 152 AMBE frames,
eight data syncs, zero sync misses, and EOT on all five fixed symbol phases. If
the radio stops delivering mic buffers, paced synthetic descriptors finish this
drain without blocking RX/status processing or depending on a later radio
packet. Hard mode/slice/disconnect cancellation invalidates vocoder generations
and clears both PCM and AMBE queues immediately.

Very short keys are a separate firmware boundary. On the same radio, releasing
MOX before the default 250 ms preamble completed produced `READY` only 15 to
109 ms later in measured short-key trials. That is not enough time to send the
rest of the preamble, the 660-bit protected header (137.5 ms), and EOT. The
helper therefore detects this case and aborts the incomplete header, queued
speech, and vocoder generation instead of continuing a truncated, undecodable
transmission. Once the preamble has completed, the normal bounded header,
speech, and EOT drain described above is used.

## Offline Verification

The focused C tests are built with AddressSanitizer and UndefinedBehaviorSanitizer
on supported Clang/GCC hosts:

- `dstar_modem_test`: FEC guard canaries, reference vector and golden digest,
  independent full protected-header/PFCS pipeline, packed on-air golden vector,
  FEC round-trip, deterministic Gaussian FIR reset, modem loopback, exact sync
  and last-frame lengths, AMBE output and slow-data bounds
- `dstar_transmit_state_test`: idle receive, real TX-to-RX, atomic phases,
  finite/saturated PCM conversion, gain limiting, and the 128-sample/24-kHz
  packet interval
- `digital_voice_tx_gate_test`: active/TX-slice eligibility, TUNE inhibition
  across source-less UNKEY, field-order-independent slice removal, duplicate
  transitions, and all terminal states
- `dstar_tx_path_test`: interlock through delayed fake-ThumbDV responses,
  bounded header/voice/tail framing, source-independent tail completion, and
  shared-socket VITA stream IDs/packet counters
- `thumbdv_queue_test`: unconditional queue purge, stale response generations,
  and concurrent response/flush lock ordering
- `vita_packet_count_test`: independent per-stream sequence and modulo-16 wrap
- `digital_voice_mode_registry_test`: descriptor validation, response-verified
  setup sequencing, returned-stream mapping, and fail-fast command rejection
- `digital_voice_waveform_process_test`: settings migration, exclusive mode/slice
  ownership, directional RX/TX telemetry and history, routing validation, and
  process failure behavior

No automated test keys the transmitter. Bridge verification launches without
`AETHER_AUTOMATION_ALLOW_TX`; service lifecycle and radio registration are
therefore testable while all keying controls remain blocked.

`get waveforms localDigitalVoice` reports the helper state, implemented mode
descriptors, registration verification, exclusive active slice, executable and
device selection, nested D-STAR routing, and configuration validity. The full
`get waveforms` payload also reports each radio-authoritative raw `mode_list`,
the maximum DSTR occurrences in any one slice list, and the last asynchronous
maintenance command. `waveform start dstar`, `waveform stop`, `waveform resync`,
and generic `waveform unregister <name>` are non-keying bridge verbs. Cleanup is
verified by polling the command response and then the raw mode lists; command
acceptance alone is not treated as proof.

## Dependency Boundary

The helper is a separate executable, not part of the main Qt GUI binary. Flex's
current `waveform-sdk` is LGPL-3.0 and normally comes from their waveform
development container. The bundled helper instead uses the public GPL-3.0
SmartSDR-DSP ThumbDV waveform sources listed in `THIRD_PARTY_LICENSES`.

The historical SmartSDR-DSP branch carried legacy FTDI D2XX headers and a
static library; those are intentionally not bundled. `aether_serial_compat.c`
provides the small D2XX call surface that the ThumbDV code uses on top of
normal OS serial ports. Unix-like builds use POSIX `termios`; Windows builds
use native Win32 COM-port APIs.

## Historical Radio USB Diagnostic

The radio-installed D-STAR rewrite originally included a clean-room legacy
waveform diagnostic named `AetherThumbDVProbe`. That probe was removed from the
product path after live testing showed that this radio/firmware accepts legacy
waveform package metadata but does not launch or attach the legacy executable.
AetherSDR still keeps the evidence below so future work does not rediscover the
same failure mode.

If a future Flex firmware or older known-working radio makes legacy executable
testing useful again, recover the removed diagnostic source and package builder
from checkpoint commit `f0978bbb55b684593bb6a8a59cafcf3a0c593ee5`. Do not
reintroduce it as a bundled user-facing package unless the target
radio/firmware is proven to run `slice waveform_cmd <slice> status` against a
live waveform process.

### Live Legacy Waveform Findings

These notes come from live testing against a FLEX-6700 named `GeekJeep`,
serial `4213-3107-6700-8545`, running firmware `4.2.18.41174`. Keep them
with the implementation because the legacy waveform path has several
non-obvious behaviors that are not clear from FlexLib alone.

- Uploading a `.ssdr_waveform` package through AetherSDR uses the radio's
  `file upload <size> new_waveform` path. A successful upload does not
  immediately prove the waveform executable can run.
- The radio did not report the legacy package in `waveform installed_list`
  until slice 0 was switched to `DSTR`. The radio then reported slice 0 mode
  as `DIGU`; this appears to be the radio's underlying-mode presentation for
  the D-STAR legacy waveform path.
- Legacy `installed_list` entries can contain a double DEL separator:
  `Name<DEL><DEL>Version`. The parser must tolerate that shape.
- `installed_list` appears to use the package header `Version`, not the
  version passed to `waveform create`. The historical ThumbDV package has
  `Version: "1.2.0"` in the header but `waveform create ... version=1.1.0`,
  and the radio reports `ThumbDV<DEL><DEL>v1.2.0`.
- The historical ThumbDV package is a two-file ZIP with the executable first,
  FAT/MS-DOS ZIP attributes, and CRLF text config. Its executable is a dynamic
  ARM EABI5 binary using `/lib/ld-linux.so.3`, Linux `2.6.16`, and GLIBC 2.4
  symbols. A modern static ARM/Linux 3.2 glibc build is not a good compatibility
  match for this radio path.
- The historical package registers `mode=DSTR underlying_mode=DFM`,
  `tx=1`, RX filters, TX filters, and `udpport=5000`. Matching `DSTR`,
  TX filters, `tx=1`, executable-first ZIP order, FAT ZIP attributes, and the
  old CodeSourcery-style ABI made the diagnostic package install/list cleanly
  but still did not prove its executable was running.
- FlexLib sends active waveform commands as
  `slice waveform_cmd <slice> <command>`. The old ThumbDV UI sends
  `slice waveform_cmd 0 status` after the slice enters `DSTR`; the waveform
  responds with `waveform status slice=<n> ...`, which FlexLib surfaces as
  slice `waveform_status`.
- The radio forwards those waveform commands to the waveform's SmartSDR API
  TCP connection as `C<seq>|slice <n> <command>`. A running waveform must process
  that inbound command and answer with `waveform response <seq>|<ret>`. A
  one-shot helper that only sends status and exits is not sufficient for the
  legacy on-radio command path.
- During diagnostic testing, `slice waveform_cmd 0 status` returned
  `0x50001000` for `AetherThumbDVProbe` while the slice was active in the
  waveform mode. That means the radio listed the waveform from package/config
  metadata, but did not have a running command target for our diagnostic
  executable at that point.
- The archived historical `ThumbDV.ssdr_waveform` package behaved the same way
  on this radio/firmware: it uploaded, appeared as `ThumbDV<DEL><DEL>v1.2.0`
  after switching slice 0 to `DSTR`, and still returned `0x50001000` for
  `slice waveform_cmd 0 status`. Treat this as evidence that this
  FLEX-6700/4.2.18 legacy path lists package metadata but does not launch or
  attach the legacy executable, independent of the clean-room diagnostic
  package.
- The removed clean-room diagnostic tried direct SmartSDR API connection,
  loopback when the discovered/restricted radio IP was local, and a VITA
  discovery fallback on UDP 4992. No radio USB enumeration result was captured
  because the blocker was legacy executable launch, not ThumbDV serial probing.
- The regular Flex USB cable API is not a substitute for this diagnostic on the
  tested radio. `sub usb_cable all` returned success but no `usb_cable`
  statuses for the attached ThumbDV, `usb_cable list` and `usb_cable info`
  returned `0x50001000`, and `usb list` returned `0x50000015`.

Receive-only activation recipe used during testing:

```text
sub slice all
sub waveform all
slice set 0 mode=DSTR
slice waveform_cmd 0 status
slice set 0 mode=LSB
```

Do not leave slice 0 in `DSTR` after diagnostics. Confirm
`transmitting=false` and restore the user's original mode after each test.

## Trademark Note

D-STAR is a registered trademark of Icom Inc. AetherSDR uses the term only to
identify compatibility with the D-STAR amateur-radio protocol and is not
affiliated with or endorsed by Icom Inc.
