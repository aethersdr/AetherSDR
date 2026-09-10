# TelePost LP-100A Wattmeter Support — Design Note

**Status:** Draft for maintainer review. Not a new `IRadioBackend` family — the
LP-100A has no FlexRadio awareness at all, so this adds a **peripheral
accessory** in the same sense as the existing ACOM, SPE, VK3AMP and 4O3A
PGXL/TGXL integrations, which AGENTS.md's touchpoint taxonomy explicitly
exempts from the `IRadioBackend`-design-doc requirement gating a *new radio
family*.

**Scope:** A dedicated `LpMeterApplet` driven by `LpMeterConnection`, a
peripheral transport (serial or ser2net-style raw TCP, identical records either
way), plus a Peripherals settings row. **Read-only in v1** — the meter's three
write commands are deferred, see §4. Tracks issue #5315, raised from a
community request.

---

## 1. Why this is a peripheral, not a backend

`IRadioBackend`'s canonical path exists for devices the *radio* relays. The
LP-100A is a standalone RS-232 instrument the radio has never heard of, and it
works with any radio or none. So, like `AcomConnection`/`SpeConnection`/
`VkampConnection`/`PgxlConnection`, `LpMeterConnection` lives directly under
`src/core/`, outside the radio seam, and never touches `IRadioBackend`.

It is also **not** an extension of `CrossNeedleMeterApplet`. That applet is fed
by the radio's own forward/reflected telemetry; this is an independent
instrument measuring at its own coupler. The ACOM design note §2 records what
happens when a peripheral is folded into an existing device's model for
code-reuse's sake — PGXL's applet appeared whenever an ACOM connected — and the
lesson generalises.

---

## 2. What gets added

```
LpMeterProtocol (src/core/LpMeterProtocol.h/.cpp)   -- pure, Qt6::Core only
  Reading            decoded record
  ResponseParser     streaming, resyncs on ';'
  looksLikeRecord()  the entire integrity mechanism (no checksum exists)
  PollGate           shared-transport arbitration (§6)
  RangeTracker       gauge-scale hysteresis (§5)

LpMeterConnection (src/core/LpMeterConnection.h/.cpp)
  QIODevice* -- QSerialPort (local port) or QTcpSocket (raw-TCP proxy),
  chosen at connect time. One parser serves both.
  signals: connected/disconnected/connectionFailed, readingUpdated,
           gaugeCeilingChanged, dataFlowingChanged

LpMeterApplet (src/gui/LpMeterApplet.h/.cpp)
  Two HGauge rows (Power, SWR), a six-cell info grid, a three-state status
  pill, and a context menu for the per-range full scale.

AppletPanel      "LP100" in the Metering category, markHardwareConditional,
                 absent from kDefaultOrder
RadioSetupDialog Peripherals row 8
```

---

## 3. Protocol summary

**Primary source:** TelePost's own *LP-100A Operations Manual*, pp. 20–21.
**Where the manual and the wire disagree, the wire wins** — every field width
below was measured, and §8 lists what the manual gets wrong.

115200 8N1, no handshake. Four single-byte host commands, and **every one
except `P` is an increment with no absolute-set form and no acknowledgement**:

| Cmd | Effect |
|---|---|
| `P` | Poll for one reading |
| `A` | Increment SWR alarm set point |
| `M` | Increment mode / power range |
| `F` | Cycle Peak / Avg / Tune |

The meter **never pushes**. It answers `P` and nothing else — the single
largest structural difference from the ACOM, which streams once enabled.

### Record layout — measured, 2026-08-29

```
;0000.00,046.3,088.5,0,NF0T  ,2,1,-2.3,1.00
```

`;` **leads** a record and is not a terminator; there is no terminator at all.
Body is exactly **42 characters**, every field fixed width.

| # | Field | Width | Offset¹ | Notes |
|---|---|---|---|---|
| 0 | Power | 7 | 2 | W, zero-padded |
| 1 | Z | 5 | 10 | ohms, zero-padded, **magnitude only** |
| 2 | Phase | 5 | 16 | degrees, zero-padded, **magnitude only** (§7) |
| 3 | Alarm set point | 1 | 22 | 0=Off 1=1.5 2=2.0 3=2.5 4=3.0 5=User |
| 4 | Callsign | 6 | 24 | space-padded |
| 5 | Power range | 1 | 31 | 0=High 1=Mid 2=Low — **ceiling not transmitted** (§5) |
| 6 | Peak-hold | 1 | 33 | 0=Avg 1=Peak 2=Fast |
| 7 | dBm | 4 | 35 | **signed, NOT zero-padded** |
| 8 | SWR | 4 | 40 | |

¹ 1-based, including the leading `;`.

**There is no checksum.** `looksLikeRecord()` is the only thing between a
corrupted record and a gauge, so it validates in four layers: field count,
separator positions (at the canonical length), **per-field maximum width**,
**strict lexical form**, and physical range. The reference implementation
consulted (§9) checks length alone, which accepts any single flipped digit.

The width and lexical layers were added after review of #5320 found the first
three insufficient, and the case that made it clear is worth recording because
it defeats the obvious defence. `QString::toDouble()` accepts **exponent
notation**, and `01e+100` is *exactly seven characters* — the canonical Power
width — so it passed the separator-offset check as well. It decoded to 1e100 W,
propagated into `RangeTracker`'s ceiling, reached the applet as a `float`
infinity, and `evenTicks()`'s `static_cast<int>` on it is undefined behaviour
(`inf * 0.0f` is NaN, so even the first tick was UB). Reproduced before fixing.

So: **offset checking is not a value check**, and the fix is per-field lexical
form (digits, at most one decimal point, sign only on dBm, no exponent) plus a
width cap. The width cap doubles as the value bound — *n* characters cannot
express a magnitude of 10ⁿ — which is why the upper limits in
`LpMeterProtocol.h` are read off the measured wire format rather than invented.
`evenTicks()` also rejects a non-finite scale on its own account, because a GUI
helper should not depend on its caller having validated.

This is Constitution VII (untrusted input validated at the boundary), and the
boundary is `looksLikeRecord()`. **Do not weaken it without replacing it** —
`RangeTracker::kCeilingExpandRecords` is 2 rather than something larger
*because* this function carries the weight (§5).

### The fields are internally consistent, and that is load-bearing

|Z| and phase reproduce the SWR the meter separately reports, to a **mean
absolute error of 0.0038** across 218 captured records under drive. That
redundancy validated the whole field interpretation against physics rather than
against another document — and it is what makes §7's coherence detection
possible.

---

## 4. Command scope for v1

| Tier | In v1? | Rationale |
|---|---|---|
| Poll + full decode | Yes | Read-only, no risk. |
| Alarm / range / peak-avg cycling (`A`/`M`/`F`) | **No** | Deferred deliberately. |

Every write command is a **blind increment with no acknowledgement**, so a UI
built on them must never display a commanded state — only what the *next poll*
reports. Proving the transport and parser against hardware first was worth more
than three buttons. **That rule is written down now so v2 inherits it.**

Two mechanisms already established for v2, from the reference flow (§9):
substitute the command for one poll tick rather than interleaving it, and —
improving on that flow — confirm the relevant field actually changed in the
next validated reading, because a dropped byte otherwise silently does nothing.

---

## 5. Gauge scaling: the meter reports *which* range, never *how many watts*

The manual's VCP lists 25/250/2500 W; the reference station's unit is set to
700/125/25 W; and the flow that drives it hard-codes its own three numbers with
a comment telling you to match them to your meter. Field 5 says which of three
ranges is active and nothing more, so the ceiling **cannot be derived from the
wire**.

Defaults are **1500 / 150 / 25 W** — High is the US legal limit, which never
under-scales a legal station; Low is the one value the manual and the reference
unit agree on. They are edited from the applet's own context menu, because a
gauge full scale is a display preference while the Peripherals tab holds
connection state.

`RangeTracker` follows the meter with a deliberate asymmetry — **expand
immediately, contract only after the smaller range has been held for 2 s**. An
under-scaled gauge pins the needle and hides an overpower condition; an
over-scaled one costs a moment of a generous axis. Three details differ from
the reference flow's version of the same idea, each for a stated reason:

1. **Wall-clock, not record-counted.** The flow counts 20 records and its own
   comment concedes that assumes a 100 ms poll rate — but §6's gate can
   legitimately ride along behind a slow foreign poller, where 20 records is
   minutes.
2. **Contraction needs a *stable* candidate.** The flow's counter advances on
   any disagreement and then adopts whatever a single record says when it
   expires, so a meter hunting between ranges latches on one arbitrary sample.
   With no checksum, a lone corrupt-but-plausible record is likelier here than
   in any checksummed peer.
3. **No "power is present" gate.** In the flow that stalls the timer between
   SSB syllables, making "2 seconds" an unpredictable multiple of itself.
   Dropping it is safe *because* expansion is immediate.

Session-scoped, up-only ceiling expansion covers a mis-set ceiling: two
consecutive over-ceiling records raise it, and `reset()` restores the
configured value on reconnect — never persisted, the same rule
`AcomConnection` applies to its auto-ranged tier.

**The up-only guard applies to a config load, not to the operator.**
`RangeTracker::setCeilings()` takes a `CeilingSource`, and the distinction is
load-bearing rather than decorative. The guard exists so a re-read of stored
settings cannot shrink a ceiling that observed power already expanded — but the
same entry point also serves a deliberate edit from the applet's context menu,
and there it is simply wrong. An operator who lowers High to match their meter
would get no change *and no feedback*, because the ceiling never moves so
`gaugeCeilingChanged()` never fires. An operator action that silently does
nothing is worse than the stale ceiling the guard was written to prevent, so an
`OperatorEdit` always wins. Honouring a mistaken edit is self-correcting: real
power above the new ceiling re-expands it within `kCeilingExpandRecords`
records. Found in review of #5320; three test rows pin all three behaviours so
the two paths cannot be merged back into one.

---

## 6. The wire is shared: listen before you talk

Other clients are commonly already polling the meter through the same ser2net
port. **Measured on the reference station: a second connection that sent
nothing received 60 complete records in 6 s** — the proxy mirrors one client's
replies to every other.

Polling blindly on top of that doubles the meter's work and, for any client
that reads blind after a fixed delay, makes it attribute our replies to its own
polls. So `PollGate` rides along when someone else is polling and polls when
the wire is quiet.

**The subtlety worth preserving.** Gating on "no record in the last N ms" does
not work: our own replies reset the same timestamp, so N sets the solo poll
rate *and* the suppression threshold. Measured against the reference station's
100 ms foreign cadence, N=130 ms suppresses cleanly but caps solo polling at
7.7 Hz, while N=100 ms keeps 10 Hz and polls over the other client 48.5% of the
time. **Gating on *foreign* records only breaks that coupling** — solo stays at
a full 10 Hz and the threshold is free to be whatever suppression needs.

Classification is "at most one reply per poll": the first record after an
unanswered poll is ours, regardless of elapsed time. The local station's
own-reply latency was ≤15 ms, but a remote or VPN ser2net path has no defensible
40 ms ceiling. If a foreign record wins the race it consumes the pending slot;
our later reply becomes the first foreign sample, and the next foreign record
still establishes the shared cadence. The quiet threshold is **2×** the
observed foreign cadence because the multiplier must cover the jitter *tail*
(mean 100.5 ms, gaps to 121 ms) rather than the mean.

**Stated limits.** The threshold is clamped to 2 s, so a foreign poller slower
than that is *not* suppressed — we supplement rather than let our own gauge
follow a 5 s client down. And rebroadcast is confirmed on one proxy, not
guaranteed: where a proxy refuses a second client or serialises per connection,
the gate simply never trips and we poll, which is the plain design. It is an
optimisation that engages where the transport allows and costs nothing where it
does not.

---

## 7. Two protocol facts that look like implementation gaps

Both are documented at their point of use, because both will read as bugs.

**Phase is an unsigned magnitude — the sign is not on the wire at all.** The
manual (p.12) has the *operator* recover it by hand: QSY ~100 kHz and watch
which way the reactance moves. LP-Plot automates that "since it can control
your transmitter's frequency", sweeping to read the slope. 954 captured records
across three frequencies contained no sign character. So the applet renders
|Z| and |phase| and **never** signed reactance or an `R+jX` form.

*Worth recording:* LP-Plot can infer the sign only because it drives the
transmitter — and AetherSDR drives the radio too. The QSY-slope inference is
therefore implementable here in a way it is not in a standalone meter client.
Out of scope for v1; noted so it is not rediscovered.

**A record is not a coherent snapshot.** At key-up the meter holds power and
dBm for **~1.7 s** while Z, phase and SWR revert to idle immediately: 54 of 894
captured records carry real power beside `SWR 1.00`. Consequences:

- **Nothing cross-derives between fields.** Reflected power from
  `P_fwd·Γ²` would read 0 W against 8.77 W forward during that window, which is
  why the applet shows no reflected figure at all.
- **Detection is from physics, not from the mode field.** |Z|∠φ must reproduce
  the reported SWR; coherent records agree to 0.0093 while incoherent ones
  diverge by up to 43. Peak Hold explains *this* capture, but the mechanism is
  "power and impedance have different time constants" and Avg mode has its own
  averaging hold — gating on `peakHoldMode` would silently miss it.

---

## 8. Where the manual is wrong, and what is still unverified

**Wrong, corrected by measurement:**

- Its example record's **Z field is 4 characters; the wire sends 5** (zero-padded).
  That one character is the whole of the 41-vs-42 discrepancy — and it is *not*,
  as an early draft of this work assumed, a missing pad on the Power field.
- It lists **five** alarm values and **two** peak-hold modes. Cycling `A` and
  `F` through full rotations on hardware gives **six** (0–5) and **three**
  (0–2). The reference flow's fuller tables were right.

**Unverified, and stated as such:**

- **What the meter's own display shows for return loss at a perfect match.**
  Not in the manual, and not observed. The applet shows `RL >52 dB`, because
  the physics gives +∞ (a perfect match reflects nothing) while the SWR field
  carries only two decimals — a reported `1.00` means [0.995, 1.005), which
  justifies no more than ~52 dB. The bound is derived from the field's own
  quantisation rather than chosen. **If the instrument turns out to have a
  house convention, match it.**

  Recorded here because the first implementation got this exactly backwards:
  it returned **0.00 dB** at a perfect match, which is the value for *total
  reflection*, and displayed it throughout receive since the meter idles at
  SWR 1.00 — a matched load and a dead short rendered identically. Its comment
  called that "the division-by-zero edge every naive implementation gets
  wrong", which was wrong twice: γ at SWR 1 is (1−1)/(1+1) = 0, so nothing is
  divided by zero; the divergence is log10(0). **A unit test pinned the wrong
  value**, so the suite protected the defect instead of catching it. Found by a
  human reviewer on #5320 after two AI reviews passed over it — the reason the
  test now asserts monotonicity (a near-match must be *large*) rather than one
  point.

- **Whether field 0 is forward or net power.** The meter read 8.62–8.93 W
  against a 10 W TUNE setting at SWR ≈ 1.68; forward implies 0.53 dB of coax
  and calibration loss, net implies 0.24 dB, and both are entirely ordinary.
  It needs a deliberately high SWR to discriminate. **Until then the applet
  labels the field "PWR", as the meter does, and claims nothing** — and nothing
  derives from it in a way the answer would change.
- **dBm width on other units.** It is the only field that is signed *and* not
  zero-padded; this meter idles at −2.3 and peaks at 39.5, but a value in
  −99.9…−10.0 would be 5 characters. The parser keys on field shape rather than
  a hard length, so such a record degrades to a rejection rather than a
  misparse.
- **Firmware before 1.2.0.0** (38400 baud, and before 1.0.3 no dBm or SWR) is
  not supported and not detected. Such a unit produces records the parser
  rejects rather than misreading them.

---

### One deliberate divergence from the sibling peripherals

`LpMeterConnection::setAutoReconnect(false)` **stops an armed retry timer**;
`AcomConnection`, `SpeConnection` and `VkampConnection` all have the flag-only
version (`{ m_autoReconnect = on; }`) whose armed callback never re-reads the
flag, so an operator disabling the option during the five-second delay still
gets one more reconnect.

That is a real defect in all three, found while reviewing this PR, and it is
**theirs to fix, not this PR's** — but copying it here would have been the
mistake §9's standing rule exists to prevent: a working sibling is evidence
that an approach functions, never that it is correct. Both halves are
implemented here (the setter stops the timer, and the callback re-reads the
flag). The default test graph keeps LP-100A coverage socket-free; transport
lifecycle is exercised through code review and the real peripheral path rather
than a loopback TCP peer.

---

## 9. Provenance, and the audit of what was borrowed

Three artifacts informed this work. **None was adopted unexamined** — a working
implementation proves that *some* approach functions, not that it is correct at
the edges.

| Source | Behaviour | Verdict |
|---|---|---|
| Reference flow | `;` frame marker | Adopt |
| Reference flow | Field-offset table | **Adopt** — correct in every field, where the manual is not |
| Reference flow | `length == 42` as validation | **Improve** — catches truncation, not corruption (§3) |
| Reference flow | Expand-now/contract-slowly | **Improve** — three defects (§5) |
| Reference flow | 100 ms poll cadence | Adopt — it also equals the applet's label throttle |
| Reference flow | Leaves DTR/RTS alone | Adopt — and this diverges from `AcomConnection`, which forces both low |
| Reference flow | Alarm 5 / peak-hold 2 | Adopt — since confirmed on hardware |
| Reference flow | `dBW = dBm − 30`, clamped at 0 | **Reject** — the clamp discards everything below 1 W |
| Reference flow | 700/125/25 ceilings | **Reject as defaults** — station-specific (§5) |
| `AcomConnection` | Self-contained `disconnect()`, latched decode warning, 5 s reconnect, up-only guard on a late authoritative value | Adopt — each documents the bug that motivated it |
| `AcomConnection` | `kAutoRangeConsecutiveFrames = 2` | **Improve** — that 2 was chosen against an 8-bit checksum; this protocol has none, so it is defensible here only because `looksLikeRecord()` carries the weight |
| Manual | Field order and semantics | Adopt — corroborated three ways |
| Manual | Example record's field *widths* | **Reject** (§8) |

See `THIRD_PARTY_LICENSES` for the licence-level provenance record.

---

## 10. Applet presence and #4944

Registration follows ACOM/SPE/VKAMP: `markHardwareConditional("LP100")`, absent
from `kDefaultOrder`, `Metering` category, configured from the Peripherals tab.

Issue #4944 asks where an undiscoverable peripheral gets configured, and
whether gating the tile on a *connection* is right when a configured-but-
unreachable device then shows no tile at all. **That objection does not bite
here**, because `LpMeterConnection` deliberately does not drop the link when the
meter stops answering: a wedged meter keeps its tile and shows `NO DATA` inside
it. Only a genuinely absent transport hides the tile. So this adds no further
divergence, and #4944 can be settled on its own merits.

That state is not hypothetical. During bring-up the meter wedged with the
serial link perfectly healthy — TCP up, ser2net serving its banner, zero
records until it was power-cycled. `VkampConnection`'s dead-link watchdog
aborts its socket in that situation, which hides its applet at the moment the
operator most needs an explanation.

---

## 11. Touchpoint tagging

`core/LpMeterConnection.h` is tagged `peripheral(lp100a)` in
`aetherd-touchpoint-tags.json`, matching the `peripheral(acom)`/`(spe)`/
`(vkamp)`/`(4o3a)` precedent. `src/gui/LpMeterApplet.h` needs no entry — the
manifest tracks `core/`/`models/` headers the UI includes, not `gui/` files.

---

## 12. Phasing

1. Wire capture and protocol layer — done, unit-tested against literal captured
   bytes and the manual's own example.
2. Transport — done, verified against real hardware on the ser2net path.
3. Applet, registration, wiring, Peripherals row — done, verified in the
   running app.
4. Community data: the forward-vs-net question and dBm width on other units
   (§8) — ongoing, not gated on a release.
5. v2: the three cycle commands, under §4's rule.
