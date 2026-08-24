# Green Heron Everyware antenna switch and rotator (the GHE applet)

The **GHE** tile selects the antenna on a Green Heron *Everyware* remote
antenna switch, and turns a rotator attached to the same station. It is a
peripheral accessory: it speaks the device's own TCP transport, knows nothing
about any radio family, and works with whatever radio is connected, or none.

Both halves arrive on **one** connection, which is why they are one tile and
one model rather than two of each.

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

### The rotator row

Under the antennas, and **only while the device is actually reporting a
heading**:

```
Rotor            62.9° · asked 64.3° · Δ1.4°
[ 64.3        ]  [ Turn ]
```

Three things about it are load-bearing rather than stylistic.

**Choosing a heading and sending it are separate gestures.** Typing in the
field proposes a heading; `Turn` (or Enter in the field) transmits. There is no
stop, park, or disable verb anywhere in this protocol — powering the controller
off produced no protocol record at all, because it is a physical action — so a
rotation that starts cannot be recalled in software. Nothing here transmits on
a single click or a drag, and it must stay that way.

**The readout is the reported heading, never the commanded one, and it never
says "on target".** Measured on an RT-21: a commanded 64.3 settled at a mean
62.9 over 23 samples, having overshot to 65.5 on the way; the return leg
commanded 52.3 settled at 54.55. Both stopped about two degrees short *along
their direction of travel*, which is opposite signs in absolute terms — so
`reported − commanded` has no consistent sign and correcting for a fixed offset
would double the error one way. At rest, with nothing commanded and nobody
touching the controller, the reported heading wandered over a **±3.8° band**
(60 s sample: mean 65.54, range 61.2–68.8). Any arrival threshold at that noise
level would flicker, and the error it tested would be smaller than the
measurement. Hence "asked X · ΔY" and no verdict.

**Re-sending the same heading is not a correction strategy.** Once the rotator
is inside its controller's deadband the same command does nothing — the
controller reads the same noisy sensor and already believes it has arrived. A
loop of command-measure-command does not converge; it sits at three or four
degrees of error issuing commands the rotator ignores. Nudge by eye instead.

The row disappears when the rotator stops reporting — see *Presence is reported
only by silence* below.

**The compass rose is drawn only when the tile is floating.** The reference GTK
client draws a full Cairo rose in its main window. In the docked rail — 248 px
of usable width — it is still refused: a recognisable rose would cost most of
the tile's height to restate one number the readout already gives. Popped out,
the operator has sized the window themselves, and the dial goes under the Turn
row:

```
Rotor                             57.4°
[ degrees      ]          [ Turn ]
              ╭───N───╮
              │   ╎   │      ╎  asked
              │   ↑   │      ↑  reported
              W   ●   E
              ╰───S───╯
```

It is a second view of the readout, not a second source for it: the needle is
the **reported** heading and the dashed tick, when this session has commanded
one, is the **asked** heading. There is no arrival state, no "on target"
colour, and no tolerance ring, for the reasons measured above. Neither mark
uses an accent colour — success and warning tokens would editorialise about a
difference the protocol gives no way to judge — so the needle is the primary
text colour and the ghost the label colour, both resolved through
`ThemeManager::color()` rather than as hardcoded hex.

**The rose is read-only, and that is the first invariant above rather than an
omission.** A compass rose's natural affordance is click-to-point, which is
exactly the single gesture that may not transmit. `RotorCompass` has no mouse
handlers at all and must not acquire one — not even to fill the heading field,
which is one Enter away from a rotation that cannot be recalled.

The compass lives *inside* the rotator section, so it inherits that section's
gate: it appears with the first `POINT` and goes away with the silence timeout,
rather than being left pointing at a stale heading after the controller is
switched off. The floating window resizes itself around it — measured, 346 px
to 538 px the moment the section appeared — so no default float size is
declared for the tile.

## Where the code lives

| | |
|---|---|
| `src/core/GreenHeronProtocol.{h,cpp}` | framing, record parsing, `SET_SWITCH` and `TURN` encoding — **pure**, no sockets |
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
ADD          ␟Rotor
POINT        ␟Rotor␟50.4␟0␟0
```

On connect the device sends one `SWITCHADD` per switch, then streams
`SWITCHUPDATE` + `SWITCHLOCKS` per switch, round-robin, every ~0.5–3.4 s
indefinitely. Note `Dummy Load` contains a space — split on `US`, never on
whitespace.

**It replays unprompted on a RECONNECT too, not only on the first connect.**
The model depends on this: `isReady()` keeps the retained panel disabled and
refuses `SET_SWITCH` until a record arrives on the new socket, so a device that
only spoke once would leave the tile dead forever. Measured against the
reference hardware — connect, read, close, immediately reconnect: first bytes
at **+105 ms** on connection 1 and **+78 ms** on connection 2, both starting
with `SWITCHADD`, 195 B each, then 16 further records in the next 6 s. The gate
therefore opens about a tenth of a second after the link returns.

### Client → device

```
SET_SWITCH␟AS-84F-4␟Beam-20\r\n       29 bytes
SET_SWITCH␟AS-84F-4␟Dummy Load\r\n    32 bytes
SET_SWITCH␟AS-84F-4␟OFF\r\n           25 bytes
TURN␟Rotor␟89.0\r                     16 bytes   ← bare CR
```

Fire-and-forget: no ack, no correlation id. `green_heron_protocol_test`
asserts these bytes and these lengths, so the encoder cannot drift from what
was captured.

**`TURN` ends with a bare `CR` (`0x0d`), not `CRLF`.** The whole 16-byte
command, captured:

```
54 55 52 4e 1f 52 6f 74 6f 72 1f 38 39 2e 30 0d
T  U  R  N  US R  o  t  o  r  US 8  9  .  0  CR
```

That is a genuine asymmetry with `SET_SWITCH`, and assuming the two matched
would produce a command the device may not act on. It is confirmed twice over:
observed on the vendor client's wire, and then confirmed **accepted** by this
client's own `TURN` moving a real RT-21. Do not "tidy" it without a capture.

The heading is formatted with one decimal place and **locale-independently** —
a comma-decimal locale would otherwise put `89,0` on a wire that only ever
carries `89.0`, and only on the machines of operators not running an English
locale. `green_heron_protocol_test` sets a German locale and asserts the bytes.

Only these two verbs are ever sent. Three operator actions across one session —
turn to 89, turn back, power the rotator off — produced exactly three records,
all `TURN`; powering the controller off produced none.

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

### The rotator — `ADD`, `POINT`, and presence

`POINT` is pushed about every **0.96 s** (26 records over 23.9 s, measured
through this port's own parser against the live server) while the rotator's
controller is on. `ADD` arrives once, on connect — **at +0.08 s, about a second
before the first `POINT` at +1.00 s**.

That ordering is load-bearing and it is easy to get wrong in a way loopback
tests do not catch: a handler that writes `announced` onto an existing rotator
entry writes it onto nothing, every time, because there is no entry yet. The
model keeps the announced names in a list of their own and `POINT` consults it
when it creates the rotator. `green_heron_model_test` sends the two records in
that order and asserts the flag survives.

`ADD` is handled as a **generic device announcement, not a rotator one**. Every
capture of it reads `ADD␟Rotor`, and that field is the device's
*operator-configured* name — the same way switch records key on the switch
name. That the name happened to be the word "Rotor" is not evidence the verb is
rotor-specific: one installation, one device, one generic three-character verb
on a socket shared with the switches. A rotator named `Beam` would announce
`ADD␟Beam`. **`POINT` is what establishes that a named device is a rotator with
a heading**, so the model keys its rotator table on `POINT` and treats `ADD` as
corroboration.

**Presence is reported only by silence, and it is dynamic mid-session.** With
the controller powered down, a fresh connection receives no `ADD` and no
`POINT` at all while switch records continue normally. Switching it off during
a session is *not a socket event*: `ADD` and `POINT` simply stop while
`SWITCHUPDATE` and `SWITCHLOCKS` carry on down the same connection, and nothing
on the wire ever contradicts the last heading. A client without its own
timeout therefore inherits a phantom — a heading on screen for an antenna
nobody is driving, beside a control that would aim a controller that is off.

`GreenHeronModel` treats **10 s** without a `POINT`
(`kRotorSilentAfterMs`, about ten missed reports at the measured cadence) as
absence: the rotator leaves `rotorNames()`, `turnTo()` refuses it, and the tile
hides the row. Because nothing arrives when a rotator goes quiet, this cannot
be signal-driven — the model polls its own state once a second, and only while
at least one rotator has ever reported.

Rotator state is also **dropped for every socket the model opens**, which is
the opposite of what the switch roster does. The switch panel degrades
honestly — a grey grid nobody can click — but a retained heading beside a live
`Turn` button is the same phantom, and clearing it is what makes `turnTo()`'s
liveness check a statement about *this* connection.

Which host has the rotator was settled with a discriminating test rather than
assumed. Two Everyware servers were listened to read-only for 25 s each, back
to back:

| | `SWITCHADD` | `SWITCHUPDATE` | `SWITCHLOCKS` | `ADD` | `POINT` |
|---|---|---|---|---|---|
| `…​.99` | 4 | 28 | 28 | **0** | **0** |
| `…​.100` | 4 | 26 | 26 | **1** | **25** |

`.99` streams switch records perfectly while emitting no rotor record at all —
which is exactly the configuration that once produced the conclusion that
"rotators are not on this protocol". One host could never have distinguished
the hypotheses.

Re-run against both hosts through **this** implementation, 25 s each,
read-only:

| | `SWITCHADD` | `SWITCHUPDATE` | `SWITCHLOCKS` | `ADD` | `POINT` | `Unknown` |
|---|---|---|---|---|---|---|
| `…​.99` | 4 | 34 | 34 | **0** | **0** | **0** |
| `…​.100` | 4 | 28 | 28 | **1** | **26** | **0** |

Every byte both servers sent parses into a named record — no `Unknown` on
either. On `.99` the tile draws no rotator row at all, which is the absence
path working on hardware rather than in a stand-in.

### End to end from this build, on the RT-21

The first `TURN` this port has sent, through `GreenHeronModel::turnTo()` to the
live controller. **It moved the rotator, so the bare CR is confirmed accepted by
the device rather than merely observed being sent.**

```
resting 51.8   TURN -> 60.0   wire: 54 55 52 4e 1f 52 6f 74 6f 72 1f 36 30 2e 30 0d
   0.9 - 2.0   51.8 - 52.3    nothing moves for two seconds
   3.0         56.0           moving
   6.2         59.3           near the target
  10 - 45      57.4 - 59.8    settled: mean 57.74 over 45 samples

then    TURN -> 51.8   (the resting heading it started from)
  46 - 105     55.6 - 57.4    settled: mean 57.35 over 60 samples
```

Outbound behaved as every earlier capture predicted: motion began about two
seconds after the command, and it finished **2.3° short along its direction of
travel**.

**The return leg stalled.** Commanded 51.8 from 57.4, it moved about half a
degree and stopped; the command was neither refused locally nor followed by
motion. That is a second independent observation of the stall recorded above —
the session that needed three `TURN`s to get most of the way back. It is
hardware behaviour and not a protocol or client fault: the same encoder, the
same socket and the same code path had moved it correctly one command earlier.
The rotator was left ~5.6° clockwise of where it started, and no correction
loop was attempted, for the reason recorded above: re-commanding argues with a
sensor the controller also reads and does not converge.

This is exactly why the tile shows `asked X · ΔY` and refuses to say
"on target". Through the whole of the above the readout tracked the **reported**
heading and never the commanded one, and `POINT` field 3 held `0` throughout —
a fourth independent session confirming it does not mean "in motion".

### Heading resolution — tenths the device does not have

`POINT` reports one decimal place and **does not resolve it**. 120 consecutive
records from a stationary rotator (118.4 s, nothing transmitted) gave mean
52.01, sd 0.80, range 49.0–52.7, on just **eight distinct values** lying on a
~0.47° grid. Treat the heading as half-degree data wearing a tenths costume;
nothing should assume 0.1° means anything.

The operator sees the same wandering float in the vendor's own Windows client,
which rules out this client's parsing, display and sampling as causes — it is
upstream of the protocol.

### Still unestablished

Parsed into named fields and carried through, but nothing branches on them:

- the `SWITCHADD` leading `1`, and the per-port `0␝0␝false` subfields —
  identical on all four switches and unchanged by any operation performed.
- whether a UDP discovery/announce path exists alongside the TCP port.
- **`POINT` field 3.** It held `0` for the whole of three separate confirmed
  rotations, in different sessions and at different headings, which **rules
  out** "in motion" — and then read `5` exactly once, as a controller was being
  powered down. That resembles the RT-21's documented status byte, but it is in
  direct tension with the field staying `0` during a move, where the same enum
  predicts `1`. One sample, one value, no discriminating test. Carried; nothing
  branches on it.
- **`POINT` field 4.** Never anything but `0`.

Five verbs are handled. That is not provably the whole vocabulary — the
captures behind it cover two installations — so `parse()` returns `Unknown` for
anything else and the model logs it rather than treating it as an error. (Both
servers did, on the 25 s runs above, produce **zero** `Unknown` records.)

## Input validation (Constitution Principle VII)

Nothing arriving on this socket is trusted to be well-formed, and the device is
reachable over the LAN or a tunnel:

- the pending buffer is capped (64 KiB); a peer that never sends CRLF has its
  buffer dropped rather than growing it without bound
- single records above 8 KiB are dropped, and the records around them still
  arrive
- fields per record and ports per `SWITCHADD` are capped
- **outbound** names are checked too: switch, port and rotator names reach us
  from the device's own roster and announcements, so `encodeSelect()` and
  `encodeTurn()` refuse any name containing `US`, `GS`, `CR` or `LF` rather
  than echoing it back as extra fields or an extra record
- `encodeTurn()` also refuses any heading that is not finite and inside
  0–360°. Deliberately strict: overlap-capable rotators may well accept
  headings past 360, but none was ever observed doing so, there is no stop
  command, and a guess here aims a real antenna
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

**Code provenance**, which is a separate question from the protocol provenance
above: the implementation is a from-scratch Qt port of the author's own prior
MIT-licensed
[`motoham88/everyware-linux`](https://github.com/motoham88/everyware-linux),
contributed to this repository under its GPLv3 by the same copyright holder —
the switch half and the rotator half alike. Nothing is vendored: the Python was
read for facts about the wire and the Qt written from scratch, no file here is
a copy of that project, and there is therefore no `THIRD_PARTY_LICENSES` entry
for it. That project's other two front ends — a curses TUI and an MQTT / Home
Assistant bridge — are deliberately **not** part of this port; AetherSDR has
its own MQTT applet, and the TUI has no analogue here. Its Cairo compass widget
is not ported either, for the reasons in *The rotator row* above.

The rotator measurements quoted throughout — the settle-short behaviour, the
±3.8° rest band, the ~0.47° quantisation, the deadband — were taken on an
RT-21 through that project, and this port's own `TURN` was confirmed to move
the same hardware.

AetherSDR is not affiliated with or endorsed by Green Heron Engineering.
