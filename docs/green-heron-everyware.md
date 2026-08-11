# Green Heron Everyware antenna switch (the GHE applet)

The **GHE** tile selects the antenna on a Green Heron *Everyware* remote
antenna switch. It is a peripheral accessory: it speaks the device's own TCP
transport, knows nothing about any radio family, and works with whatever radio
is connected, or none.

## What is on the far end

Port 10000 is the Everyware **server** — a service running on a PC with the
switch hardware attached to it over serial — not a switch. One server presents
several switches (four, named `AS-84F-1`…`AS-84F-4`, on the reference
installation), each with the same set of antenna ports.

Switch names are only unique **within one server**. An installation with two
servers can have `AS-84F-1` on both, so anything driving more than one must key
on host + switch name rather than switch name alone.

## What the applet shows

A radio is wired to exactly **one** switch, so the applet asks which one and
then shows only that switch's antennas:

| field | |
|---|---|
| **IP** | address of the Everyware server |
| **Port** | its TCP port; 10000 unless the installation moved it |
| **Switch** | which switch this radio is fed from — filled from the device's roster once connected, and remembered |

Below that is one row per antenna port:

| row state | meaning |
|---|---|
| `Beam-20 · ON` (lit) | what the **device** reports this switch is on |
| `Beam-15 · in use by AS-84F-3` (greyed) | held by another switch; an antenna feeds one switch at a time, and the device would refuse the click |
| `EFHW-40` | free — click to select |

The reference Python client (`motoham88/everyware-linux`) draws all four
switches as a matrix. This applet deliberately does not: one column of real
antenna names fits a 260 px sidebar tile, where a 4 × 9 grid only fits by
abbreviating everything into initials. The other switches are not ignored —
their selections are exactly what marks an antenna "in use" here.

**Nothing is ever inferred from a command we sent.** `selectPort()` transmits
and returns; the device is the sole authority on where the relays are and
republishes within ~123 ms. A relay that fails to move shows up as a button
that does not light, rather than as a UI that lies.

When the link drops, the last known state stays on screen flagged *stale* and
the rows go insensitive — blanking the tile on every blip loses more than it
protects, and a click while stale would reach nothing.

## Where the code lives

| | |
|---|---|
| `src/core/GreenHeronProtocol.{h,cpp}` | framing, record parsing, `SET_SWITCH` encoding — **pure**, no sockets |
| `src/models/GreenHeronModel.{h,cpp}` | the socket, keepalive, reconnect backoff, and the state they produce |
| `src/gui/GreenHeronApplet.{h,cpp}` | the tile; owns its own model |
| `tests/green_heron_protocol_test.cpp` | verbatim wire fixtures in, records out |
| `tests/green_heron_model_test.cpp` | the socket path against a stand-in server on loopback |
| `tests/green_heron_applet_test.cpp` | the tile, offscreen |

The protocol TU is pure so the parser can be tested against bytes captured off
the device with no hardware and no network. The applet owns its model rather
than being handed one by `MainWindow` (the AntennaGenius pattern) because the
Everyware server has **no discovery path** — there is nothing for `MainWindow`
to detect, and the tray button therefore cannot be hardware-conditional the way
`AG` / `SS` are: the operator has to be able to open the tile in order to type
the address into it.

Settings live in the shared `Peripherals` object under a `GreenHeron`
sub-object (Constitution Principle V), alongside ACOM and SPE Expert. No
credentials are involved — the protocol has no authentication of any kind.

## The protocol

ASCII, line-oriented:

| | | |
|---|---|---|
| `US` | `0x1f` | between fields |
| `GS` | `0x1d` | between subfields within a field |
| `CRLF` | `0x0d 0x0a` | ends a record |

The device transmits immediately on connect — no client hello, no login, no
banner.

**Record boundaries do not align with TCP segments.** Observed, not assumed:
one 585-byte segment carried three whole `SWITCHADD` records; the recurring
109-byte segment carries `SWITCHLOCKS` + `SWITCHUPDATE` + `SWITCHLOCKS`,
starting mid-cycle. Every read goes through `splitRecords()`, and the test
suite splits a record at every possible byte offset, including between the CR
and the LF.

### Device → client

```
SWITCHADD    ␟1␟AS-84F-1␟Beam-10␝0␝0␝false␟…9 ports…
SWITCHUPDATE ␟AS-84F-1␟OFF␟0␟-27
SWITCHLOCKS  ␟AS-84F-1␟OFF␟OFF␟OFF␟OFF
```

On connect the device sends one `SWITCHADD` per switch, then streams
`SWITCHUPDATE` + `SWITCHLOCKS` per switch, round-robin, every ~0.5–3.4 s
indefinitely. Note `Dummy Load` contains a space — split on `US`, never on
whitespace.

### Client → device

```
SET_SWITCH␟AS-84F-4␟Beam-20\r\n       29 bytes
SET_SWITCH␟AS-84F-4␟Dummy Load\r\n    32 bytes
SET_SWITCH␟AS-84F-4␟OFF\r\n           25 bytes
```

Fire-and-forget: no ack, no correlation id. `green_heron_protocol_test`
asserts these bytes and these lengths, so the encoder cannot drift from what
was captured.

### Keepalive

A single **NUL (`0x00`) every 5.000 s** — measured across 38 consecutive
keepalives, intervals 4.9987–5.0007 s.

It matters. When the host slept, the server dropped the idle connection and the
vendor's own client, unaware, retransmitted into a half-open socket for over
ten minutes with a growing send queue. That is why the model reconnects with
backoff (1 s doubling to 30 s) and bounds its own connect attempts rather than
trusting `ESTABLISHED` to mean anything.

### `SWITCHLOCKS` — the trap

Slot *N* carries the port selected by the *N*th switch **in announcement
order**, republished to every switch so a UI can grey out antennas in use
elsewhere.

Announcement order on the reference hardware is `AS-84F-1, AS-84F-3, AS-84F-2,
AS-84F-4` — **not** sorted:

| slot | 0 | 1 | 2 | 3 |
|---|---|---|---|---|
| switch | AS-84F-1 | AS-84F-**3** | AS-84F-**2** | AS-84F-4 |

Verified with a case that discriminates between the two candidate orderings:
`SET_SWITCH␟AS-84F-3␟Beam-15` produced `SWITCHLOCKS|AS-84F-1|OFF|Beam-15|OFF|OFF`
— slot 1, and AS-84F-3 is announced second. Under sorted order it would be
slot 2.

> This was first "confirmed" as *sorted* order using `AS-84F-4`, which landed
> in slot 3 — and proves nothing, because AS-84F-4 is index 3 under **both**
> orderings. Switches 2 and 3 are the only discriminating cases.

So `GreenHeronModel` keeps two orderings and they are not interchangeable:
`announcedOrder()` is arrival order, append-only across reconnects, and is the
index basis for lock slots; `displayOrder()` is sorted and is only for
presentation. Conflating them mislabels which switch holds an antenna, with no
crash and nothing logged. `green_heron_model_test` pins the discriminating
case.

Locks propagate one switch per round-robin step, so the full set takes a few
seconds to converge. Lock state briefly appearing on some switches and not
others is the device's cadence, not a bug.

### `SWITCHUPDATE` field 4

The Green Heron **wireless link signal** — `-27` on switches 1–2 and `-28` on
3–4, stable across every sample including while relays were switching, and
unrelated to antenna selection. It is parsed and carried but not displayed. No
unit is asserted anywhere: the values look like they could be dBm, but that
scale was never established from the wire, so labelling it would be a guess
presented as fact.

### Still unestablished

Parsed into named fields and carried through, but nothing branches on them:

- the `SWITCHADD` leading `1`, and the per-port `0␝0␝false` subfields —
  identical on all four switches and unchanged by any operation performed.
- whether a UDP discovery/announce path exists alongside the TCP port.

Three verbs are handled. That is not provably the whole vocabulary — the
captures behind it cover one installation with one kind of switch attached — so
`parse()` returns `Unknown` for anything else and the model logs it rather than
treating it as an error.

## Input validation (Constitution Principle VII)

Nothing arriving on this socket is trusted to be well-formed, and the device is
reachable over the LAN or a tunnel:

- the pending buffer is capped (64 KiB); a peer that never sends CRLF has its
  buffer dropped rather than growing it without bound
- single records above 8 KiB are dropped, and the records around them still
  arrive
- fields per record and ports per `SWITCHADD` are capped
- **outbound** names are checked too: switch and port names reach us from the
  device's own roster, so `encodeSelect()` refuses any name containing `US`,
  `GS`, `CR` or `LF` rather than echoing it back as extra fields or an extra
  record
- a connect that never completes is bounded by a 10 s timer — `QTcpSocket` has
  no timeout of its own, and a dropped SYN is precisely the half-open shape
  this device's idle-drop behaviour produces

## Provenance (Constitution Principle IV)

There is no vendor documentation for this protocol. Everything above was
determined by **observing traffic on the wire** between the vendor's own client
and the vendor's own server — packet captures plus a hexdump probe run against
a live installation — which Principle IV names explicitly as a clean input. No
binary was decompiled, disassembled, or read for strings, and nothing here is
transcribed from such output.

Ported from the author's own prior MIT-licensed implementation,
[`motoham88/everyware-linux`](https://github.com/motoham88/everyware-linux).
That project's other two front ends — a curses TUI and an MQTT / Home Assistant
bridge — are deliberately **not** part of this port; AetherSDR has its own MQTT
applet, and the TUI has no analogue here.

AetherSDR is not affiliated with or endorsed by Green Heron Engineering.
