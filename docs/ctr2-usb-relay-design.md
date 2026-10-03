# CTR2 USB HID relay design

Status: link format v0 agreed with Lynn; host side implemented in PR #6090
under approved RFC #6091. The CTR2 USB firmware is in progress. First
hardware result, reported by Lynn with a test build of this branch: the
radio's status stream reaches the CTR2 over USB (HELLO, READY and DATA
framing working, radio to CTR2 only). CTR2-to-radio commands, UDP, and
restart/CLOSED handling have not been exercised on hardware yet. Jeremy reports the TCP relay working with a
stock CTR2.

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

Lynn provides the firmware implementation. Binary patching and reverse
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

The TCP relay is the starting point: the controller-facing TCP connection is
replaced by the HID link below, and the dedicated upstream radio connection,
separate from AetherSDR's own command connection, stays.

After the link starts, radio payload bytes are forwarded unchanged in both
directions. The adapter understands the HID link framing only; it does not
inspect or interpret the enclosed radio protocol.

- Forward all controller traffic and all replies and unsolicited output
  received on its dedicated radio connection.
- Preserve byte values and order, including line endings and timing fields.
- Do not parse commands, filter status, renumber sequences, rewrite handles,
  translate CW timestamps, emulate the radio, or manufacture replies.
- Do not route payload through RadioModel command dispatch or an existing
  AetherSDR backend command socket.
- Keep transport errors and diagnostics outside the radio byte stream.

The radio processes the CTR2's commands and returns the results. HID message
boundaries need not match API command boundaries.

The CTR2 remains an independent radio client. Its existing registration and
MultiFlex binding behavior remains its responsibility. The relay does not
assign AetherSDR's client identity or guarantee selection of its GUI session.

## USB link specification (version 0)

### HID interface

Vendor-defined usage page `0xFF00`, usage `0x01`, report ID `0x01`, 8-byte
reports in **both** directions. Lynn's descriptor declares only the Input
(device-to-host) report; the host also needs an Output report to send to
the CTR2. Add the two marked lines before End Collection:

```c
static constexpr uint8_t REPORT_ID = 0x01;
static const uint8_t report_descriptor[] = {
    0x06, 0x00, 0xFF,  // Usage Page (Vendor Defined 0xFF00)
    0x09, 0x01,        // Usage (0x01)
    0xA1, 0x01,        // Collection (Application)
    0x85, REPORT_ID,   //   Report ID (1)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x75, 0x08,        //   Report Size (8 bits)
    0x95, 0x08,        //   Report Count (8 bytes)
    0x09, 0x01,        //   Usage (0x01)
    0x81, 0x02,        //   Input (Data, Var, Abs)    device -> host
    0x09, 0x01,        //   Usage (0x01)              <-- add
    0x91, 0x02,        //   Output (Data, Var, Abs)   <-- add: host -> device
    0xC0               // End Collection
};
```

Use a 1 ms polling interval (`bInterval = 1`) on the interrupt IN and OUT
endpoints. At one 8-byte report per millisecond per direction, payload
throughput is about 7 KB/s each way; the radio's initial status burst after
connecting can take a few seconds to arrive. That is expected, not a fault.

In everything below, a *report* is the 8 bytes after the report ID.

### Messages

Every message is one header report followed by zero or more data reports.

Header report:

| Byte | Field | Value |
| --- | --- | --- |
| 0 | Start of message | `0xFF` |
| 1 | Version | `0x00` |
| 2 | Message counter | `0x00`-`0x7F` |
| 3 | Message type | see below (Lynn's "Future" byte) |
| 4 | Packet count, MSB | number of **reports** in the message, including this header |
| 5 | Packet count, LSB | |
| 6 | Payload byte count, MSB | exact number of payload bytes |
| 7 | Payload byte count, LSB | |

Data reports (packet count - 1 of them):

| Byte | Field |
| --- | --- |
| 0 | Message counter, same as the header |
| 1-7 | Next 7 payload bytes; after the last payload byte, pad with `0x00` |

Rules:

- Packet count = 1 + ceil(payload bytes / 7).
- The payload byte count is exact, so a receiver drops the padding by count
  rather than by stripping zeros. A payload may contain `0x00` or `0xFF`.
- Counters are per direction, 7 bits, and wrap `0x7F` -> `0x00`. A data
  report therefore never starts with `0xFF`.
- Each DATA message carries the previous message's counter + 1.
- A message's reports are sent back to back; messages in one direction are
  never interleaved.
- Maximum DATA payload per message is **512 bytes**; longer data is sent as
  several messages. A DATAGRAM carries up to 1474 bytes (port plus 1472).
  The receiver buffers one whole message before using it, so its buffer
  needs 1474 bytes.
- There is no checksum in version 0. The version byte leaves room to add one.

Message types:

| Type | Name | Direction | Payload | Meaning |
| --- | --- | --- | --- | --- |
| `0x00` | DATA | both | 1-512 bytes | Radio TCP bytes, unchanged |
| `0x01` | HELLO | both | none | Host: "are you there?" (sent on Start, repeated until answered). Device: "restart the link" |
| `0x02` | READY | both | none | Device: "here, ready" (the answer to HELLO). Host: "radio connection is open; DATA may flow" |
| `0x03` | CLOSED | both | none | The radio connection has ended |
| `0x04` | DATAGRAM | both | 3-1474 bytes | One UDP datagram: radio UDP port (2 bytes, MSB first), then 1-1472 datagram bytes |

Every header starts with `0xFF`, whatever it carries; the type byte, not a
second start byte, distinguishes TCP data, UDP datagrams and control
messages. A receiver treats an unknown type as a framing error.

DATA and DATAGRAM share the same counter sequence in each direction. A
DATAGRAM always carries exactly one whole UDP datagram, so its boundaries
are preserved; datagrams over 1472 bytes are not carried.

Control messages (HELLO, READY, CLOSED) are always a single header report
with packet count 1 and payload count 0. A receiver accepts a control
message with any counter and expects the next DATA to carry that counter + 1;
this is how both sides resynchronize. A sender resets its counter to 0 each
time it sends HELLO or READY.

### Link flow

The operator presses Start in AetherSDR; nothing needs pressing on the CTR2.

```text
CTR2                                   AetherSDR                    Radio
  | <-------------------------- HELLO -  |  (Start; repeated ~1 s     |
  |                                      |   until answered)          |
  | -- READY ------------------------->  |                            |
  |                                      | -- new TCP connection ---> |
  | <-------------------------- READY -  |  (connected)               |
  | -- DATA "C1|..." ----------------->  | -- bytes ----------------> |
  | <-------------------------- DATA --  | <-- replies / status ----- |
  |              ...                     |              ...           |
  | <------------------------- CLOSED -  | <-- radio closed --------- |
  | <-------------------------- HELLO -  |  (calling again)           |
```

Treat the host's READY exactly like the Wi-Fi TCP socket connecting and
CLOSED exactly like it disconnecting. Every link gets a **fresh** radio
connection, so the CTR2's normal registration and binding sequence runs
again. Plug order does not matter, and a CTR2 that resets simply answers the
next HELLO.

Host behavior:

- **Start pressed, or no link up:** send HELLO, and repeat it about once a
  second until the device answers. After a failure (the radio connection
  failed, or a link fault below) repeat it every 5 s instead, until a link
  ends cleanly or the operator presses Start again. Never queue a HELLO
  while earlier output to the device is still undelivered, so a device that
  is not reading cannot accumulate them.
- **READY received while no link is up and after a HELLO the host has sent
  since the last link ended** (the answer to that HELLO), **or HELLO received
  at any time** (the device asking for a restart): close any
  existing radio connection, discard all link state, including every report
  still queued for the device, and open a new radio connection (10 s
  timeout). On success send READY and start forwarding, including any radio
  output that arrived first. On failure send CLOSED.
- **Any other READY** (during a link, during the back-off after a failure, or
  while a CLOSED is still undelivered): a late answer to an earlier HELLO;
  ignored. It never discards a pending CLOSED or skips the back-off.
- **DATA received:** forward the payload to the radio.
- **DATAGRAM received:** send the datagram bytes to the radio's IP on the
  given UDP port, from one UDP socket the host opens for this link.
- **UDP datagram from the radio** (to that socket, from the radio's IP):
  forward it whole as a DATAGRAM carrying the radio's source port. UDP never
  delays the TCP stream: if more than about 3 KB of datagrams are already
  waiting on the USB link, newer ones are dropped and counted, as UDP allows.
- **CLOSED received:** forward any DATA that came before it, then close the
  radio connection. No reply is sent. The next HELLO waits until that drain
  has finished.
- **Radio connection closes:** forward the radio's remaining bytes, then send
  CLOSED.
- **Framing error, DATA or DATAGRAM with no radio connection, or a message
  still incomplete 1 s after it began:** close the radio connection, discard
  queued output, send CLOSED, and call the device again with HELLO. A link
  gets at most one CLOSED; DATA the device sends before answering is
  ignored.
- **USB device removed, or Stop pressed:** close the radio connection (and
  send CLOSED if the device is still there).
- **AetherSDR disconnects from the radio or switches radios:** stop the
  relay as for Stop. The relay always targets the radio AetherSDR is
  connected to, and AetherSDR's transmit indicator must keep showing that
  radio while the CTR2 can key it.

Device behavior:

- **At power-up or on entering USB mode:** start in the waiting state
  described next, as if READY had just been sent. The host's next HELLO
  starts the link. A device that may have reset while a link was up can
  instead send HELLO at once, so the host restarts without waiting.
- **On HELLO from the host:** reset the counter and answer READY. Then,
  until the host's READY or CLOSED arrives, ignore every received report
  that is not a header (byte 0 `0xFF`) of type READY, CLOSED or HELLO; reset
  the receiver and process that header. (A further HELLO means the host is
  still calling: answer READY again.) Data reports never start with `0xFF`,
  so this skips the tail of anything sent earlier without misreading it.
- **Host READY:** the radio connection is open, exactly like the Wi-Fi socket
  connecting. Send DATA only after it, and only until CLOSED.
- **Payload is a byte stream.** The host splits radio output at arbitrary
  points, so a DATA message may end mid-line and one line may span several
  messages. Feed received payload into the same parser the Wi-Fi TCP path
  uses. Commands the CTR2 sends may be one per message or batched; either
  works.
- **UDP:** send each datagram for the radio as a DATAGRAM with the radio's
  UDP port, exactly as the Wi-Fi path would send it. Register for the
  radio's UDP by sending a datagram to radio port 4992 this way (the radio
  learns the return address from it) rather than with the TCP
  `client udpport` command: the host cannot see that command, and the
  port it names would be on the PC, possibly AetherSDR's own. Datagrams
  from the radio arrive as DATAGRAM messages, one per datagram.
- On CLOSED, behave as when the Wi-Fi socket drops, then wait: the host
  calls again with HELLO.
- On a framing error in what it receives, or to force a restart, send HELLO
  and resynchronize as above; the host answers with a fresh radio connection
  and READY (or CLOSED).

### Test vectors

Each line is one 8-byte report (after the report ID), in hex.

| Message | Reports |
| --- | --- |
| HELLO, counter 0x00 | `FF 00 00 01 00 01 00 00` |
| READY, counter 0x00 | `FF 00 00 02 00 01 00 00` |
| CLOSED, counter 0x7F | `FF 00 7F 03 00 01 00 00` |
| DATA `C1\|ping\n` (8 bytes), counter 0x01 | `FF 00 01 00 00 03 00 08` / `01 43 31 7C 70 69 6E 67` / `01 0A 00 00 00 00 00 00` |
| DATA `abcdef\n` (exactly 7 bytes), counter 0x02 | `FF 00 02 00 00 02 00 07` / `02 61 62 63 64 65 66 0A` |
| DATA `00 FF 0A`, counter 0x7E | `FF 00 7E 00 00 02 00 03` / `7E 00 FF 0A 00 00 00 00` |
| DATAGRAM to port 4992 (`0x1380`), bytes `01 02 03`, counter 0x03 | `FF 00 03 04 00 02 00 05` / `03 13 80 01 02 03 00 00` |

### Reference implementation for the firmware

`tools/ctr2-firmware-reference/ctr2_link.{h,c}` implements both directions in
portable C99 with no heap: `ctr2_tx_send()` emits one message as reports
through a callback, `ctr2_tx_send_datagram()` emits one UDP datagram, and
`ctr2_rx_feed()` takes one received report at a time and returns a complete
message or an error. Those two files are
**MIT-licensed** so they can go straight into the firmware. AetherSDR's test
suite compiles them, checks them against the vectors above, and
cross-checks them against the application's codec in both directions.

## CW behavior

The CTR2 runs its existing keyer and generates local sidetone. AetherSDR
forwards the resulting bytes exactly as it forwards all other traffic.
It neither runs another paddle keyer nor changes event timestamps or indices.

This is an independent external-client connection, not AetherSDR's own
transmit command path. It does not claim TxCoordinator admission or
ownership for opaque traffic. The relay never generates key-down, retries
commands, or invents a key-up on disconnect. On any link failure the host
closes the CTR2's radio connection, so the radio's own handling of a
disconnected client applies; whether that releases transmit must be verified
on hardware in controlled CW testing. Loss of the connection does not by
itself prove the radio is idle.

## Full transport coverage

The goal remains replacement of all radio traffic used by Wi-Fi mode, not
only CW. The first HID milestone carries the entire TCP byte stream.

UDP between the CTR2 and the radio travels as DATAGRAM messages through one
host UDP socket per link. The radio learns that socket from the CTR2's own
registration datagram, so nothing is negotiated, no TCP command is parsed,
no embedded address is rewritten, and AetherSDR's own UDP sockets are never
involved. Datagram bytes and boundaries are unchanged.

The HID link carries about 7 KB/s each way, so small UDP traffic (meters,
status) fits but panadapter, waterfall or audio streams do not; excess
datagrams toward the CTR2 are dropped and counted rather than delaying TCP.
UDP discovery broadcasts are not relayed: the radio is configured by
address. Verify each UDP-dependent CTR2 feature on hardware before calling
USB a complete replacement for Wi-Fi.

## Host implementation

| Piece | File |
| --- | --- |
| Opaque byte pump, both directions, bounded | `src/core/ByteRelay.{h,cpp}` |
| Link codec (v0) | `src/core/Ctr2HidFraming.{h,cpp}` |
| HID device access on its own I/O thread (hidapi) | `src/core/Ctr2HidPort.{h,cpp}` |
| Link state machine, radio TCP connection and per-link UDP socket | `src/core/Ctr2UsbRelay.{h,cpp}` |
| Applet: Wi-Fi or USB, device list; radio follows AetherSDR's connection | `src/models/Ctr2ProxyModel`, `src/gui/Ctr2ProxyApplet` |

HID I/O runs on one dedicated worker thread because hidapi reads and writes
block; a stalled controller can then never freeze the UI or other
controllers. Nothing waits on that thread: queued output is cancelled
between writes, a restart fences the queue (late acknowledgements from
before the fence are ignored), and shutdown hands the port its final
CLOSED and lets it delete itself once its writes finish. The per-link UDP
socket is bound to the interface that reaches the radio, and only that
radio's datagrams are relayed back. Each direction's relay budget is 16 KiB
in USB mode (256 KiB over Wi-Fi) so a drain fits the link's ~7 KB/s. Everything else runs on the GUI event loop, as the TCP relay
does. hidapi is an existing optional dependency; a build without it shows
USB mode dimmed with the reason. The device list shows every HID interface
on usage page `0xFF00`, usage `0x01`, and the operator picks one explicitly;
the CTR2's USB vendor and product IDs will narrow it once known.

RFC #6091 (approved) covers this design and the independent-client transmit
boundary: the relay runs only after the operator enables it, it relays only
to the radio AetherSDR is connected to, and it stops if that changes, so
AetherSDR's existing transmit indicator always covers on-air visibility. Its amendment for the
HID I/O thread is recorded on the issue. This design does not change
AetherSDR's existing transmit policy or its own radio command paths.

## Firmware status

Lynn has confirmed version 0 as specified here (one start byte, type in
byte 3, exact payload count, no checksum, 512-byte DATA buffering) and is
implementing it, Output report included.

The CTR2 runs on three ESP32-S3 boards, each enumerating with its board's
own USB IDs. AetherSDR names these in its device list and sorts them first;
the operator still picks the device, since `303A:1001` is Espressif's
default for any ESP32-S3 and only the product string tells them apart:

| VID:PID | Product string | CTR2 model |
| --- | --- | --- |
| `303A:1001` (Espressif) | `ESP32S3_DEV` | CTR2-Max, and the CTR2-Nano in development |
| `303A:1001` (Espressif) | `M5STACK_DIAL` | CTR2 units based on the M5Stack M5Dial |
| `2886:0056` (Seeed) | `XIAO_ESP32S3` | CTR2-MIDI (Seeed XIAO) |

Still open: UDP registration through a DATAGRAM to port 4992 rather than
`client udpport`, and, for CW testing, what the CTR2 does if CLOSED arrives
while it is keying.

## Next steps and acceptance

1. Lynn adds USB mode beneath the controller's existing Wi-Fi radio logic
   (in progress).
2. Bench the link with the reference vectors and AetherSDR's USB mode.
   Done, as reported by Lynn: startup and the radio's status stream
   arriving at the CTR2. Still to do: controls (CTR2 to radio), display
   updates, MultiFlex binding, UDP, unplug and replug, and a CTR2 restart
   mid-session. Verify AetherSDR's existing RX operation remains intact.
3. With the operator's transmit authorization and test setup, verify paddle
   keying, local sidetone, latency under load, and what the radio does when
   the link drops mid-transmission.
4. Test each UDP-dependent CTR2 feature over USB before declaring full
   Wi-Fi/USB parity. Repeat on the intended VPN/tailnet route
   and record the host platforms actually verified.

No packet capture or API-parser implementation is a prerequisite for the
opaque relay. The inputs to this design are Jeremy's requirements, his
report of the working TCP relay, and Lynn's HID proposal. Host
implementation remains independent of proprietary firmware disassembly.
