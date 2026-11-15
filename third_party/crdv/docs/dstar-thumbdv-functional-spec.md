# AetherSDR D-STAR / ThumbDV Functional Specification

**Document status:** clean-room implementation specification

**Specification date:** 2026-07-11

**Target radio profile:** SmartSDR protocol 1.4.0.0 on FLEX-6000-series firmware 4.2.18

**Target digital-voice mode:** D-STAR voice through a host-attached DVSI USB-3000/ThumbDV-class vocoder

## 1. Clean-room boundary

This document describes required behavior, wire formats, validation rules, timing,
failure behavior, and black-box acceptance tests. It does not prescribe program
structure, source-language constructs, algorithms beyond published protocol
definitions, internal names, or control flow.

The future implementation team **must not access** any of the following:

- `third_party/smartsdr-dsp`, including copies, forks, patches, or generated
  artifacts derived from that tree;
- the source-analysis conversation that produced this specification; or
- git history containing that code, including the checkpoint branch, commits,
  diffs, blobs, tags, stashes, reflogs, or remote references from which it can be
  recovered.

The implementation team may use only this specification, the linked public
authorities, independently obtained hardware captures, and black-box test
fixtures whose provenance is recorded. Any behavior marked **CORROBORATION
REQUIRED** is non-normative until the isolated team verifies it without viewing
the prohibited material.

The words **must**, **must not**, **should**, and **may** have their ordinary
requirements meaning. Values described as “compatibility profile” are
AetherSDR product requirements rather than claims about a public protocol.

## 2. Public authorities

The implementation team should pin local copies of these materials, record
their hashes, and retain the cited revisions with its test evidence.

1. JARL, [D-STAR Standard, Version 7.0 (May 2025)](https://www.jarl.com/d-star/STD7_0.pdf),
   especially §§3.1, 4.1, 6.1–6.4 and Appendices 1–2, plus JARL's
   [English technical-requirements edition](https://www.jarl.com/d-star/shogen.pdf)
   for the explicitly printed CRC and convolutional generator equations.
2. DVSI, [USB-3000 Digital Voice USB Stick User's Manual](https://www.dvsinc.com/manuals/USB-3000_Manual.pdf),
   especially §§4.2, 5.1–5.5, 6.1–6.4, and the D-STAR rate table.
3. DVSI, [AMBE-3000F Vocoder Chip User's Manual](https://www.dvsinc.com/manuals/AMBE-3000F_manual.pdf),
   especially §§5.2–5.4 and 6.1–6.7.
4. FlexRadio, [Waveform API overview](https://www.flexradio.com/api) and the
   [Waveform SDK at pinned public commit `7c717b0`](https://github.com/flexradio/waveform-sdk/tree/7c717b034c7c60c883e6c48fb33934c5b7fb776d).
   The SDK's [integration guide](https://github.com/flexradio/waveform-sdk/blob/7c717b034c7c60c883e6c48fb33934c5b7fb776d/doc/README.md),
   [public API declarations](https://github.com/flexradio/waveform-sdk/blob/7c717b034c7c60c883e6c48fb33934c5b7fb776d/include/waveform_api.h),
   [radio transport](https://github.com/flexradio/waveform-sdk/blob/7c717b034c7c60c883e6c48fb33934c5b7fb776d/src/radio.c),
   and [VITA transport](https://github.com/flexradio/waveform-sdk/blob/7c717b034c7c60c883e6c48fb33934c5b7fb776d/src/vita.c)
   are the public reference for the Waveform API behavior below.

If a later public authority conflicts with this document, the implementation
team must record the conflict and obtain maintainer direction; it must not
silently change the AetherSDR compatibility profile.

## 3. Externally observable helper contract

### 3.1 Process inputs

The delivered helper executable is named `aether-dv-waveform` (with the
platform's conventional executable suffix when required). It accepts either
`--name value` or `--name=value` for value-bearing options.

AetherSDR persists the parent-side service configuration in the
`DigitalVoiceWaveform` `AppSettings` JSON object with this public schema:

| JSON member | Type/meaning |
|---|---|
| `Backend` | string; `ThumbDV` |
| `AutoStart` | boolean |
| `SerialPort` | string containing the selected host serial path/name |
| `ExecutablePath` | optional string override; empty/absent uses the bundled executable |
| `DStar.MyCall` | station call string |
| `DStar.MyCallSuffix` | zero-to-four-character suffix |
| `DStar.UrCall` | destination call string |
| `DStar.Rpt1` | departure repeater string |
| `DStar.Rpt2` | destination repeater string |
| `DStar.Message` | zero-to-twenty-character message |

An existing flat `DStarWaveform` object is migrated once into this schema and
then removed. A historical bundled `aether-dstar-waveform` executable path is
discarded during migration; a non-bundled custom path is preserved. If no
MYCALL override exists, the connected radio callsign is the candidate value,
but it still must pass §3.2 validation. The serial device remains attached to
the computer running AetherSDR; the radio is not expected to expose or share
that USB device.

| Input | Meaning | Required/default |
|---|---|---|
| `--host`, `--ip` | SmartSDR radio IPv4 address | required for normal operation |
| `--serial` | host serial-device path or port name | required |
| `--vocoder`, `--backend` | vocoder selection | must select `thumbdv` |
| `--mycall` | originating station call | required |
| `--mycall-suffix` | originating station terminal suffix | empty |
| `--urcall` | destination/called station | `CQCQCQ` |
| `--rpt1` | departure repeater | `DIRECT` |
| `--rpt2` | destination repeater | `DIRECT` |
| `--message` | short message | twenty spaces |
| `--mode` | displayed waveform mode | `DSTR` |
| `--underlying-mode` | radio demodulator mode | `DFM` |
| `--waveform-name` | SmartSDR waveform name | `AetherDStar` |
| `--probe-serial` | verify the vocoder and exit without radio registration | off |
| `--console` | keep diagnostic output attached to the invoking console | off |

The equivalent environment inputs are:

| Environment variable | Equivalent/configuration |
|---|---|
| `SSDR_RADIO_ADDRESS` | `--host` |
| `AETHER_DV_THUMBDV_SERIAL` | `--serial` |
| `AETHER_DV_VOCODER` | `--vocoder` |
| `AETHER_DV_MODE` | `--mode` |
| `AETHER_DV_UNDERLYING_MODE` | `--underlying-mode` |
| `AETHER_DV_WAVEFORM_NAME` | `--waveform-name` |
| `AETHER_DSTAR_MYCALL` | `--mycall` |
| `AETHER_DSTAR_MYCALL_SUFFIX` | `--mycall-suffix` |
| `AETHER_DSTAR_URCALL` | `--urcall` |
| `AETHER_DSTAR_RPT1` | `--rpt1` |
| `AETHER_DSTAR_RPT2` | `--rpt2` |
| `AETHER_DSTAR_MESSAGE` | `--message` |
| `AETHER_DV_FAIL_FAST=1` | fail startup instead of waiting for a missing serial device |

Command-line values take precedence over environment values. Whitespace in a
value is data, not an argument separator, once the operating system has passed
the argument to the process.

The following diagnostic settings are part of the compatibility profile but
must not be exposed as ordinary user defaults:

| Variable | Accepted value and default | Observable effect |
|---|---|---|
| `AETHER_DSTAR_TX_INVERT` | boolean, default `1` | selects the historical Flex transmit-polarity mapping |
| `AETHER_DSTAR_TX_GAIN` | finite decimal `0.05`–`2.0`, default `1.0` | scales generated baseband before limiting |
| `AETHER_DSTAR_TX_PREAMBLE_MS` | integer `14`–`1000`, default `250` | selects transmitted bit-sync duration |
| `AETHER_DSTAR_TX_NULL_AMBE` | boolean, default `0` | substitutes the verified silence codeword for microphone speech |
| `AETHER_DSTAR_VERBOSE_RX_IDLE_DIAG` | boolean, default `0` | permits high-rate idle-receive diagnostics |
| `AETHER_DSTAR_DIAG_SYNC_TIMING` | boolean, default `0` | reports measured intervals between D-STAR data syncs |
| `AETHER_DV_DIAG_SETUP` | boolean, default `0` | reports registration/setup detail |
| `AETHER_DSTAR_CAPTURE_RX_PATH` | file path, unset | writes little-endian float32, mono, 24 ksample/s receive samples |
| `AETHER_DSTAR_CAPTURE_TX_PATH` | file path, unset | writes little-endian float32, mono, 24 ksample/s transmit samples |
| `AETHER_DSTAR_CAPTURE_RX_META_PATH` | file path, unset | writes receive-packet metadata as CSV |

The `14` ms diagnostic minimum covers at least the standard 64-bit GMSK bit
sync at 4.8 kbit/s; the standard value itself is 64 bits, or 13.333… ms
([JARL 7.0 §4.1.1(a)](https://www.jarl.com/d-star/STD7_0.pdf)). The 250 ms
default and the diagnostic bounds are AetherSDR compatibility policy, not JARL
requirements.

### 3.2 Configuration validation

Configuration is normalized to uppercase ASCII before transmission. Rejection
must occur before radio registration or serial-device use.

| Field | Accepted external value | On-air field |
|---|---|---|
| MYCALL | 3–8 `A`–`Z`/`0`–`9`, with at least one letter and one digit | right-padded to 8 spaces |
| MYCALL suffix | 0–4 `A`–`Z`/`0`–`9` | right-padded to 4 spaces |
| URCALL | 1–8 uppercase letters, digits, or spaces; one leading `/` is allowed; at least one alphanumeric | right-padded to 8 spaces |
| RPT1, RPT2 | 1–8 uppercase letters, digits, or spaces; at least one alphanumeric | right-padded to 8 spaces |
| message | 0–20 printable ASCII bytes `0x20`–`0x7e`, excluding `|` | right-padded to 20 spaces |

The 8/8/8/8/4 routing and call field widths and space padding come from
[JARL 7.0 §4.1.1(f–j)](https://www.jarl.com/d-star/STD7_0.pdf). `DIRECT` for
simplex operation and `CQCQCQ` for a general call come from
[JARL 7.0 §§2.3 and 2.6](https://www.jarl.com/d-star/STD7_0.pdf). The stricter
MYCALL and printable-message checks are AetherSDR boundary policy.

### 3.3 Standard output records

All records are one UTF-8 line terminated by `\n`. Readers must tolerate any
operating-system read boundary, including partial lines and several lines per
read. A consumer may truncate a displayed line after 1,000 characters, but the
helper must not emit malformed partial records intentionally.

Successful initialization is signaled exactly once by:

```text
AETHER_DV_READY mode=<mode> waveform=<name> rx_stream=0x<8-hex> tx_stream=0x<8-hex>
```

It must not be emitted until the serial device is verified, the D-STAR rate is
configured, all required Waveform API commands have successful responses, and
the required stream identifiers are known and distinct.

Other stable records are:

```text
AETHER_DV_ERROR <human-readable-detail>
AETHER_DV_DEVICE state=connected|waiting|disconnected [detail=<single-line-detail>]
AETHER_DV_PROBE verified backend=thumbdv
```

Once per measurement window, metrics use these exact key sets:

```text
AETHER_DV_METRIC v=2 mode=DSTR dir=RX rate_hz=<decimal> vita_gaps=<u64> source_blocks=<u64> turn_mean_us=<decimal> turn_max_us=<u64> queue_max=<u64>
AETHER_DV_METRIC v=3 mode=DSTR dir=TX rate_hz=<decimal> vita_gaps=<u64> null_frames=<u64> pcm_clips=<u64> pcm_invalid=<u64> send_failures=<u64> queue_max=<u64> tail_samples=<u64> tail_us=<u64> preroll_frames=<u64> preroll_delay_ms=<u64> ambe_queue_max=<u64> ambe_underflows=<u64> ambe_overflows=<u64> ambe_sequence_errors=<u64> vocoder_submit_failures=<u64> vocoder_pending_max=<u64> drain_frames=<u64> drain_timeouts=<u64> drain_discarded_frames=<u64>
```

Unsigned values are base-10 and non-negative; decimals must be finite. A
consumer must reject an otherwise metric-looking line containing a duplicate,
missing, non-numeric, negative unsigned, or non-finite field. Unrecognized
diagnostic lines must not change readiness or health state.

The compatibility health window is 188 VITA packet intervals, approximately
one second at 128 frames per packet and 24 ksample/s. A true packet-counter gap
is unhealthy immediately. Five consecutive windows below 23,760 samples/s or
two consecutive source-deficit windows are unhealthy; ten consecutive healthy
windows clear a prior rate/deficit warning. These thresholds are product
policy, not SmartSDR protocol constants.

### 3.4 Exit and supervision behavior

| Situation | Exit status | Required record/behavior |
|---|---:|---|
| successful `--probe-serial` | 0 | `AETHER_DV_PROBE verified backend=thumbdv` |
| invalid/missing argument or unsupported backend/mode | 2 | one `AETHER_DV_ERROR` line |
| serial probe reached device but verification failed | 3 | one `AETHER_DV_ERROR` line |
| normal termination request | 0 | cease transmit, close transports, then exit |
| runtime fatal error | nonzero | one preceding `AETHER_DV_ERROR` line when output remains available |

The AetherSDR parent grants 10 seconds from launch for the readiness record. On
deadline expiry it terminates the child; after 1,500 ms without exit it kills
it. During application shutdown it waits up to 1,500 ms after terminate, then
up to 1,000 ms after kill. These are parent-process compatibility requirements.
POSIX builds ignore `SIGPIPE` and treat `SIGINT`/`SIGTERM` as graceful stop
requests; other platforms provide equivalent semantics.

The parent exposes service states `Stopped`, `Starting`, `Running`, `Stopping`,
and `Failed`. Only a verified readiness record transitions `Starting` to
`Running`; process creation alone never does. A requested stop transitions
through `Stopping` until exit. An unexpected exit, readiness timeout, malformed
readiness record, or fatal helper error transitions to `Failed` with the most
specific available error text.

## 4. SmartSDR Waveform API contract

### 4.1 Control connection and registration

The helper opens the SmartSDR TCP command connection on port 4992. Commands are
`C<decimal-sequence>|<command>\n`; responses are
`R<same-sequence>|<hex-status>|<body>`. Status and message records may arrive
between a command and its response. This grammar and connection behavior are
defined by the pinned [Waveform SDK integration guide](https://github.com/flexradio/waveform-sdk/blob/7c717b034c7c60c883e6c48fb33934c5b7fb776d/doc/README.md)
and [radio transport reference](https://github.com/flexradio/waveform-sdk/blob/7c717b034c7c60c883e6c48fb33934c5b7fb776d/src/radio.c).

The helper subscribes to the slice, radio, and client status needed to identify
the active D-STAR slice and transmit state. It then performs this externally
observable registration transaction, waiting for each matching response:

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
waveform set AetherDStar udpport=<local-port>
```

Removal of a nonexistent prior registration is harmless, but a response is
still required. Every later command requires zero status. The create response
must provide distinct `tx_stream_in_id`, `rx_stream_in_id`,
`tx_stream_out_id`, and `rx_stream_out_id` values. Direction must be derived
from those named response fields, never from bit patterns in the identifiers.
The public SDK defines the create/set transaction and filter/stream concepts
([integration guide](https://github.com/flexradio/waveform-sdk/blob/7c717b034c7c60c883e6c48fb33934c5b7fb776d/doc/README.md),
[API declarations](https://github.com/flexradio/waveform-sdk/blob/7c717b034c7c60c883e6c48fb33934c5b7fb776d/include/waveform_api.h)).
The names, version string, and filter values above are AetherSDR compatibility
profile values.

The compatibility response deadline is five seconds per correlated SmartSDR
command. A timeout fails registration; a late response cannot satisfy a newer
sequence. This deadline is **CORROBORATION REQUIRED** as C17 because it is a
product policy rather than a public SmartSDR protocol constant.

The helper binds one UDP socket, reports its actual local port with the
waveform `udpport` command, receives on that socket, and sends waveform data
from it to radio UDP port 4991. Dynamic local port allocation is preferred and
matches the public [SDK radio/VITA transports](https://github.com/flexradio/waveform-sdk/tree/7c717b034c7c60c883e6c48fb33934c5b7fb776d/src).
Fixed local port 5000 was observed in the checkpoint and is
**CORROBORATION REQUIRED**; it must not be assumed necessary without a real
radio test.

### 4.2 Slice and transmit interaction

Selecting `DSTR` causes the corresponding slice to operate over `DFM`. The
AetherSDR compatibility profile requests:

```text
slice s <slice> fm_deviation=1200 post_demod_low=0 post_demod_high=6000 dfm_pre_de_emphasis=0 post_demod_bypass=1 squelch=0
```

These values are Waveform API integration policy, not D-STAR over-the-air
constants. They require regression testing on firmware 4.2.18.

The public Waveform API exposes active/inactive, PTT-requested, and
unkey-requested lifecycle states and warns that only one stream is active at a
time ([Waveform SDK API declarations](https://github.com/flexradio/waveform-sdk/blob/7c717b034c7c60c883e6c48fb33934c5b7fb776d/include/waveform_api.h)).
The helper may generate a D-STAR transmit waveform only when all of these are
true:

1. the radio's authoritative interlock state represents operator PTT, not TUNE;
2. the active `DSTR` slice is the radio's selected transmit slice;
3. the helper is ready, the serial vocoder is connected and configured, and
   the TX stream is valid; and
4. no stop, mode-change, slice-removal, or hard-cancel event is pending.

TUNE must never start or sustain digital-voice generation. Loss of any
condition cancels generated transmit samples and prevents a stale serial reply
from restarting transmission. An unkey request allows the bounded speech drain
and D-STAR end pattern in §5.7; a completed radio-ready transition after the
tail makes the helper idle. Unkey before the preamble completes aborts the
header and speech rather than transmitting a truncated D-STAR frame.

### 4.3 Waveform sample transport

For non-RAW `DFM`, Waveform API buffers contain paired 32-bit floating-point
left/right audio samples rather than complex RF I/Q. Each VITA packet contains
128 pairs at 24,000 pairs/s, so its nominal interval is 5.333… ms. Public SDK
constants identify the 24 ksample/s, 32-bit, two-frame format and the callback
contract ([Waveform API declarations](https://github.com/flexradio/waveform-sdk/blob/7c717b034c7c60c883e6c48fb33934c5b7fb776d/include/waveform_api.h),
[VITA source](https://github.com/flexradio/waveform-sdk/blob/7c717b034c7c60c883e6c48fb33934c5b7fb776d/src/vita.c)).

The helper accepts only packets whose declared length, stream identifier,
packet class, and payload size agree. Payload floats are network byte order and
must be finite. The 4-bit VITA packet count advances modulo 16 independently
per stream. Timestamps use VITA's UTC integer seconds and real-time fractional
seconds representation used by the public SDK. Outbound packet class fields,
FlexRadio OUI `0x001c2d`, information-class code `0x534c`, audio data-class
codes, timestamp form, and packet layout must match the pinned public
[VITA definitions](https://github.com/flexradio/waveform-sdk/blob/7c717b034c7c60c883e6c48fb33934c5b7fb776d/src/vita.h)
and [serializer](https://github.com/flexradio/waveform-sdk/blob/7c717b034c7c60c883e6c48fb33934c5b7fb776d/src/vita.c).

Received audio is converted to one finite mono signal for D-STAR demodulation.
Generated mono baseband is copied to both outbound audio channels. Replies use
the corresponding *input* stream identifier returned by `waveform create`, as
the pinned SDK does; the SDK notes that output stream identifiers did not work
for returning processed samples
([SDK VITA transport](https://github.com/flexradio/waveform-sdk/blob/7c717b034c7c60c883e6c48fb33934c5b7fb776d/src/vita.c)).

### 4.4 Waveform commands and statuses

The application updates the active slice with:

```text
slice waveform_cmd <slice> set destination_rptr=<RPT2> departure_rptr=<RPT1> companion_call=<URCALL> own_call1=<MYCALL> own_call2=<suffix> message=<message>
```

Spaces inside token values are represented as byte `0x7f`; the helper converts
them to spaces only after validating the full command and converts padded
spaces back to `0x7f` in status tokens. A valid command receives
`waveform response <sequence>|0`; malformed, unknown, or out-of-range input
receives a nonzero response and changes no active configuration. A `status`
request returns the current transmit fields.

At the first accepted receive header and whenever it changes, the helper emits:

```text
waveform status slice=<slice> destination_rptr_rx=<RPT2> departure_rptr_rx=<RPT1> companion_call_rx=<URCALL> own_call1_rx=<MYCALL> own_call2_rx=<suffix>
```

At the start of a transmit it emits the analogous `_tx` fields and may include
`message_tx=<message>`. A newly reconstructed 20-byte received message emits:

```text
waveform status slice=<slice> message=<message>
```

End of a received transmission emits:

```text
waveform status slice=<slice> RX=END
```

Identical repeated headers or messages within one receive session do not
produce duplicate statuses. These command/status names are AetherSDR's public
integration contract with its UI; they are not JARL over-the-air fields.

## 5. D-STAR air-interface behavior

### 5.1 Modulation and rates

D-STAR voice uses GMSK at 4.8 kbit/s with occupied bandwidth no greater than
6 kHz. Voice is 2.4 kbit/s AMBE plus voice FEC to 3.6 kbit/s; the remaining
1.2 kbit/s is slow data. These values are specified by
[JARL 7.0 §3.1.2](https://www.jarl.com/d-star/STD7_0.pdf).

On transmission each bit occupies exactly five 24 ksample/s waveform samples.
The RF result must be Gaussian minimum-shift keying (continuous-phase MSK with
Gaussian pulse shaping) and use the public D-STAR polarity: logical `1` is
positive frequency deviation
([JARL 7.0 Appendix 1.5](https://www.jarl.com/d-star/STD7_0.pdf)). Bit sequences
within protocol fields are transmitted least-significant bit first where the
standard defines a byte/code symbol
([JARL 7.0 Appendix 1.4](https://www.jarl.com/d-star/STD7_0.pdf)).

JARL 7.0 does not publish a Gaussian BT product or a finite host-audio pulse
response. Therefore this specification intentionally does not invent one.
The isolated team must establish the Flex `DFM` discriminator-audio waveform
by a permissibly usable public authority or independent capture, then prove
4.8 kbit/s interoperability and the JARL 6 kHz occupied-bandwidth limit. This
open point is C15 in §10.

The default generated waveform is polarity-inverted at the Flex audio boundary
relative to the logical D-STAR symbol convention, then limited to magnitude
0.98 after the optional gain. This mapping is **CORROBORATION REQUIRED** on
FLEX-6000 hardware; a standards-only implementation must not infer it from the
JARL logical polarity.

### 5.2 Transmission frame

One transmission consists of the following contiguous regions:

| Region | Exact size | Content/source |
|---|---:|---|
| bit sync | standard 64 bits; compatibility default 1,200 bits | alternating `10`, JARL §4.1.1(a); product default is 250 ms |
| frame sync | 15 bits | `111011001010000`, JARL §4.1.1(b) |
| protected radio header | 660 bits | §5.3 |
| repeated voice/data frames | 96 bits each, 20 ms | 72 AMBE bits followed by 24 slow-data bits, JARL §4.1.2 |
| last frame | 48 bits, 10 ms | 32 alternating sync bits, then `000100110101111`, then one `0`, JARL §4.1.2 |

The standard sync, voice/data, and last-frame definitions are in
[JARL 7.0 §§4.1.1–4.1.2](https://www.jarl.com/d-star/STD7_0.pdf). With the
compatibility default preamble, the first voice frame starts nominally
390.625 ms after waveform generation begins: 250 ms bit sync, 3.125 ms frame
sync, and 137.5 ms protected header. Scheduling and hardware buffering may add
latency but must not change emitted sample counts.

For receive acquisition, the exact 15-bit frame sync is accepted only when it
is immediately preceded by the final 16 bits of the standard alternating bit
sync, beginning with logical one. The 16-bit qualifier permits at most two bit
errors; the frame sync remains exact. This bounded product compatibility rule
prevents a frame-sync-shaped noise sequence from initiating protected-header
decoding while remaining within the standard's minimum 64-bit preamble.

### 5.3 Radio header, PFCS, FEC, interleaving, and scrambling

Before protection, the header is exactly 41 bytes in this order:

| Bytes | Field |
|---:|---|
| 3 | flags 1, 2, 3 |
| 8 | destination repeater (RPT2) |
| 8 | departure repeater (RPT1) |
| 8 | companion/destination call (URCALL) |
| 8 | originating call (MYCALL) |
| 4 | originating suffix |
| 2 | P-FCS |

Field order, widths, ASCII encoding, space padding, and the P-FCS field are
specified by [JARL 7.0 §4.1.1](https://www.jarl.com/d-star/STD7_0.pdf). Normal
direct voice starts with all three flag bytes zero.

The P-FCS covers the first 39 bytes. It is CRC-CCITT polynomial
`x^16 + x^12 + x^5 + 1`; the compatible reflected representation is `0x8408`,
initialized to `0xffff`, complemented after the final input byte, and placed
low byte then high byte. The polynomial and reflected form are public in
[JARL 7.0 Appendix 2.1](https://www.jarl.com/d-star/STD7_0.pdf) and the
[JARL English technical requirements](https://www.jarl.com/d-star/shogen.pdf);
initialization,
final complement, and byte placement must be verified against vector V1 below
and an independently captured radio header before release.

Header protection is exactly:

1. Feed the 41 header bytes least-significant bit first, followed by two zero
   termination bits, into a rate-1/2, constraint-length-3 convolutional code.
   Its two outputs are `input XOR delay1 XOR delay2` and
   `input XOR delay2`, starting with both delays zero. This yields 660 bits.
2. Interleave those 660 bits according to the JARL 24-row by 28-column matrix:
   the transmitted order is input positions `row + 24*column`, advancing
   columns within each row and omitting positions 660–671.
3. XOR the interleaved bits with the `x^7 + x^4 + 1` scrambler initialized to
   seven ones at the start of the protected header.

The rate, constraint length, zero termination, byte bit order, interleave
matrix, scrambler polynomial, and reset state are defined by
[JARL 7.0 Appendices 1–2](https://www.jarl.com/d-star/STD7_0.pdf); the two
generator polynomials are also printed in the
[JARL English technical requirements](https://www.jarl.com/d-star/shogen.pdf).
The equations
above are a declarative definition of the two generator outputs, not a required
implementation technique.

Reception reverses these transformations, performs maximum-likelihood decoding
for the published convolutional code, and accepts the header only when its
P-FCS is valid. A failed header must not update callsigns, routing, or transmit
state.

### 5.4 Voice frames and AMBE

Each 20 ms voice/data frame contains 9 AMBE bytes (72 bits) followed by 3 slow
data bytes (24 bits), matching the 3.6/1.2 kbit/s split in
[JARL 7.0 §§3.1.2 and 4.1.2](https://www.jarl.com/d-star/STD7_0.pdf).
The helper gives each 9-byte AMBE unit to the DV3000 exactly once for receive
decode and obtains one 9-byte unit for each 160-sample microphone block on
transmit.

The candidate D-STAR silence codeword is
`9e 8d 32 88 26 1a 3f 61 e8`. It is **CORROBORATION REQUIRED** because no
authoritative public DVSI document located for this specification defines that
specific vector. It may be used only after an isolated hardware or public
interoperability test confirms it.

### 5.5 Slow data, synchronization, messages, and header repetition

The first voice/data frame and every 21st frame thereafter carries the
unscrambled 24-bit data synchronization pattern
`101010101011010001101000` (`aa b4 68` when displayed most-significant bit
first). Thus a superframe is 21 frames or 420 ms. The data-sync position and
pattern are defined by [JARL 7.0 §4.1.2](https://www.jarl.com/d-star/STD7_0.pdf).

All other 24-bit slow-data portions are scrambled, while data sync and the last
frame are not. The scrambler uses `x^7 + x^4 + 1`, starts from seven ones at
each standard-defined initialization point, and produces the conventional
three-byte XOR mask `70 4f 93` for one slow-data fragment. The scope and reset
points are defined by [JARL 7.0 Appendix 1.3](https://www.jarl.com/d-star/STD7_0.pdf);
vector V2 below fixes the required byte interpretation.

A slow-data block spans two adjacent 24-bit fragments after a sync frame. Once
descrambled it contains a one-byte mini-header followed by five payload bytes,
as defined by [JARL 7.0 §6.1](https://www.jarl.com/d-star/STD7_0.pdf).

A 20-byte short message is sent as four five-byte blocks with mini-header bytes
`0x40`, `0x41`, `0x42`, and `0x43`. A receiver may publish the message only
after all four block numbers have been received for the current session; it
replaces missing or conflicting assemblies on the next data sync rather than
mixing sessions. This numbering is normative under
[JARL 7.0 §6.3](https://www.jarl.com/d-star/STD7_0.pdf).

Header repetition uses mini-header types `0x51` through `0x55` to carry the
41-byte header over slow data; the final block contains only the remaining
header byte and padding. An independently received repeat with valid P-FCS may
restore header metadata after a damaged initial header. The assignment and
content are specified by [JARL 7.0 §6.4](https://www.jarl.com/d-star/STD7_0.pdf).

The section 6.3 diagram labels the four blocks `0x40` through `0x43`, and the
message function uses that range for both transmission and reception. `0x44`
is not a short-message block and must be rejected by the message assembler.

### 5.6 Callsign and message behavior

The receive header maps RPT2, RPT1, URCALL, MYCALL, and suffix to the `_rx`
Waveform status fields in §4.4. Trailing spaces are preserved on air and
trimmed only for user display. Embedded spaces remain significant. Direct
operation uses `DIRECT` in both repeater fields; a general call uses `CQCQCQ`.
Repeater-area CQ may use a leading `/` in URCALL, as described in
[JARL 7.0 §§2.4 and 4.2.1](https://www.jarl.com/d-star/STD7_0.pdf).

On transmit, one immutable snapshot of routing, call, suffix, message, and
flags is used for the initial header and all slow-data repeats in that
transmission. A configuration change during PTT applies to the next
transmission, preventing a header/message identity split within one session.

### 5.7 End, drain, and receive-loss behavior

After an unkey request, already accepted speech is drained in order for a
bounded interval, followed by exactly one 48-bit last frame. The last frame is
10 ms at 4.8 kbit/s and must be paced through ordinary 128-pair VITA packets,
not emitted as an unbounded burst. Speech arriving after the drain boundary is
discarded. A hard cancel omits the tail and stops immediately.

The checkpoint used a 280 ms vocoder drain budget and bounded the final sample
tail below 1,024 samples. These values are **CORROBORATION REQUIRED**. The
isolated team must measure ThumbDV worst-case response latency and FLEX-6000
unkey timing, then choose a documented bound that preserves ordered speech
without keeping the radio keyed indefinitely.

The checkpoint tolerated up to two bit differences in a data-sync comparison
and abandoned receive lock after three consecutive missing sync opportunities.
Those are **CORROBORATION REQUIRED** receiver policy values, not JARL constants.
Whatever values are selected must be tested with noise, must bound false lock,
and must emit exactly one `RX=END` for a completed receive session.

## 6. DV3000 serial protocol

### 6.1 Link and discovery

The serial device is opened exclusively. The preferred link is 460,800 bit/s,
8 data bits, no parity, one stop bit, RTS/CTS flow control. DVSI recommends that
combination, a 4 ms USB latency timer, and 1,024-byte host buffers in
[USB-3000 Manual §4.2](https://www.dvsinc.com/manuals/USB-3000_Manual.pdf).

The operator supplies a stable device path. On Linux, `/dev/serial/by-id/...`
is preferred; on macOS, a `/dev/cu.*` path is preferred; on Windows, ordinary
`COMn` and extended `\\.\COMn` spelling identify the same port. Discovery UI
may identify FTDI USB VID:PID `0403:6015`, but this value is
**CORROBORATION REQUIRED** against shipping ThumbDV variants and must never be
the sole match condition.

A 230,400 bit/s, no-flow-control fallback was observed in the checkpoint but
is **CORROBORATION REQUIRED**. It may be enabled only if the isolated team
confirms the applicable hardware switch configuration and documents how it
avoids accidental connection to an unrelated serial device.

### 6.2 Packet envelope and parity

Every AMBE-3000 packet begins with start byte `0x61`, followed by a two-byte
big-endian length and one packet-type byte. The length counts all bytes after
the four-byte envelope. Packet types are `0x00` control, `0x01` channel, and
`0x02` speech. These constants and the maximum packet framing rules are
specified by [AMBE-3000F Manual §§5.2–5.4](https://www.dvsinc.com/manuals/AMBE-3000F_manual.pdf).

After reset the documented interface has parity enabled. A parity-bearing
packet appends field byte `0x2f` and one XOR byte; the XOR covers every prior
packet byte except start byte `0x61`, including the `0x2f` field identifier.
The length includes
the parity field and value. Parity can be disabled with control field `0x3f`
and value `0x00`. This behavior and example packets are in
[USB-3000 Manual §§5.2–5.3](https://www.dvsinc.com/manuals/USB-3000_Manual.pdf).

Startup must work from the documented parity-enabled reset state. The
checkpoint sent initial reset/product probes without parity and then disabled
parity; device acceptance of that sequence is **CORROBORATION REQUIRED**. The
isolated implementation should first exercise the documented sequence and may
add a bounded alternate probe only when independent hardware evidence requires
it.

The receiver rejects wrong start bytes, any packet whose complete encoded size
exceeds the 2,048-byte AetherSDR safety bound,
unknown packet types, truncated packets, parity failures when parity is active,
and fields whose declared content length disagrees with their packet length.
Rejected bytes must not be interpreted as PCM or AMBE data.

### 6.3 Identification and initialization

The following public control fields are used:

| Purpose | Request field/data | Required response |
|---|---|---|
| reset | `0x33` | ready field `0x39` |
| product ID | `0x30` | field `0x30` plus NUL-terminated product text |
| version | `0x31` | field `0x31` plus version text |
| disable parity | `0x3f 0x00` | matching field and success value |
| read configuration | `0x37` | configuration response |
| initialize codec | `0x0b 0x07` | matching field and success value |
| gain | `0x4b 0x00 0x00` | matching field and success value |
| companding | `0x32 0x00` | matching field and success value |
| channel packet format | `0x15 0x00 0x00` | matching field and success value |

Field definitions, ready/product/version responses, codec controls, and result
codes are specified in [AMBE-3000F Manual §§6.1–6.7](https://www.dvsinc.com/manuals/AMBE-3000F_manual.pdf)
and [USB-3000 Manual §§5–6](https://www.dvsinc.com/manuals/USB-3000_Manual.pdf).
The exact values and order after product verification are the AetherSDR
ThumbDV compatibility profile; any unsupported-field response fails readiness.

The product text must identify an AMBE-3000-family device. A response that is
well framed but names another product fails probe rather than being used as a
generic serial device.

D-STAR mode uses rate parameter field `0x0a` followed by six 16-bit big-endian
rate-control words:

```text
0130 0763 4000 0000 0000 0048
```

This is DVSI's published D-STAR rate selection
([USB-3000 Manual D-STAR rate table](https://www.dvsinc.com/manuals/USB-3000_Manual.pdf)).
With parity disabled, the complete request is:

```text
61 00 0d 00 0a 01 30 07 63 40 00 00 00 00 00 00 48
```

Readiness requires a successful response to every required request. A timeout,
negative result, field mismatch, product mismatch, framing failure, or serial
disconnect fails the current initialization generation.

### 6.4 Speech-to-channel encoding

One encode request contains exactly 160 signed linear PCM samples representing
20 ms at 8,000 samples/s. Samples are signed 16-bit, most-significant byte
first. The speech packet is type `0x02`, uses linear-speech field `0x40`, and
declares sample count `0x00a0`, followed by 320 PCM bytes. These field and byte
order requirements are specified by
[AMBE-3000F Manual §5.4 and §6 speech packets](https://www.dvsinc.com/manuals/AMBE-3000F_manual.pdf).

The expected encode reply is a type-`0x01` channel packet containing channel
field `0x01`, bit count `0x48` (72), and exactly 9 AMBE bytes. Other bit counts
do not satisfy a D-STAR request. At most one reply is credited to one submitted
PCM block, in submission order.

### 6.5 Channel-to-speech decoding

One decode request is exactly:

```text
61 00 0b 01 01 48 <nine AMBE bytes>
```

It is a type-`0x01` packet with channel field `0x01`, bit count `0x48`, and
72 channel bits. The expected reply is type `0x02` with speech field `0x00`,
sample count `0x00a0`, and 320 bytes representing 160 signed linear,
big-endian PCM samples. Packet directions, fields, bit/sample counts, and byte
order come from [AMBE-3000F Manual §§5.4 and 6](https://www.dvsinc.com/manuals/AMBE-3000F_manual.pdf).

### 6.6 Timing and transaction semantics

The helper submits no more than one request per 20 ms D-STAR voice frame in
each active direction. The device's serial replies are FIFO, but the helper
must maintain enough request context to distinguish expected encoded-channel
and decoded-speech replies. A reply of the wrong type/field/length is an error,
not a substitute for the expected reply.

Pending work belongs to a connection generation. Reset, disconnect, mode
change, direction change, or hard cancel invalidates the prior generation;
later bytes from it are discarded. The helper must not associate a delayed
encode reply with a newer PTT session or a delayed decode reply with a newer RX
session.

The compatibility deadline for one complete DV3000 response is 100 ms. A
timeout invalidates that transaction; startup fails or runtime recovery begins
as appropriate. Reopen attempts occur no more frequently than once per second,
with cancellation taking effect promptly. These values are **CORROBORATION
REQUIRED** as C16 because they are product limits, not DVSI protocol constants.

Missing transmit AMBE at a 20 ms deadline must not reorder subsequent speech.
After the independently verified silence codeword is available, a single
silence frame may preserve D-STAR timing; otherwise the helper fails closed and
terminates generated transmission. Duplicate or unsolicited replies are
discarded and counted.

## 7. Platform-neutral concurrency and failure semantics

### 7.1 Execution model requirements

The implementation may choose any thread/event-loop architecture, subject to
these externally testable constraints:

- SmartSDR TCP control, UDP sample ingress/egress, DSP timing, and serial I/O
  must make progress independently; blocking serial I/O must not stall VITA
  packet receipt or control responses.
- Waveform callbacks may arrive on different threads over time. They must copy
  and enqueue bounded work promptly; the public SDK explicitly permits this
  callback model
  ([Waveform API declarations](https://github.com/flexradio/waveform-sdk/blob/7c717b034c7c60c883e6c48fb33934c5b7fb776d/include/waveform_api.h)).
- Every queue has a documented finite capacity. Overflow changes a metric and
  applies a deterministic policy; it never allocates without bound.
- A real-time sample path must not wait on a serial transaction, filesystem
  write, UI action, or long-held mutex.
- Direction changes atomically invalidate incompatible queued work. RX AMBE
  cannot appear in TX and TX PCM cannot appear in RX.
- External buffers are length-checked and copied before their owner may reuse
  them.
- Timestamps and sequence numbers use wrap-safe arithmetic; integer overflow
  cannot extend a key or create an unbounded wait.

The checkpoint bounded transmit AMBE pre-roll at 32 frames and dropped the
oldest frame on overflow to bound latency. That exact capacity and drop choice
are **CORROBORATION REQUIRED** through hardware latency measurements.

### 7.2 Failure and recovery

| Failure | Required behavior |
|---|---|
| initial serial device absent, fail-fast off | emit `state=waiting`, remain unready, retry no more frequently than once per second |
| initial serial device absent, fail-fast on | emit error and exit nonzero |
| serial disconnect during RX | stop producing decoded audio, end current RX once, reconnect without transmitting |
| serial disconnect during TX | immediately fail closed, stop generated samples, report error, never reuse pending replies |
| reconnect | close old handle, ensure old reader has ended, open exclusively, reset, identify, configure, then announce connected |
| SmartSDR TCP loss | stop TX, invalidate stream IDs, close UDP, return to unready or exit according to supervisor policy |
| malformed TCP/UDP/serial input | discard bounded unit, count/report error, preserve process integrity |
| output capture open/write failure | report diagnostic; normal radio/vocoder behavior continues unless explicitly configured fail-fast |
| queue overflow | apply documented bounded drop/fail policy, increment metric, never block indefinitely |
| process termination | cancel TX first, invalidate generations, close device/transports, then exit |

Reconnect does not emit readiness until the entire serial and Waveform API
contract is valid again. No recovery path may key the radio automatically.

## 8. Black-box vectors

All byte strings below are hexadecimal. These vectors were generated from the
public formulas in §5 and were not copied from the checkpoint tests.

### V1 — complete protected radio header

Input fields:

```text
flags   = 00 00 00
RPT2    = "DIRECT  "
RPT1    = "DIRECT  "
URCALL  = "CQCQCQ  "
MYCALL  = "N0SPEC  "
suffix  = "A   "
```

Expected 41-byte unprotected header, including P-FCS:

```text
00 00 00 44 49 52 45 43 54 20 20 44 49 52 45 43 54 20 20 43 51 43 51 43 51 20 20 4e 30 53 50 45 43 20 20 41 20 20 20 05 e9
```

Expected 660 protected on-air bits, packed least-significant transmitted bit
into bit 0 of each displayed byte (the high four bits of the final displayed
byte are padding and are not transmitted):

```text
38 cd eb 4d 40 fc 3a a8 a0 60 ab 1c bb a2 b1 5e 1b 65 1b f6 b0 06 d9 36 9b ab b2 82 13 cc 17 fb 5f 03 ed 58 2a 88 92 d8 03 ef f7 a0 53 c3 ea 78 eb 96 67 d7 1c 8e dc e4 9d 6f c5 2e d0 b7 51 ab 9e 2c 2b 07 06 53 c9 11 d4 6a 44 d0 4a 77 5f ae 49 9e 08
```

SHA-256 of those 83 displayed bytes is
`d503406abee1c52a9eea97ec565d54384e4e2ec5a0cbedafe3afc50dd7e54ee5`.
An encoder must match all 660 transmitted bits; a decoder must recover the
fields and accept P-FCS.

### V2 — short message and slow-data scrambling

Message: `CLEANROOM DSTAR TEST` (exactly 20 ASCII bytes).

| Block | Unscrambled six bytes | First scrambled fragment | Second scrambled fragment |
|---:|---|---|---|
| 1 | `40 43 4c 45 41 4e` | `30 0c df` | `35 0e dd` |
| 2 | `41 52 4f 4f 4d 20` | `31 1d dc` | `3f 02 b3` |
| 3 | `42 44 53 54 41 52` | `32 0b c0` | `24 0e c1` |
| 4 | `43 20 54 45 53 54` | `33 6f c7` | `35 1c c7` |

The fragment mask is `70 4f 93`. Interleave a sync frame `aa b4 68` at the
standard superframe position. A receiver presented the four block pairs in any
order within one session publishes exactly one message only after all four
distinct standard block numbers are present. Removing one pair yields no
complete message.

### V3 — DV3000 envelope and control

| Case | Bytes | Expected result |
|---|---|---|
| DVSI manual parity-bearing product request | `61 00 03 00 30 2f 1c` | one parity-validated product response |
| parity-bearing reset | `61 00 03 00 33 2f 1f` | ready response `0x39` |
| parity-bearing disable-parity | `61 00 04 00 3f 00 2f 14` | success before subsequent no-parity traffic |
| D-STAR rate | `61 00 0d 00 0a 01 30 07 63 40 00 00 00 00 00 00 48` | rate success response |
| decode known payload | `61 00 0b 01 01 48 00 11 22 33 44 55 66 77 88` | one 160-sample speech response |

The parity-bearing example is published by
[USB-3000 Manual §5.3](https://www.dvsinc.com/manuals/USB-3000_Manual.pdf);
the packet envelope and field forms are published by
[AMBE-3000F Manual §§5–6](https://www.dvsinc.com/manuals/AMBE-3000F_manual.pdf).
A byte-by-byte serial emulator must split every vector at every possible byte
boundary and obtain the same result as a single write.

### V4 — timing and lifecycle

At 24,000 sample pairs/s:

- each 128-pair VITA packet spans 5.333… ms;
- a 20 ms D-STAR voice/data frame spans 480 sample pairs;
- the 21-frame superframe spans 10,080 sample pairs and 420 ms;
- the 48-bit last frame spans 240 sample pairs and 10 ms;
- the default 1,200-bit bit sync, 15-bit frame sync, and 660-bit protected
  header span 9,375 pairs and 390.625 ms.

Feed a synthetic, jitter-free stream for ten seconds. Packet timestamps,
counter rollover 15→0, and output packet pacing must remain continuous. Insert
one missing counter value: `vita_gaps` increases exactly once and health becomes
unhealthy immediately. Issue unkey before pair 6,000: no protected header,
speech frame, or last frame may be emitted after cancellation. Issue unkey
after speech begins: accepted speech remains ordered and exactly one last frame
appears, subject to the independently established drain bound.

## 9. Acceptance criteria

Release is acceptable only when an isolated implementation team can supply
black-box evidence for all of the following without accessing prohibited
material:

1. Argument/environment precedence, every validation boundary, defaults, exit
   statuses, and exact stable output records match §3.
2. Readiness is withheld for each independently injected serial or Waveform API
   setup failure and appears once after complete success.
3. A fake SmartSDR endpoint observes correlated command/response handling even
   with interleaved status messages, split TCP reads, timeout, error response,
   duplicate response, and four distinct returned stream IDs.
4. VITA parsing rejects every truncated header/payload, wrong stream, wrong
   packet class, non-finite float, and inconsistent length without crash or
   unbounded allocation.
5. Valid VITA packet counts roll over modulo 16; gaps, duplicates, and reordered
   packets produce deterministic metrics and no stream cross-routing.
6. V1 matches all 660 bits and round-trips with P-FCS validation. Every
   one-bit mutation in the unprotected header is rejected unless the supplied
   P-FCS is recomputed.
7. Independent tests cover convolutional decode at representative error
   positions, short interleave input, and malformed scrambler lengths.
8. A modulation analyzer measures 4.8 kbit/s, five samples/bit at 24 ksample/s,
   continuous phase, correct public logical polarity before the Flex boundary,
   and no non-finite or magnitude-over-0.98 output after gain/limit.
9. A known-good D-STAR receiver decodes the V1 callsigns and V2 message from a
   generated waveform; a known-good transmitter is decoded in the reverse
   direction.
10. V2 reconstructs once, not once per superframe; missing, duplicate,
    conflicting, and out-of-session blocks cannot combine into a false message.
11. Header repetition is accepted only with valid P-FCS and never changes the
    transmit configuration.
12. A byte-fragmenting DV3000 emulator validates every envelope length, packet
    type, field, parity state, rate word, 160-sample speech unit, and 72-bit
    channel unit in §§6 and V3.
13. Real ThumbDV hardware passes reset, product identity, version, parity
    transition, D-STAR configuration, 1,000 sequential encode requests, and
    1,000 sequential decode requests without reorder or leak.
14. Serial unplug during idle, RX, preamble, active TX, and drain always yields
    bounded recovery; none can cause automatic or prolonged transmit.
15. A late serial reply from an invalidated generation is counted and ignored.
    It cannot be attached to a later PTT/RX session.
16. TUNE, a non-TX slice, removal of the D-STAR slice, mode change, TCP loss,
    process stop, and helper not-ready all inhibit generated transmission.
17. Sustained load proves every queue bound and overflow policy, while VITA
    ingress and control responses continue to make progress.
18. Metrics accept arbitrary read fragmentation and reject missing, duplicate,
    negative, overflowed, and NaN/Inf fields.
19. Windows, macOS, and Linux each complete launch, probe, graceful stop,
    forced-stop fallback, exclusive serial ownership, and reconnect tests.
20. A real FLEX-6000 on firmware 4.2.18 completes D-STAR simplex transmit and
    receive with intelligible audio, correct callsigns/message, exactly one RX
    end notification, no tune-triggered voice, and no slice-0 RX regression.

Tests involving RF transmission must use a dummy load or otherwise lawful,
controlled test setup and require an explicit human operator action. No
acceptance automation may key a radio merely because a device reconnected.

## 10. Source-only observations requiring independent corroboration

Nothing in this section is an implementation instruction. Each item is a lead
that the isolated team must confirm from public documentation, an independently
captured device/radio session, or a fresh black-box experiment before making it
normative.

Stable ID C5 is resolved by JARL D-STAR Standard 7.0 section 6.3, PDF page 64,
which normatively assigns message mini-headers `0x40` through `0x43`. C5 is no
longer quarantined and is intentionally absent from the unresolved table.

| ID | Observation from checkpoint/source-analysis side | Required independent evidence |
|---|---|---|
| C1 | fixed local waveform UDP port 5000 | compare dynamic and fixed ports on firmware 4.2.18; retain command/response and packet capture |
| C2 | exact DFM slice values in §4.2 | real-radio before/after status and interoperable RX/TX capture |
| C3 | Flex transmit baseband needs compatibility inversion and 0.98 limiting | RF/baseband measurement plus decode on an independent D-STAR receiver |
| C4 | silence AMBE codeword `9e 8d 32 88 26 1a 3f 61 e8` | DVSI authority or independently recorded ThumbDV/known-radio silence frame |
| C6 | initial no-parity reset/product request is accepted | power-cycle capture from at least two ThumbDV units; compare documented parity-first sequence |
| C7 | 230,400 bit/s without flow control is a useful fallback | hardware switch/manual evidence and successful identity/configuration stress test |
| C8 | FTDI VID:PID `0403:6015` identifies supported ThumbDV hardware | inventory of shipping variants; prove no unrelated-device false match |
| C9 | 280 ms drain and tail below 1,024 samples | measured latency distribution and unkey behavior under loaded ThumbDV/FLEX hardware |
| C10 | 32-frame transmit pre-roll, oldest-drop overflow | measured end-to-end latency and intelligibility under controlled queue pressure |
| C11 | receive sync accepts two differing bits and unlocks after three misses | BER/false-lock test against independent transmitters and noise captures |
| C12 | UI health thresholds in §3.3 are operationally appropriate | ten-minute jitter/loss runs on each supported platform and live radio |
| C13 | observed FLEX-6700 UNKEY→READY around 345 ms and very short keys below about 110 ms fail to start a complete frame | fresh firmware-4.2.18 lifecycle trace with synchronized TCP/VITA timestamps |
| C14 | four create-response stream IDs are always present and outbound data must use input IDs | compare pinned SDK with live create responses on supported firmware |
| C15 | the checkpoint's finite host-audio Gaussian pulse shape is the correct Flex `DFM` drive for interoperable D-STAR GMSK | derive independently from a usable public authority or captured discriminator/baseband samples; verify 4.8 kbit/s decode and ≤6 kHz occupied RF bandwidth |
| C16 | 2,048-byte serial packet cap, 100 ms response deadline, and one-second reopen interval are safe on supported ThumbDV hardware | stress product/version replies, worst-case codec latency, unplug/replug, and cancellation on each platform |
| C17 | five seconds is the correct per-command SmartSDR response deadline inside the parent's ten-second readiness window | inject delayed responses and measure live firmware 4.2.18 registration under load |

If corroboration changes one of these values, the implementation PR must link
the new evidence and update this specification before or with the behavioral
change.

## 11. Provenance and source-access log

### 11.1 Specification-team access

The specification team was authorized to inspect the licensing-remediation
checkpoint, its tests, the existing architecture note, and the public sources
listed in §2. It used the checkpoint only to identify externally observable
requirements and uncertainty candidates. It did not copy source text,
comments, identifiers for internal routines or data structures, table layout,
control flow, or pseudocode into this document.

| Date | Material accessed | Purpose | Information admitted to this document |
|---|---|---|---|
| 2026-07-11 | checkpoint worktree at commit `607e00ce` | enumerate process, radio, serial, telemetry, timing, and failure observations | externally observable contract only; uncertain values isolated in §10 |
| 2026-07-11 | checkpoint black-box/unit tests | identify boundary cases and observable acceptance needs | new acceptance statements and independently chosen vectors; no test text/layout copied |
| 2026-07-11 | existing AetherSDR digital-voice architecture note | locate public authorities and operator-facing settings | independently rewritten functional behavior only |
| 2026-07-11 | JARL D-STAR Standard 7.0 | authoritative air interface | cited constants, frame fields, FEC/interleave/scramble, callsign/message rules |
| 2026-07-11 | DVSI USB-3000 and AMBE-3000F manuals | authoritative serial/vocoder interface | cited packet, parity, link, rate, speech, and channel requirements |
| 2026-07-11 | FlexRadio Waveform API site and pinned public SDK | authoritative radio integration | cited lifecycle, command, stream, callback, UDP, and VITA requirements |

No RF or serial hardware capture was available to the specification team in
this pass. Statements that need such evidence are therefore explicitly marked
**CORROBORATION REQUIRED**.

### 11.2 Sanitization record

The document is sanitized for handoff because:

- it contains only externally observable requirements, published protocol
  formulas, black-box vectors derived afresh from those formulas, and explicit
  uncertainty records;
- internal source routine/type/member names, source file inventory, source
  comments, control flow, pseudocode, and implementation-oriented structure
  are absent;
- all public protocol constants and algorithms are adjacent to a public JARL,
  DVSI, or FlexRadio citation;
- checkpoint-only values are labeled product policy or collected in §10 for
  independent corroboration; and
- §1 gives the isolated implementation team an explicit, reviewable source
  prohibition.

The implementation team's own provenance log must start with this document and
the public authorities in §2. It must record every additional capture, test
fixture, public source, and person who supplies protocol information.
