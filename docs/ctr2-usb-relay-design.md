# CTR2 USB HID relay design

Status: Updated proposal, assuming Lynn agrees to the opaque HID relay
approach. Jeremy reports that the TCP relay is working. HID firmware and
end-to-end USB operation have not yet been verified.

## Goal

Add USB as a third CTR2 operating mode alongside Wi-Fi and MIDI. USB mode
uses the same controls, default assignments, programming method, display
feedback, onboard paddle keyer, and local sidetone as Wi-Fi mode. It requires
no MIDI mapping or configuration import into AetherSDR.

Only the transport changes. AetherSDR carries the controller's radio traffic
over the PC's network connection, including a reachable VPN or tailnet route.
The CTR2 needs neither Wi-Fi credentials nor access to that network in USB
mode. USB is the sole radio transport in this mode; commands are not also
sent over Wi-Fi.

Lynn will provide the firmware implementation. Binary patching and reverse
engineering are not dependencies of this design.

## Architecture

```text
CTR2 controls, display, keyer, and local sidetone
                     |
              Bidirectional USB HID
                     |
          AetherSDR HID transport adapter
                     |
          Dedicated radio TCP connection
                     |
                  Flex radio
```

The existing TCP relay is the starting point. Replace its controller-facing
TCP connection with HID framing and reassembly. Keep a dedicated upstream
radio connection, separate from AetherSDR's own command connection.

After the initial HID handshake, forward radio payload bytes unchanged in
both directions. The adapter understands HID framing only. It does not
inspect or interpret the enclosed radio protocol.

- Forward all controller traffic and all replies and unsolicited output
  received on its dedicated radio connection.
- Preserve byte values and order, including line endings and timing fields.
- Do not parse commands, filter status, renumber sequences, rewrite handles,
  translate CW timestamps, emulate the radio, or manufacture replies.
- Do not route payload through RadioModel command dispatch or an existing
  AetherSDR backend command socket.
- Keep transport errors and diagnostics outside the radio byte stream.

The radio processes the CTR2's commands and returns the results. No list of
supported API commands or copy of Lynn's command parser is needed in the
host relay. HID message boundaries need not match API command boundaries;
TCP payload can be split into transport chunks and reconstructed in order.

The CTR2 remains an independent radio client. Its existing registration and
MultiFlex binding behavior remains its responsibility. The relay does not
assign AetherSDR's client identity or guarantee selection of its GUI session.

## HID agreement

Lynn reports that his controllers already send HID reports with 8-byte
payloads, currently carrying MIDI data. He will add the receive path and the
USB operating mode. Device identifiers, report descriptor, report IDs, and
host-to-device report details will come from Lynn before implementation.

Use his proposed small envelope, with an exact payload byte length added
so the receiver can distinguish data from final-report padding:

| Report | Proposed contents |
| --- | --- |
| Message header | Start marker, protocol version, 8-bit message counter, packet count, and exact payload byte length |
| Message data | Matching message counter followed by up to seven payload bytes |

A compact candidate header fits the existing eight payload bytes: one byte
each for marker, version, and counter; two bytes each for packet count and
payload length; one reserved byte. This is a proposal for agreement with
Lynn, not a finalized wire format. Agree on byte order, marker value, report
IDs, padding, and a practical maximum message length together. A two-byte
length caps each message at 65,535 payload bytes; longer streams use multiple
messages. Packet count must agree with the length and seven-byte fragments.

Send each message's fragments in order, without interleaving messages in the
same direction. Each direction has its own counter, wrapping modulo 256.
These are transport counters, not Flex command sequences. Reassembly state
and report boundaries distinguish headers from fragments; do not search the
radio payload for a special marker.

The initial handshake identifies a compatible relay protocol version and
establishes readiness before radio bytes are forwarded. AetherSDR opens the
configured radio connection and retains any initial radio output until the
HID side is ready. The precise handshake and connection-failure indication
must be agreed with Lynn. A new connection starts with fresh framing state.
No application heartbeat, command acknowledgments, or command-specific
message types are required by this proposal.

The only ongoing inspection is of the HID envelope needed for transport:
length, counter, and fragment completeness. It never extends into the radio
payload. Terminal output and MIDI events must not be mixed into the relay
payload stream.

## Applet and connection lifecycle

Extend the working relay applet with HID device discovery and selection,
the upstream radio address and port, Connect and Disconnect, connection
state, and directional byte counters. Enumerate compatible devices using
Lynn's identifiers and report information; distinguish multiple controllers
using the available device identity. Start with one controller connection.

The operator selects USB mode on the controller, plugs it in, selects it in
AetherSDR, chooses the reachable radio endpoint, and connects. The host and
controller complete the handshake, then relay bytes in both directions.
The destination remains fixed for that connection. Changing the selected
AetherSDR radio must not silently retarget it.

Use bounded buffers and preserve partial transfers without blocking the UI.
Agree on maximum message size and incomplete-message timeout with Lynn.
Do not release an incomplete HID message as a valid payload. Backpressure,
USB throughput, and fragment scheduling must be tested under sustained radio
output and CW operation. Eight-byte reports are the starting point, not a
proven bandwidth or latency result.

On disconnect, framing failure, or transport error, end the connection and
clear pending buffers and reassembly state. Reconnection requires a fresh
handshake and radio connection. Never replay old commands, reuse stale
fragments after counter wrap, or reconnect the radio underneath an ongoing
controller session. Show the failure in the applet; do not inject radio
protocol text to explain it.

## CW behavior

The CTR2 runs its existing keyer and generates local sidetone. AetherSDR
forwards the resulting bytes exactly as it forwards all other traffic.
It neither runs another paddle keyer nor changes event timestamps or indices.

This is an independent external-client connection, not AetherSDR's own
transmit command path. Do not claim TxCoordinator admission or ownership
for opaque traffic. The relay must not generate key-down, retry commands,
or invent a key-up on disconnect. Loss of the connection does not prove the
radio is idle. Validate failure behavior with Lynn and the operator as part
of controlled CW testing.

## Full transport coverage

The goal remains replacement of all radio traffic used by Wi-Fi mode, not
only CW. The first HID milestone carries the entire TCP byte stream through
the proven relay approach.

UDP discovery and streams are separate from TCP. Ask Lynn which of these
his controllers require for full Wi-Fi-mode functionality. If required,
agree on a minimal transport-level way to identify their endpoints and carry
opaque datagrams while preserving datagram boundaries. Radio payloads still
remain unchanged; do not parse TCP commands to infer UDP routing or rewrite
embedded addresses. Endpoint setup must be explicit between the firmware
and host transport.

A successful TCP-over-HID test does not establish full UDP-dependent feature
parity. Account for every required channel before calling USB a complete
replacement. This follow-up must not introduce radio-command mediation.

## Implementation boundaries

Reuse the working TCP relay's upstream connection and byte-pump behavior
where its implementation permits. Put HID framing, reassembly, and transport
in libaethercore, expose status and controls through a model, and keep the
applet limited to UI. Use existing theme, accessibility, configuration, and
lifetime conventions. Any saved preferences belong to one feature-owned
AppSettings document; never persist pending traffic or transmit state.

Choose the HID host implementation after checking existing dependencies and
Lynn's report descriptor. USB serial is not the transport in this design.
Verify enumeration, device access, report sizing, and teardown on Linux,
macOS, and Windows. Do not assume identical report-ID handling across host
APIs. No additional thread or dependency is selected by this document.

RFC #6091 (approved) covers this design and the independent-client transmit
boundary: the relay runs only after the operator enables it, and AetherSDR's
existing transmit indicator covers on-air visibility. The host HID API choice
is confirmed once the descriptor is known. This design does not change
AetherSDR's existing transmit policy or its own radio command paths.

## Next steps and acceptance

1. Agree with Lynn on the eight-byte report layout, exact length field,
   counter behavior, initial handshake, and disconnect/error signaling.
   Obtain device identifiers and the report descriptor.
2. Lynn adds USB mode and bidirectional HID transport beneath the controller's
   existing Wi-Fi radio logic. AetherSDR adds the matching HID adapter and
   device selection to the working relay.
3. Test framing independently with arbitrary bytes, padding, fragmentation,
   counter wrap, malformed/incomplete reports, bounded buffering, and
   disconnect/reconnect. Compare complete reconstructed streams in both
   directions; they must match the original bytes exactly.
4. Test hardware startup, radio greetings, controls, display updates, and
   MultiFlex binding. Verify AetherSDR's existing RX operation remains intact.
5. With the operator's transmit authorization and test setup, verify paddle
   keying, local sidetone, latency under load, and disconnect behavior.
6. Confirm required UDP coverage with Lynn and test those features before
   declaring full Wi-Fi/USB parity. Repeat on the intended VPN/tailnet route
   and record the host platforms actually verified.

No packet capture or API-parser implementation is a prerequisite for the
opaque relay. Use captures only to investigate observed failures when useful.
The inputs to this design are Jeremy's requirements, his report of the
working TCP relay, and Lynn's HID proposal. Host implementation must remain
independent of proprietary firmware disassembly.

## Host-side status

What exists on the AetherSDR side so the firmware work has a concrete
counterpart. Everything below is a draft until agreed with Lynn.

| Piece | File | State |
| --- | --- | --- |
| Opaque byte pump, both directions, bounded | `src/core/ByteRelay.{h,cpp}` | Working with the TCP relay |
| Upstream radio connection and session lifecycle | `src/core/TcpByteProxy.{h,cpp}` | Working with the TCP relay |
| HID envelope encoder and reassembler | `src/core/Ctr2HidFraming.{h,cpp}` | Draft; unit-tested, no device yet |
| HID device adapter, enumeration, handshake | not started | Needs the items below |

`ByteRelayEndpoint` is the seam for USB mode: a HID adapter implements it on
the controller side (reports in, reassembled payload out; payload in, encoded
reports out) and the radio side keeps the existing upstream TCP connection
and per-connection session pattern. The relay itself does not change.

### Draft envelope as implemented

`Ctr2HidFraming.h` keeps every negotiable value in one `WireFormat` struct.
A report here means the eight payload bytes, excluding any HID report ID.

| Field | Draft value |
| --- | --- |
| Header layout | marker, version, counter, packet count (2), payload length (2), reserved |
| Byte order of 2-byte fields | Little-endian |
| Marker | `0xA5` |
| Version | `1` |
| Reserved byte | Must be `0` |
| Data report | Counter, then up to 7 payload bytes; padding ignored via exact length |
| Counters | Per direction, start at `0` on a fresh connection, wrap modulo 256 |
| Maximum message | 4096 payload bytes (length field allows 65,535); longer writes become several messages |
| Packet count | Must equal ceil(length / 7) |

Reassembly is fail-closed. Report position, never content, separates headers
from data, so a payload byte equal to the marker is just data. Any wrong
marker, version, reserved byte, message counter, zero or oversize length,
packet-count mismatch, data-report counter mismatch, or report that is not
8 bytes puts the reassembler into a failed state. It releases no partial
message and refuses input until reset, and the owner ends the connection.
The incomplete-message timeout belongs to the adapter that owns the device,
using `isMidMessage()`.

`tests/ctr2_hid_framing_test.cpp` covers every byte value, lengths around the
7-byte boundary up to 65,535, non-zero padding, splitting into many messages
across counter wrap, independent sender chunking, marker-valued payload, each
failure class above, sticky failure until reset, and partial-message holding.

### Needed from Lynn

- USB vendor and product IDs, and how to tell two controllers apart.
- The report descriptor, report IDs, and the host-to-device report.
- Sign-off on, or changes to, the draft table above.
- The initial handshake: version check, readiness, and how a failed radio
  connection is signalled to the controller.
- Maximum message size and the incomplete-message timeout.
- Which UDP channels Wi-Fi mode relies on, if any.

hidapi is already an optional AetherSDR dependency for other USB controllers;
the host HID API is still to be chosen once the descriptor is known, and must
be verified on Linux, macOS and Windows.
