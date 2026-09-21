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
fault), operate/standby, internal-ATU tune and in-line/bypass, and antenna
select. **Network keying (`^TX`/`^RX`) is implemented in the protocol and
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

**What is and is not confirmed.** The *command spellings* below are quoted
from that reference. Several *reply payload encodings* — field widths and
numeric scaling — are not yet confirmed against a physical unit. §7 is the
checklist for that, and the code is deliberately structured so each unconfirmed
assumption is a single named constant rather than a scattered convention.

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

`<CMD>` is two or three upper-case letters. A bare `^CMD;` is a **query**; the
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
| Fault / status | `^FL`, `^SF` |
| Operate / standby | `^OS`, `^OP` |
| Power on / off | `^ON` |
| ATU mode / in-line | `^AM`, `^AI` |
| ATU tune start / cancel | `^FT`, `^FE` |
| ATU current setting | `^DA` |
| Antenna select / enable | `^AN`, `^AE` |
| Network keying | `^TX`, `^RX`, `^TQ` |
| Control port | `^CP` |

`^SF`, `^OP` and `^DA` are documented above for completeness but are not
polled by the current implementation — `^FL` already carries the fault code the
applet displays, and `^DA`'s L/C readout has no UI to land in yet.

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
- a command token that is not 2–3 letters `A–Z` is dropped, never forwarded;
- payloads are length-capped (`kMaxArgChars`) and the accumulation buffer is
  capped (`kMaxBufferBytes`), so a peer that opens a `^` and never sends `;`
  cannot grow memory without bound;
- `applyMessage()` range-checks every decoded value and leaves the snapshot
  **untouched** on anything malformed. A bad frame cannot flip OPERATE, drive a
  gauge negative, or select an antenna that does not exist.

---

## 4. Polling model

Because the amp never pushes, `Kpa1500Connection` drives a 500 ms poll split in
two:

- **fast set, every tick:** `^PWF`, `^PWR`, `^SW`, `^TM` — the values that move
  while the operator is transmitting;
- **slow set, one per tick round-robin:** `^FL`, `^OS`, `^ON`, `^BN`, `^AN`,
  `^AE`, `^AM`, `^AI` — configuration readbacks that change only when someone
  changes them. A full cycle is ~4 s, which keeps the wire quiet without making
  the panel feel stale.

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
control row (OPERATE, TUNE, ATU IN/BYP) plus antenna 1–3. Readouts are throttled
through the shared 10 Hz label timer rather than repainting on every reply.

Fault codes are shown as **raw numbers**, not names. The code-to-name table is
not something this integration has confirmed, and a confidently wrong fault
*name* is worse than an honest number.

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
manage a key it can no longer refresh. `^TQ` readback is authoritative: if the
amp says it is not keyed, this class does not get to keep claiming it is.

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

Nothing below is a blocker for reviewing the structure; all of it is a blocker
for trusting the readouts. Each item maps to a single named constant or table.

| # | Assumption | Where | Failure if wrong |
|---|---|---|---|
| 1 | `^PWF`/`^PWR` payloads are whole watts | `kPowerDivisor` | Power reads 100× high/low |
| 2 | `^SW` payload is SWR in hundredths | `kSwrDivisor` | SWR reads 100× off |
| 3 | `^BN` uses the Elecraft `BN` band-code table | `bandName()` | Band label wrong (cosmetic; unknown codes already render `—`) |
| 4 | `^AM` values are 0=bypass, 1=auto, 2=manual | `AtuMode` | ATU mode label wrong; `^AM` set commands wrong mode |
| 5 | Antenna ports are 1–3 | `kMinAntenna`/`kMaxAntenna` | A 4th port would be rejected client-side |
| 6 | `^FL0;` clears a fault | `buildClearFault()` | Clear-fault button no-ops |
| 7 | `^TQ` answers `0`/`1` | `applyMessage` | Key-state readback ignored (fails safe — `^TX` timeout still applies) |

A payload that arrives with an explicit decimal point (e.g. `1.5`) is taken at
face value rather than re-scaled, so items 1–2 degrade gracefully if the
firmware ever switches encodings.

---

## 8. Files

| File | Role |
|---|---|
| `src/core/Kpa1500Protocol.h/.cpp` | Framing parser, `Status` decode, command builders. No Qt GUI, no sockets — unit-testable in isolation. |
| `src/core/Kpa1500Connection.h/.cpp` | TCP transport, reconnect, poll loop, keying refresh. `peripheral(kpa1500)`. |
| `src/gui/Kpa1500Applet.h/.cpp` | The panel. |
| `tests/kpa1500_protocol_test.cpp` | Framing/boundary tests and the keying-builder safety invariant. |
| `src/gui/RadioSetupDialog.cpp` | Peripherals row (`PeripheralSettings` device `"Kpa1500"`). |
| `src/gui/MainWindow_Wiring.cpp` | Signal wiring and startup auto-connect. |

Settings live under `PeripheralSettings` device `"Kpa1500"`, fields `ManualIp`
and `ManualPort` — the nested per-feature object Principle V requires, not new
flat keys.
