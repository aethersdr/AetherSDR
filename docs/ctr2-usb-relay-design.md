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
  send CLOSED if the device is still there). If Start follows at once, that
  CLOSED reaches the device before the new link's first HELLO.
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
  UDP port, exactly as the Wi-Fi path would send it. Datagrams from the
  radio arrive as DATAGRAM messages, one per datagram. On a LAN the radio
  sends a client's UDP to the TCP peer's address, which here is the host,
  at the port the client names with `client udpport <port>`. It does not
  learn an endpoint from a registration datagram: `client udp_register`
  sent to port 4992 got no UDP back on a FLEX-8600 (firmware 4.2.18). The
  port to name is the host's own UDP socket for the link, which a device
  learns by negotiating the RELAY_UDP_PORT extension (below). Then send
  `client udpport <port>`, and a one-byte datagram to radio port 4992,
  which covers firmware that rejects `udpport`, as AetherSDR's own LAN
  connection does. Without that extension a device cannot register for UDP
  over USB.
- On CLOSED, behave as when the Wi-Fi socket drops, then wait: the host
  calls again with HELLO.
- On a framing error in what it receives, or to force a restart, send HELLO
  and resynchronize as above; the host answers with a fresh radio connection
  and READY (or CLOSED).

### Extensions (negotiated per device)

Version 0 above is complete on its own, and a CTR2 never needs anything in
this section. Controllers that want more than the relayed radio stream can
offer link extensions. AetherSDR uses them only with a device that offers
them, so new extensions never need a change to the CTR2 or to version 0.

**Discovery.** Right after opening the device, before the first report is
read, the host sends GET_REPORT for HID Feature report `0x02`. The device
answers with 8 bytes after the report ID:

```text
['C'] ['X'] [extension version 0x01] [0x00] [capabilities, 32 bits MSB first]
```

The report is exactly 8 bytes after the report ID (9 with it). Capability
bit *n* stands for extension message type `0x40 + n`. A device
without this Feature report (every CTR2) fails the request. So does a reply
that is not exactly this shape. Either way the device offers nothing and the
link is plain version 0.

**Acceptance.** When the offer shares bits with what the host supports, the
host sends SET_REPORT for Feature report `0x02` in the same layout. It
carries only the shared bits: the extensions the host will use. Those bits,
and no others, are allowed on the link in either direction. They stay in
force until the device leaves the bus (unplugged, reset or re-enumerated),
across any number of HELLO/READY link restarts. If the SET fails, the host
uses no extensions.

**Messages.** Extension types `0x40`–`0x5F` use the version 0 header and
data reports. Each type has its own maximum length (the table below); a
longer header is a framing error, and so is any type with no defined
length, even when its bit was negotiated. They are sequenced like DATA and
DATAGRAM and share their counter: one counter, one receiver state machine. A receiver treats a type that was not accepted exactly
like any other unknown type: a framing error. The host sends extension
messages only while the link is relaying (after its READY). It drops them
rather than queue them when about 130 queued reports (~130 ms; one meter
datagram plus headroom) are already waiting, so they never delay the radio
stream. Their reports carry no TCP cost, so the backlog statistic still
counts radio bytes only. Each AUDIO_SPECTRUM frame of 32 bars is 7 reports
(header + 6 data), so 20 frames a second use about 14% of the link.

| Type | Bit | Direction | Payload (max length) |
| --- | --- | --- | --- |
| `0x40` AUDIO_SPECTRUM | 0 | host → device | (69 bytes) `[bars N, 1..64][low Hz hi][low Hz lo][span Hz hi][span Hz lo][N levels]`. Receiving: what the operator hears (the Client EQ RX tap: after the radio's DSP, client NR and the RX chain up to and including the EQ; not stages after the EQ, output conversion or volume) over the active slice's passband width. Transmitting: the transmit audio (the TX EQ tap) over the TX filter width. The span is that width rounded up to a clean step (500 Hz–20 kHz, capped at the audio's Nyquist frequency: 12 kHz for 24 kHz audio). With low > 0 the bars are log-spaced from low to span Hz, bar *i* covering low·(span/low)^(i/N) to low·(span/low)^((i+1)/N); AetherSDR sends low = span/60 within 20–100 Hz. With low = 0 they split 0..span evenly. Each level is 0..255, linear in dB from −90 dB (0) to 0 dBFS (255). About 20 a second. |
| `0x41` RELAY_UDP_PORT | 1 | host → device | (2 bytes) `[port hi][port lo]`: the host's UDP socket for this link, sent once right after the host's READY. The device registers for UDP with `client udpport <port>` (and a one-byte datagram to radio port 4992). |
| `0x42` AUDIO_SPECTRUM_LINEAR | 2 | host → device | (69 bytes) The same payload as AUDIO_SPECTRUM, `[bars N][low Hz hi][low Hz lo][span Hz hi][span Hz lo][N levels]`, but the bars always split low..span evenly, bar *i* covering low + (span − low)·i/N to low + (span − low)·(i+1)/N, whatever low is. A device that negotiates it gets it in place of AUDIO_SPECTRUM for windows that do not start at 0. AetherSDR sends it while receiving CW: a window centered on the CW pitch covering the passband (pitch + filter low to pitch + filter high) with half its width to spare on each side, kept within 0 Hz and Nyquist, so a controller can show how far a signal is from zero beat. Requires AUDIO_SPECTRUM. |
| `0x43` CW_TEXT | 3 | host → device | (2–33 bytes) `[confidence 0..3][1..32 bytes of text]`: CW decoded by AetherSDR's CW decoder from the active slice (while it runs: the slice in CW and RX decoding on), as it arrives, about a character at a time with word gaps as spaces. Filtered by the CW panel's sensitivity threshold; newlines become spaces and only printable ASCII is sent. Confidence 0 is the most certain (decoder cost < 0.15), then 1 (< 0.35), 2 (< 0.60) and 3, the bands the CW panel colors. |
| `0x44` FIRMWARE | 4 | both | (1–517 bytes) `[op][...]`: replaces the device's application over the link. Never negotiated by the relay; only AetherSDR's firmware updater uses it, on a session of its own. See *Firmware updates* below. |
| (none) WIDE_REPORTS | 5 | both | No message: type `0x45` is never sent. Bundles the link's reports in HID report `0x04`, up to seven to a USB transfer, about 49 KB/s each way. See *Wide reports* below. |

**Wide reports.** A device that offers bit 5 declares a second report beside
report `0x01`: report ID `0x04`, 63 bytes, Input and Output. Once the host
accepts the bit, both directions stop using report `0x01` and carry the same
8-byte reports bundled in report `0x04`:

```text
[count 1..7][count reports of 8 bytes, back to back][zero padding to 63 bytes]
```

Nothing else changes: the messages, counters and rules above apply report by
report, so a receiver unpacks a bundle and feeds its reports in order. A
bundle claiming 0 or more than 7 reports, or a report `0x04` on a link that
did not accept the bit, is a framing error. Seven reports a millisecond carry
49 payload bytes, against 7 with report `0x01` alone. The device drops back
to report `0x01` whenever the host reads Feature report `0x02` (each new host
session does, before anything else), until the Set that follows accepts the
bit again, so a session that does not take wide reports never receives
them. AetherSDR's relay and firmware updater both accept it.

Declaring report `0x04` changes what Windows does for the whole interface:
it pads every report read from the device to the longest one, 64 bytes with
the report ID, and pads every report written to it the same way. A host
therefore takes report `0x01` from any read of 9 bytes or more, and a device
takes it from any write of 8 bytes or more after the ID. AetherSDR releases
before this change accept only exactly 9 bytes, so on Windows they cannot
relay a device that declares report `0x04`: ship such firmware only once an
AetherSDR release that reads padded reports has been out for a while.

The reference implementation in `tools/ctr2-firmware-reference` covers the
Feature report layout (`ctr2_caps_encode` / `ctr2_caps_decode`), sending
(`ctr2_tx_send_extension`) and the accepted set
(`ctr2_rx_set_extensions`). AetherSDR's tests cross-check it against the
application in both directions.

### Firmware updates

A device that can replace its own application offers FIRMWARE (`0x44`), and
reports its version in HID Feature report `0x03`. Neither is part of
relaying: the relay never accepts FIRMWARE, and a device without them is
updated some other way.

**Version report.** Feature report `0x03`, Get only, exactly 24 bytes after
the report ID, readable at any time without a link:

```text
['F'] ['W'] [layout 0x01] [major] [minor] [patch] [flags] [0x00]
[build ID: 16 bytes of printable ASCII, NUL-padded]
```

Flags: `0x01` PENDING, this image was just installed and has not completed a
link yet, so the device drops it on its next reset; `0x02` SIGNED, FIRMWARE
accepts only images signed for this device. AetherSDR reads the report
whenever it opens the device, and shows the version in the CTR2 Proxy
applet.

**Session.** The relay must not hold the device. The host accepts only
FIRMWARE in Feature report `0x02`, sends HELLO and waits for READY as usual,
then, in place of its own READY, resets its counter and sends FIRMWARE
BEGIN. No radio connection opens. The device takes that header's counter as
the start of the sequence (`ctr2_rx_start` in the reference) and sends only
FIRMWARE messages until the session ends with CLOSED, HELLO or its own
restart. Integers are big-endian:

| Op | Direction | Payload | Meaning |
| --- | --- | --- | --- |
| `0x01` BEGIN | host → device | `[size u32][SHA-256, 32 bytes]` (37 bytes) | an image of `size` bytes follows |
| `0x02` DATA | host → device | `[offset u32][1..512 bytes]` | the next bytes of the image, in order |
| `0x03` END | host → device | none | verify the image and its signature, select it, restart into it |
| `0x04` ABORT | host → device | none | drop the partial image |
| `0x05` SWITCH | host → device | none | restart into the device's other firmware (its maker's, kept beside this one) |
| `0x06` BOOTLOADER | host → device | none | restart into the chip's ROM download mode, for an install there |
| `0x81` STATUS | device → host | `[op][status][value u32]` (7 bytes) | the answer to each host message; for DATA, value is the bytes stored so far |

Status: `0` OK, `1` BAD_STATE (no session, a second BEGIN, or no other
firmware for SWITCH), `2` BAD_OFFSET, `3` TOO_LARGE, `4` FLASH_ERROR, `5`
REJECTED (not a valid image signed for this device), `6` HASH_MISMATCH, `7`
BAD_MESSAGE.

AetherSDR keeps four DATA messages in flight. At the link's rate of about
7 KB/s (one 8-byte report per millisecond) a 1 MB image takes about two and
a half minutes; the device erases its flash as the bytes arrive, so the link
never waits for a whole-slot erase.

**Keeping the new image.** After END answers OK the device leaves the bus
and restarts on the new image, which reports PENDING. The host opens it
again, checks the version is the one it sent, and completes a link (HELLO,
READY, then CLOSED). Only then does the device keep the image; it clears
PENDING, and the host reads the report once more to confirm. A device that
resets before that, because the new image crashed or never reached the host,
returns to its previous image. A device that refuses the image at END keeps
its firmware, and the host ends the session with ABORT and CLOSED.

**Two firmwares on one device.** A device may keep its maker's firmware
beside the one AetherSDR updates, in a slot an update never writes, so the
maker's firmware and its settings survive every update. SWITCH asks the
device to restart into that firmware; how a device selects its image is its
own business.

A device answers SWITCH and BOOTLOADER with STATUS before it leaves the bus.
AetherSDR also counts the request done if the device leaves while the answer
is outstanding, once the port has written the request, so a restart that beats
its answer, or an answer naming the wrong op, is not reported as a lost
device. A port that fails before writing it (a write error) still reports the
device lost.

**Host implementation.** `Ctr2FirmwareUpdater` runs the session and checks
the image before sending anything: an ESP32-S3 application image with an
application description (`readImage`), not a merged flash image or a
bootloader. `Ctr2ProxyModel` stops the relay for the update and starts it
again afterwards. `tests/ctr2_firmware_updater_test.cpp` holds it to a
simulated device that runs the reference for its side of the link.

### Installing AetherCTR2 on a CTR2-Max

The CTR2 window (**Settings → Controllers → CTR2...**, `FirmwareInstallDialog`)
holds the relay's controls (`Ctr2ProxyPanel`, over `Ctr2ProxyModel`) and, below
them, the CTR2-Max firmware installer (over `FirmwareInstallModel`, which owns
the catalog download, the device watch and the install job; the window
includes no core headers). It installs AetherCTR2 beside the CTR2-Max's stock
firmware, from releases listed in AetherSDR's firmware catalog
(`EspFirmwareCatalog`, `https://www.aethersdr.com/firmware/catalog.json`).

- **Package.** Each release is one file, `PRODUCT-VERSION.asdr`: an
  uncompressed ustar archive of plain files in one folder, holding
  `manifest.json` (`EspFirmwareManifest`), `manifest.json.sig` and the images
  the manifest names. The catalog's `package` links to it. AetherSDR refuses
  anything else in the archive (links, folders, paths, hidden or repeated
  names) and a damaged or cut-short one.
- **Trust.** The manifest carries an Ed25519 signature (`manifest.json.sig`)
  by a firmware-release key built into AetherSDR, checked before anything
  else in the package is used, and the manifest's SHA-256s cover every
  image. There is no
  way to install an image of one's own. The signature is the whole trust
  chain: the catalog is unsigned and only says where releases are, so a
  wrong catalog can at most offer a different validly signed release. Older
  releases are listed on purpose, so an operator can go back to one; the
  version an operator picks is the one installed (the signed manifest must
  name it), and an update over the cable names the version the unit runs
  before it starts.
- **Testing a publisher's builds.** A build configured with
  `-DAETHER_FIRMWARE_DEV_OVERRIDES=ON` (off by default, never for release)
  honors `AETHER_FIRMWARE_CATALOG` (another catalog URL) and
  `AETHER_FIRMWARE_TRUST_KEYS` (extra keys, hex), and logs each override it
  uses. A release build has neither.
- **Downloads** are capped as they arrive: the catalog at 256 KiB and a
  package at 17 MiB; inside a package, the manifest at 64 KiB, the signature
  at 1 KiB and each image at the size its signed manifest gives.
- **Layout.** The manifest lists the images and where they go, the regions
  never written (the stock firmware, its settings and its registration), the
  partition-table rows that must be on the unit and stay, and the app slots
  the boot selection (otadata) chooses among. `EspFirmwareInstaller` refuses
  any write over a protected region, at parse time and again when writing,
  counting whole erased sectors.
- **Registration.** AetherCTR2 is installed only on a unit its stock firmware
  has registered: `Reg.txt` in the stock firmware's LittleFS volume
  (`Ctr2MaxRegistration`, read-only littlefs). The installer checks it before
  writing anything, and AetherCTR2 checks it again at every start. This
  honors the agreement with Lynovation by checking that the stock firmware
  registered the unit. It looks for `Reg.txt` (the firmware also reads the
  call sign in it), never for a registration key, and is not itself a
  copy-protection mechanism.
- **Versions.** The unit reports its version as three bytes, read from the
  front of its `esp_app_desc` version string (`%u.%u.%u`). A release's
  version must therefore start with `major.minor.patch`, each 0–255; a suffix
  (`0.9.3-rc1`) is allowed and ignored by the check. `Ctr2FirmwareUpdater`
  refuses an image whose version does not, and compares the unit's report
  against the same three numbers.
- **The device confirmed.** An action is taken on the device the operator
  confirmed: the window captures it when the action begins, the model refuses
  the action if another device is there by the time it is confirmed, and the
  job compares the MAC of the device it opened before writing anything (a
  port name can be reused). A device whose MAC cannot be read is refused.
- **A plan that holds together.** Besides the protected regions, no two
  writes may share a sector (images, and the boot selection), and no required
  region may lie under the boot selection, which every install and start
  rewrites. Parsing checks this, and the installer checks it again before the
  first write (`EspFirmwareManifest::checkPlan`).
- **Boot selection.** The boot selection (otadata) has two entries, one per
  sector. Selecting a slot rewrites only the entry not in use, as its own
  sector, so an interrupted write always leaves the entry the unit started
  from.
- **Two paths.** A unit running AetherCTR2 is updated over the link
  (Firmware, above). One running its stock firmware, or held in ROM download
  mode, is installed through the chip's ROM bootloader (`EspFlasher`, over
  Espressif's esp-serial-flasher): optionally a whole-flash backup first,
  then the images and the boot selection. In download mode the window also
  starts AetherCTR2 (rewriting only the boot selection), and backs up or
  restores the whole flash; a backup restores only onto the unit it came
  from.

### The CTR2-Max's own firmware

The CTR2 window also installs the CTR2-Max's stock firmware, Lynovation's
(the **Lynovation** tab). AetherSDR never hosts it.

**From a file.** The operator downloads Lynovation's release (a zip holding
one merged flash image, or the .bin itself) and opens it.
`Ctr2MaxStockFirmware` takes it only if:

- the application image is an intact ESP32-S3 image (its appended SHA-256
  holds);
- it carries the CTR2-Max firmware's markers.

The install writes that application into app0 and selects it. The unit's own
partition table lays the install out (`Ctr2MaxStockFirmware::plan`); the
unit's nvs, otadata, app0 and spiffs rows must match the file's, and
everything else is protected. The bootloader, the table, the settings files
and registration (spiffs) and AetherCTR2 are never written. Each version
opened is kept in AetherSDR's data folder, so it can be installed again.

**From Lynovation's list.** Lynovation may publish its releases for
AetherSDR to fetch (`Ctr2MaxStockCatalog`):

- `catalog.json` at `https://ctr2.lynovation.com/firmware/catalog.json`,
  format 1, lists each release's .bin, the same file as the download, by
  URL, size and SHA-256, newest first:

  ```json
  { "format": 1,
    "products": [ { "name": "CTR2-Max firmware", "device": "CTR2-Max", "chip": "ESP32-S3",
                    "releases": [ { "version": "v1.02.00n",
                                    "file": "https://ctr2.lynovation.com/.../CTR2-Max_v10200n.bin",
                                    "size": 1622720,
                                    "sha256": "<64 hex digits>" } ] } ] }
  ```

- `catalog.json.sig` beside it is the Ed25519 signature of the catalog's
  exact bytes, in base64, made with Lynovation's own key:

  ```sh
  openssl pkeyutl -sign -rawin -inkey lynovation-release.pem -in catalog.json \
      | base64 -w0 > catalog.json.sig
  ```

AetherSDR reads only a catalog signed by a Lynovation key built into it
(`kBuiltInKeys` in `Ctr2MaxStockCatalog.cpp`; none yet), and fetches nothing
without one. A downloaded .bin must be the size and SHA-256 the catalog
gives, and the image's own version must be the one the catalog names. It is
downloaded when the operator presses Install on it, then installed, and kept,
as a file opened by hand would be.

A build configured with `AETHER_FIRMWARE_DEV_OVERRIDES` (never a release)
also reads two settings from the environment, so Lynovation can test its own
catalog before publishing it:

- `AETHER_LYNOVATION_CATALOG`: another catalog URL (a `file://` URL works);
- `AETHER_LYNOVATION_TRUST_KEYS`: public keys to trust, as hex, separated by
  commas.

**Restarting without the buttons.** The stock firmware may take two FIRMWARE
operations on the link, as AetherCTR2 does: SWITCH (`0x05`) into AetherCTR2,
and BOOTLOADER (`0x06`) into download mode. It offers them like any device:

- **Offer:** bit 4 of Feature report `0x02` (`'C' 'X' 01 00 00 00 00 10`).
- **Session:** HELLO/READY, then the FIRMWARE message, answered with STATUS.
- **Not needed:** it needs neither the version report nor the image
  transfer.

AetherSDR reads the offer when it reads the device's version
(`Ctr2ProxyModel::usbFirmwareCommands()`). Where it is made, **Restart into**
and every install from the stock firmware restart the unit on request, with
no BOOT/RESET. Without it, the window asks the operator for the buttons.

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
| Wide reports: bundling in report `0x04`, padded report `0x01` reads | `src/core/Ctr2HidThreadPort.cpp` |
| Link codec (v0) | `src/core/Ctr2HidFraming.{h,cpp}` |
| HID device access on its own I/O thread (hidapi) | `src/core/Ctr2HidPort.{h,cpp}` |
| Link state machine, radio TCP connection and per-link UDP socket | `src/core/Ctr2UsbRelay.{h,cpp}` |
| CTR2 window: Wi-Fi or USB, device list; radio follows AetherSDR's connection | `src/models/Ctr2ProxyModel`, `src/gui/Ctr2ProxyPanel` |
| Extension negotiation (Feature report `0x02`), on the HID I/O thread | `src/core/Ctr2HidThreadPort.cpp`, `src/core/Ctr2HidapiPort.cpp` |
| AUDIO_SPECTRUM producer: the Client EQ RX tap (post-EQ), or the TX EQ tap while transmitting, its own 2048-point analyzer (reset on each RX/TX switch), 32 bars at 20 Hz, floor-level bars while the tap in use is not being written, only while a negotiating device is relaying | `src/gui/Ctr2AudioSpectrumFeeder` |

HID I/O runs on one dedicated worker thread because hidapi reads and writes
block; a stalled controller can then never freeze the UI or other
controllers. Nothing waits on that thread: queued output is cancelled
between writes, a restart fences the queue (late acknowledgements from
before the fence are ignored), and shutdown hands the port its final
CLOSED and lets it delete itself once its writes finish. A port reopened
on the same device waits on its own thread, up to 1 s, until the previous
port has closed it, so the old CLOSED cannot overtake the new link. The per-link UDP
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
own USB IDs. AetherSDR names these in its device list, sorts them first and
preselects the first one (USB is the applet's default mode). The operator
can still pick another device, since `303A:1001` is Espressif's
default for any ESP32-S3 and only the product string tells them apart:

| VID:PID | Product string | CTR2 model |
| --- | --- | --- |
| `303A:1001` (Espressif) | `ESP32S3_DEV` | CTR2-Max, and the CTR2-Nano in development |
| `303A:1001` (Espressif) | `M5STACK_DIAL` | CTR2 units based on the M5Stack M5Dial |
| `2886:0056` (Seeed) | `XIAO_ESP32S3` | CTR2-MIDI (Seeed XIAO) |
| `303A:1001` (Espressif) | `AETHER_KNOB` | AetherKnob (Elecrow CrowPanel 2.1" rotary display) |
| `303A:1001` (Espressif) | `Aether*` | Other controller firmware on the same link, named by its product string (case-sensitive prefix plus at least one character; printable ASCII, at most 32 characters) |

UDP registration is settled on hardware: the radio ignores a registration
datagram on a LAN, so a device registers with `client udpport` naming the
host's socket, learned through the RELAY_UDP_PORT extension (see "UDP" under
Device behavior). Still open for CW testing: what the CTR2 does if CLOSED
arrives while it is keying.

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
