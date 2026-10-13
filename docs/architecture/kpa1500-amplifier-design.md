# Elecraft KPA1500 Amplifier Support — Design Note

**Status:** Draft for maintainer review (#4097). Not a new `IRadioBackend`
family under the aetherd RFC — the KPA1500 has no FlexRadio awareness at all,
so this adds a **peripheral accessory** in the same sense as the existing 4O3A
PGXL/TGXL/Antenna Genius integrations, the ACOM S-series amplifier
(`acom-600s-amplifier-design.md`), the SPE Expert
(`spe-expert-amplifier-design.md`) and the VK3AMP
(`vkamp-amplifier-design.md`). AGENTS.md's touchpoint taxonomy explicitly
exempts a standalone accessory's transport (`peripheral(...)`) from the
`IRadioBackend`-design-doc requirement that gates a *new radio family*.

**Scope of the implementation this note accompanies:** a dedicated
`Kpa1500Applet` driven by `Kpa1500Connection` over TCP, plus a Peripherals
settings row. Telemetry (forward/reflected power, SWR, temperature, band,
fault), operate/standby, internal-ATU tune (start, progress, cancel) and
in-line/bypass, and antenna select between ANT1 and ANT2. **Network keying (`^TX`/`^RX`) is implemented in the protocol and
connection layers but is not reachable from the UI or the engine** — see §6,
which is the part of this note the maintainer actually has to rule on.

---

## 1. Why this is a peripheral, not a backend

The KPA1500 is a standalone Ethernet-controlled RF amplifier with **zero radio
awareness**: no CAT link to AetherSDR, no shared session, and — unlike the
PGXL — no `amplifierChanged(AmpDelta)`-style relay riding through the
FlexRadio's own `amplifier` status object. The Flex 8600 does not discover it,
does not know its IP, and cannot proxy a single byte of its telemetry. It sits
downstream of the radio in the RF chain and is addressed directly.

So, same as `AcomConnection`/`SpeConnection`/`VkampConnection`,
`Kpa1500Connection` lives directly under `src/core/`, outside the radio seam,
and never touches `IRadioBackend`/`invokeExtension`. Presence is
user-configured: an IP in the Peripherals row is the only thing that can ever
reveal this applet, exactly as for ACOM/SPE/VK3AMP.

One consequence worth stating explicitly, because an earlier triage pass on
#4097 got it wrong: there is **no new "presence model" to invent** here. The
standalone-amp pattern already exists three times over in this tree. This is a
fourth instance of it, not a deviation from it.

**Protocol authority (Principle I) and clean-room compliance (Principle IV):**
the wire protocol comes from Elecraft's own published *KPA1500 Programming
Reference V3*, a public manufacturer document. That is one of Principle IV's
explicitly clean inputs, and it puts this integration in a materially better
position than VK3AMP's (reverse-engineered) or even ACOM's. Nothing here is
decompiled, disassembled, or paraphrased from a proprietary binary.

**What is and is not confirmed.** Command spellings and reply encodings follow
that reference, and the protocol test asserts its literal examples. Nothing has
yet been run against a physical unit; §7 lists the few points the reference
itself leaves open.

---

## 2. Relationship to #4953 (KPA500 + KAT500)

#4953 requests the same *shape* from the same *vendor*, but it is not the same
protocol and not the same box count:

| | #4097 (this note) | #4953 |
|---|---|---|
| Hardware | KPA1500 — amp **with internal ATU** | KPA500 amp **+** KAT500 tuner, two boxes |
| Transport | Ethernet, TCP **and** UDP on port 1500 (`^CP` configurable) | RS-232/USB serial; network only via an external ser2net-style proxy |
| Command set | `^`-prefixed, `;`-terminated | KPA500 / KAT500 references — *different* command sets, one per box |
| Network keying | `^TX`/`^RX`/`^TQ` (firmware 3.07) | none — hardware KEY IN only |

**Do not build a single `ElecraftConnection` that speaks both.** What the two
issues genuinely share is the peripheral scaffolding (settings row, applet
registration, auto-reconnect fan-out), this family design note, and the
TX-interlock decision in §6 — which applies to both. The protocol classes stay
separate.

---

## 3. Wire protocol

### 3.1 Framing

One ASCII shape for every message, in both directions:

```
'^' <CMD> [<args>] ';'
```

`<CMD>` is two upper-case letters, or three for the handful of commands the
reference names that way (`^PWF`, `^PWR`, `^STS`, …). A bare `^CMD;` is a **query**; the
same token with an argument is a **set**, and the amp answers a query by
echoing the command with its current value. That symmetry is why
`Kpa1500::MessageParser` handles the whole stream with one decoder — there is
no separate reply framing.

The amp **does not broadcast spontaneously**. Every reading AetherSDR displays
arrives because `Kpa1500Connection` asked for it (§4).

### 3.2 Transport

A TCP server on port 1500 accepts the command set; a UDP server on the same
port accepts the identical set. The port is movable with `^CP`, so the
Peripherals row exposes it as an editable spin box rather than a fixed label.

**This implementation is TCP-only, on purpose.** Every control path here is a
command whose delivery matters — OPERATE, tune, antenna select, and above all
the keying refresh of §6 — and UDP provides no delivery signal at all. UDP
would be a reasonable *addition* for pure telemetry later; it is not a
reasonable substitute for the control channel.

### 3.3 Command set used

| Capability | Command(s) |
|---|---|
| Forward / reflected power | `^PWF`, `^PWR` |
| SWR | `^SW` |
| Temperature | `^TM` |
| Band data | `^BN` |
| Fault, and clearing it | `^FL` (two hex digits), `^FLC;` |
| Operate / standby | `^OS` |
| ATU mode / in-line | `^AM` (`I`/`B`), `^AI` |
| ATU tune start / progress / cancel | `^FT;`, `^TP`, `^FE;` |
| Antenna select | `^AN` |
| Network keying | `^TX`, `^RX`, `^TQ` |

Encodings that matter for the readouts, all from the reference: `^SW` is SWR in
tenths (`^SW123;` is 12.3:1); `^PWF`/`^PWR` are whole watts; `^FL` answers two
hex digits (`B0`, `C1`, …); `^AM` answers `^AMI;` or `^AMB;`; `^TQ` answers 0
(not keyed) or 1–3 (keyed by `^TX`, with or without KEY IN). The amp has two
antenna connectors, ANT1 and ANT2; firmware 3.00 adds antenna numbers 3–32
behind an external switch, which `^AN` reports but this integration never
selects.

A full-search tune "needs continuous exciter RF power to complete" (`^FE`). The
applet starts and cancels it and shows it running from `^TP`; supplying the RF
is the operator's, with the radio's own TUNE. Nothing here keys the radio.

### 3.4 Boundary validation (Principle VII)

These bytes arrive on a LAN socket, so `MessageParser` treats malformed input
as expected input, not an error path:

- bytes before a `^` are discarded, so connecting mid-stream resynchronizes
  rather than poisoning every later frame;
- **a second `^` before the current frame's `;` resynchronizes onto the later
  one.** This is not theoretical tidiness — without it, a truncated fragment
  immediately followed by a genuine frame causes the genuine frame to be
  swallowed, and the unit test for exactly that case failed before the guard
  was added;
- the command is matched by name — two letters, or one of the reference's
  three-letter commands (`^PWF`, `^STS`, …) — because payloads can start with a
  letter: a leading run of letters would read `^FLB0;` as a command `FLB`. A
  frame that does not start with a command is dropped, never forwarded;
- payloads are length-capped (`kMaxArgChars`) and the accumulation buffer is
  capped (`kMaxBufferBytes`), so a peer that opens a `^` and never sends `;`
  cannot grow memory without bound, and one `feed()` is linear in its input
  however the bytes are arranged;
- `applyMessage()` range-checks every decoded value and leaves the snapshot
  **untouched** on anything malformed. A bad frame cannot flip OPERATE, drive a
  gauge negative, or select an antenna that does not exist.

---

## 4. Polling model

Because the amp never pushes, `Kpa1500Connection` drives a 500 ms poll split in
two:

- **fast set, every tick:** `^PWF`, `^PWR`, `^SW`, `^TM`, `^TP` — the values
  that move while the operator is transmitting or tuning (plus `^TQ` while
  keyed, §6.2);
- **slow set, one per tick round-robin:** `^FL`, `^OS`, `^BN`, `^AN`, `^AM`,
  `^AI` — configuration readbacks that change only when someone changes them. A
  full cycle is 3 s, which keeps the wire quiet without making the panel feel
  stale.

`Status` holds every field as a `std::optional`. A field stays unset until its
own reply lands, so the applet renders `—` rather than a confident `0` for
something the amp has not actually reported. That distinction matters most at
connect time and after a drop.

**Principle II applies to peripherals too.** Nothing in the applet latches an
optimistic value from a button click: pressing OPERATE sends `^OS1;` and the
label moves only when the amp's own `^OS` readback confirms it.

---

## 5. UI surface

A dedicated `Kpa1500Applet`, not a reuse of `AmpApplet`, for two concrete
reasons:

1. `AmpApplet`'s `● RADIO` / `● DIRECT` source badge describes the PGXL's
   radio-relayed telemetry path. It has no meaning for a device the Flex radio
   cannot see at all, and a badge that reads `DIRECT` because there is no
   alternative is noise, not information.
2. The internal ATU and the antenna switch need somewhere to live that is not
   bolted onto the PGXL panel.

Layout follows the `AcomApplet`/`VkampApplet` convention: PWR/REF/SWR gauges, a
temp/band/ATU info row, a fault banner shown only when a fault stands, and a
control row (OPERATE, TUNE, ATU IN/BYP) plus antenna buttons for ANT1 and ANT2,
with an `ANT n` label when an external switch has the amp on antenna number
3–32. TUNE lights while `^TP` reports a tune running, and pressing it then
cancels. Readouts are throttled through the shared 10 Hz label timer rather than
repainting on every reply.

Fault codes are shown as the amp's own two-digit hex code (`Fault B0`), matching
the reference's `^FL` table and the amplifier's front panel.

---

## 6. Network keying — the open decision

This is the part of #4097 that is genuinely hard, and it is the reason this
note exists before the feature is complete.

### 6.1 The hazard

`^TX;` with **no argument** keys the amplifier and leaves it keyed until an
explicit `^RX;`, with no fail-safe if the controlling application crashes or
the LAN drops. That recreates precisely the "radio keyed into an amp that
cannot be told to stop" failure mode #4097 asks to eliminate.

The vendor reference documents the correct pattern: `^TX1;` through `^TX99;`
takes a 1–99 s timeout, and *should connection to the control software be lost,
the amplifier will turn off `^TX` when the timeout expires.*

### 6.2 What is implemented

`Kpa1500::buildKey()` is structurally incapable of emitting the bare form.
There is no input — negative, zero, or enormous — that produces `^TX;`; the
timeout is clamped into `[1, 99]` rather than rejected, because a *refused* key
command is its own hazard (a silent no-key). The protocol test asserts this
over the whole range `[-1000, 1000]`, and treats an empty frame as a failure
too.

`Kpa1500Connection::key()` sends `^TX10;` and refreshes it every 3 s while
held; `unkey()` sends `^RX;` unconditionally rather than gating on a remembered
flag. If the link drops while keyed, the connection logs that the amp's own
timeout is now the only thing that will release it, and stops pretending to
manage a key it can no longer refresh. While keyed, every poll also sends
`^TQ;`, and the amp's answer is authoritative: a `^TQ0;` stops the refresh.

### 6.3 What is NOT implemented, and why

**Nothing calls `key()`.** There is no UI control, no engine hook, and no
transmit-path wiring. Constitution Principle VI requires that any code path
which can transmit fails closed when the operator's intent is not unambiguous;
with the decision below unresolved, the fail-closed state is the shipped state.

Two things need a maintainer ruling before that changes:

1. **Is Ethernet keying trusted as the sole keying path?** T/R sequencing
   requires the amp to be keyed *before* RF reaches it, and PTT-over-Ethernet
   inserts LAN latency into that path — a hot-switching hazard if RF leads the
   key. Worth noting that KEY IN and `^TX` run in **parallel**, not
   either/or, so "network keying additive, hardware line stays" is available as
   a low-risk first milestone that does not require pulling the PTT cable.

2. **How does the amp-liveness TX interlock hook the transmit guard?** #4097
   asks that transmit be inhibited if the amp connection is lost. AetherSDR has
   no such interlock today: TX safety is gated on `RadioCapabilities::
   canTransmit` in the engine guard, not on external-device liveness. None of
   the existing standalone amps gate transmit on their own state —
   `AudioEngine.cpp` and `FlexBackend.cpp` contain no reference to `Acom`,
   `Spe` or `Vkamp` at all. So this is **net-new engine-side work**, not a
   pattern to copy, and per EB3 the hook must be a GUI-free `core/` interface
   (the `IConnectionAutomation` shape), never a `gui/` include from the engine.

The same ruling governs #4953, which is why §2 asks that the two issues not be
designed in isolation.

---

## 7. Hardware-validation checklist

The reference settles the wire encodings (§3.3). What it leaves open, and what
only a physical unit can confirm:

| # | Open point | Where | Failure if wrong |
|---|---|---|---|
| 1 | The bypassed reply to `^AI;` is printed as `^AT0;` (p.13), almost certainly a typo for `^AI0;` | `applyMessage` | ATU IN/BYP never shows bypassed |
| 2 | `^TP` reports a full-search tune started by `^FT;` as running until it completes or `^FE;` cancels it | TUNE button state | TUNE does not light, so it cannot cancel from the app |
| 3 | `^AN` answers antenna numbers 3–32 as plain digits when an external switch is in use | `applyMessage` | The `ANT n` label stays hidden |

---

## 8. Files

| File | Role |
|---|---|
| `src/core/Kpa1500Protocol.h/.cpp` | Framing parser, `Status` decode, command builders. No Qt GUI, no sockets — unit-testable in isolation. |
| `src/core/Kpa1500Connection.h/.cpp` | TCP transport, reconnect, poll loop, keying refresh. `peripheral(kpa1500)`. |
| `src/gui/Kpa1500Applet.h/.cpp` | The panel. |
| `tests/kpa1500_protocol_test.cpp` | Framing/boundary tests, decode and command spellings against the reference's examples, and the keying-builder safety invariant. |
| `src/gui/RadioSetupDialog_Peripherals.cpp` | Peripherals device (`PeripheralSettings` device `"Kpa1500"`). |
| `src/gui/MainWindow_Wiring.cpp` | Signal wiring and startup auto-connect. |

Settings live under `PeripheralSettings` device `"Kpa1500"`, fields `ManualIp`
and `ManualPort` — the nested per-feature object Principle V requires, not new
flat keys.
