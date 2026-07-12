<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Integration contract

## Boundary and ownership

`crdv` never opens a device, creates a thread, reads a clock, allocates heap memory, blocks, sleeps, keys a transmitter, or performs AMBE encode/decode. The parent owns every buffer passed to the library. Unless a function's output is an embedded library object, input and output buffers must not overlap. All size arguments are byte, bit, sample, or element counts exactly as named.

Instances (`crdv_air_receiver`, parsers, assemblers, counters, and transaction sets) are single-owner objects. A parent may use separate instances on separate threads. Concurrent access to one instance requires parent-side serialization. This keeps callback and queue policy outside the real-time sample path.

All functions finish synchronously. Return values are from `crdv_result`; a nonzero result leaves caller-visible output unchanged except documented counters, parser partial state, `written=0`, or transaction removal after a terminal reply/timeout.

## Callbacks

### DV3000 packet callback

`crdv_dv_parser_init` stores `crdv_dv_packet_callback` and its opaque context. `crdv_dv_parser_feed` invokes it synchronously once per accepted complete packet. `crdv_dv_packet_view.fields` points into the parser's fixed 2,048-byte buffer and is valid only until the callback returns. Copy required fields before returning. The parser rejects bad start bytes, zero/oversize declared lengths, unknown types, and required-parity failures. It never treats rejected bytes as speech/channel data.

The parent must change parser parity with `crdv_dv_parser_set_parity` only after the documented disable-parity response succeeds. That operation drops any partial packet.

### Line callback

`crdv_line_reader_init` stores `crdv_line_callback` and context. `crdv_line_reader_feed` invokes it synchronously for each complete LF-terminated record; CR before LF is removed. The pointer is valid only during the callback. A line exceeding 1,000 bytes is discarded through the next LF and increments `rejected`. A partial final line is retained, never emitted.

### Air callbacks

`crdv_air_receiver_init` copies four optional callbacks and context:

- `header`: called only after qualified initial frame acquisition, 660 protected bits, maximum-likelihood convolutional recovery, and valid P-FCS. The pointed `crdv_header_fields` is callback-local and must be copied.
- `voice`: called once per complete 96-bit frame with copied nine-byte AMBE and three-byte slow-data arrays. `data_sync=true` identifies the unscrambled standard sync. AMBE must be submitted exactly once to hardware by the parent.
- `end`: called at most once when the exact 48-bit last frame is observed.
- `sync_event`: called once for each accepted exact/tolerant sync, early/late sliding realignment, expected-sync miss, and lock loss. The event is callback-local and includes the 0-20 superframe position, signed bit offset, Hamming distance, and consecutive-miss count.

Callbacks execute inside `crdv_air_receiver_push`; they must copy/enqueue bounded work and return promptly. `crdv_air_receiver_cancel` invalidates partial receive state without calling `end`.

### Receive synchronization policy

Initial acquisition requires the exact 15-bit JARL frame sync to be immediately
preceded by 16 alternating bit-sync bits beginning with logical one. At most two
of those 16 qualifier bits may differ. This bounded qualifier is a false-lock
guard: every conforming transmission supplies at least 64 alternating bit-sync
bits, while an isolated frame-sync-shaped noise pattern cannot enter header
recovery. It does not relax the frame-sync pattern itself.

`crdv_air_receiver_init` zero-initializes `crdv_receive_sync_policy`. That exact/default policy has maximum Hamming distance 0, miss limit 0, sliding disabled, and realignment span 0. It preserves exact fixed-frame recognition and never drops voice lock merely because an expected data sync is absent. New counters/events are observational and do not change the default voice callback behavior.

The parent may call `crdv_air_receiver_set_sync_policy` only while the receiver is in `CRDV_RX_SEARCH`. The function rejects Hamming distances above 24, a realignment span when sliding is disabled, and sliding spans outside 1-24 bits. The 24-bit span cap is a bounded-buffer/API safety limit tied to the public sync length; it is not an AetherSDR compatibility recommendation. `consecutive_miss_limit=0` disables miss-triggered unlock; 1-255 drops lock on that many consecutive expected-sync misses.

At fixed 96-bit boundaries, a distance at or below the configured maximum is accepted. An accepted sync resets the 21-frame superframe position and consecutive-miss count. Exact and nonzero-distance accepts produce `CRDV_SYNC_EXACT` and `CRDV_SYNC_TOLERANT` respectively.

When sliding is explicitly enabled, only an expected superframe sync opportunity opens a search window around the nominal end of the slow-data field. The window is the caller's `max_realign_bits` before and after the boundary. The first acceptable candidate produces `CRDV_SYNC_REACQUIRED_EARLY` or `CRDV_SYNC_REACQUIRED_LATE` with a signed offset. Because a bit discontinuity makes the current 96-bit AMBE/slow-data division ambiguous, that one frame is not sent to the voice callback; `reacquired_frame_drops` increments and the next bit starts a newly aligned frame. A zero-offset candidate still produces the ordinary voice callback.

If no sliding candidate is accepted, the parser preserves any look-ahead bits, emits one `CRDV_SYNC_EXPECTED_MISS`, and continues at the nominal boundary unless the configured miss limit is reached. On the limit, it emits one `CRDV_SYNC_LOCK_LOST`, invokes `end` once, and returns to frame-sync search. Additional bytes cannot repeat either callback for that loss. `rejected_candidates` counts a fixed-boundary or in-window candidate exactly one bit beyond the configured Hamming threshold; more distant ordinary windows are not treated as candidates.

`crdv_air_receiver_get_sync_counters` copies cumulative exact, tolerant, early, late, rejected-candidate, expected-miss, lock-loss, and realigned-frame-drop counters. `crdv_air_receiver_cancel` preserves callbacks, policy, and cumulative counters while clearing partial lock state. Reinitializing the object resets all three.

Sliding search is deliberately immediate and bounded, not a statistical false-lock detector. Increasing Hamming distance or window width increases false acceptance risk. The AetherSDR parent must not select compatibility values until independent C11 BER/noise/false-lock evidence exists.

## D-STAR fields and sample flow

`crdv_config_normalize` performs uppercase ASCII conversion, defaults, checks, and exact on-air space padding. `crdv_header_pack` inserts PFCS; `crdv_header_unpack` never returns routing metadata for a failed PFCS. `crdv_header_protect` and `crdv_header_recover` use exactly 660 transmitted bits; the high four bits of packed byte 83 are never transmitted.

`crdv_transmission_prefix` emits caller-selected even bit sync (minimum 64), the 15-bit frame sync, and 660 protected header bits. The compatibility value 1,200 is a parent choice, not a library default. `crdv_voice_frame_pack` emits 72 AMBE bits followed by 24 slow-data bits. The parent schedules one frame per 20 ms and calls `crdv_last_frame_bits` once after its independently selected drain boundary. A hard cancel emits no tail.

`crdv_message_block` takes one-based block numbers 1-4 and transmits only the standard `0x40`-`0x43` wire mini-headers. The assembler accepts only that same range and rejects `0x44`. Assemblers are session-tagged. A caller must increment/reset the session at data-sync boundaries so missing or conflicting fragments cannot mix across superframes. `crdv_header_repeat_block` emits eight `0x55` five-byte chunks followed by one `0x51` one-byte chunk; receive publication requires all 41 bytes and valid P-FCS.

`crdv_gaussian_taps` requires explicit positive BT, five samples/bit for the specified 24 ksample/s / 4.8 kbit/s integration, and an explicit symbol span. `crdv_modulate_discriminator` returns exactly `bit_count * samples_per_bit` finite mono frequency-control samples limited to magnitude 0.98 after gain. The parent duplicates each mono sample into the two Flex audio channels. Logical 1 is positive before optional `invert`. No BT/span/invert value is normative until C3/C15 are resolved.

`crdv_demodulate_discriminator` is a bounded hard slicer for symbol-timed discriminator samples. Carrier/timing acquisition, soft metrics, and resampling remain parent DSP responsibilities; the parent feeds one recovered hard bit per 4.8 kbit/s symbol to the air receiver. The library's optional data-sync policy can repair bounded bit-count discontinuities after that handoff but does not replace carrier/timing recovery.

## DV3000 transport and state

The serial transport must be exclusive, 8N1, and initially use the documented 460,800 bit/s RTS/CTS setup. It copies outgoing packets before an asynchronous write and preserves byte order. `crdv_dv_build_startup` produces the parity-first reset/product/version/disable sequence, then no-parity configuration, codec, gain, companding, channel format, and D-STAR rate requests. The parent advances only after a matching successful response and calls `crdv_dv_product_is_ambe3000` before readiness.

`crdv_dv_build_encode` consumes exactly 160 signed host-order PCM samples and copies them into one big-endian request. `crdv_dv_build_decode` consumes exactly nine AMBE bytes. Reply accessors require the exact type, field, count, and length before copying.

`crdv_dv_transactions` is a fixed 16-entry FIFO. The parent supplies monotonic absolute deadlines to `submit`, calls `accept` for a complete parsed reply, and calls `expire` as its clock advances. Wrong replies consume and fail the expected FIFO transaction; unsolicited replies are discarded. Reset, disconnect, mode/direction change, hard cancel, and new PTT/RX sessions must call `invalidate`; its generation changes and queued work is discarded. A parent transport must tag asynchronous writes/reads with the same external generation so bytes from a closed handle are not fed into a new parser instance.

No timeout is embedded. C16 must establish the response and reopen intervals. The parent may submit no more than one codec request for each active 20 ms frame in each direction. It must never invent, reorder, or reuse AMBE replies. There is no built-in silence codeword.

## SmartSDR control and VITA

`crdv_line_reader` handles arbitrary TCP read boundaries. `crdv_control_parse_response` accepts only `R<decimal>|<hex>|<body>`. `crdv_control_transactions` holds 16 distinct sequences with parent-supplied monotonic deadlines. Status/command lines do not call `accept`. An unknown, duplicate, invalidated, or late response cannot complete another sequence. A nonzero correlated status returns `CRDV_E_CHECK`.

After `waveform create`, call `crdv_control_parse_create_streams`; it requires all four named, nonzero, distinct stream IDs. Direction comes from the name, never the numeric pattern. The parent owns the fixed registration command list and must wait for each response before readiness. C17 must select the deadline.

`crdv_vita_parse_audio` accepts exactly one Flex IF-data packet with stream ID, class present, UTC/realtime timestamps, OUI `0x001c2d`, information class `0x534c`, audio/float/24k/32-bit/two-frame class `0x03e3`, 128 stereo pairs, and 1,052 total bytes. All payload floats must be finite. `crdv_vita_build_audio` serializes the same layout and uses the supplied input stream ID for return data. The parent copies a received UDP datagram before the original owner may reuse it.

`crdv_vita_counter` is per stream. Distance 1 modulo 16 is continuous; 0 is a duplicate; forward distances 2-8 add exact gaps; distances 9-15 are classified reordered. Never share a counter across stream IDs.

`crdv_metric_validate` validates the complete exact v2 RX or v3 TX key set. It rejects missing, duplicate, extra, negative unsigned, overflowed, and non-finite values. Health-window state and threshold policy remain in the parent.

## Required parent queue and lifecycle policy

Every asynchronous boundary must use a documented fixed-capacity queue. The parent selects capacities and deterministic overflow actions. A real-time VITA/DSP callback may only validate, copy, enqueue, and return. It must not wait for serial, disk, UI, network control, or a long-held mutex.

Before generating TX, the parent must atomically confirm operator PTT (never TUNE), selected DSTR TX slice, ready serial generation, valid TX stream, and no stop/mode/slice/hard-cancel event. Losing any condition invalidates TX and serial generations. Reconnect may reestablish readiness but may never key automatically. An unkey after speech begins may perform the bounded ordered drain and one ordinary-paced tail; unkey during the prefix cancels it.

## CORROBORATION REQUIRED gates

The following remain outside normative library defaults and keep `crdv_quarantined` skipped:

C5 is resolved by JARL D-STAR Standard 7.0 section 6.3, PDF page 64: the four message mini-headers are `0x40`-`0x43`. It is intentionally absent from the unresolved table.

| ID | Integration gate |
|---|---|
| C1 | Dynamic versus fixed UDP port on firmware 4.2.18. |
| C2 | Exact DFM slice values with live before/after state. |
| C3 | Flex audio polarity inversion and 0.98 boundary measurement. |
| C4 | Candidate silence AMBE word; library contains none. |
| C6 | Optional no-parity startup probe. |
| C7 | 230,400 bit/s no-flow-control fallback. |
| C8 | FTDI VID/PID inventory and false-match analysis. |
| C9 | Drain time and maximum tail samples. |
| C10 | AMBE pre-roll capacity and overflow action. |
| C11 | Parent choices for sync Hamming distance, consecutive-miss unlock limit, sliding enablement/span, and their BER/false-lock behavior. The library defaults none of them. |
| C12 | Product health thresholds under supported-platform load. |
| C13 | Firmware 4.2.18 unkey/ready and short-key timing. |
| C14 | Live four-stream response and return through input IDs. |
| C15 | BT/span/pulse response for interoperable Flex DFM GMSK and occupied bandwidth. |
| C16 | DV3000 packet cap, response deadline, and reopen interval. The parser safety cap remains 2,048 but is not claimed as a hardware limit. |
| C17 | Per-command SmartSDR response deadline inside readiness supervision. |

RF acceptance must use a dummy load or lawful controlled setup and explicit human operator action.
