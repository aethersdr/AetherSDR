# SPE Expert Amplifier Support — Design Note

**Status:** Draft for maintainer review. A **peripheral accessory** in the
same sense as the ACOM S-series integration
(`docs/architecture/acom-600s-amplifier-design.md`) and the 4O3A
PGXL/TGXL/Antenna Genius family — `peripheral(spe)` in
`docs/architecture/aetherd-touchpoint-tags.json`. The SPE has no FlexRadio
awareness at all, so this is explicitly NOT a new `IRadioBackend` family.

**Scope:** A dedicated `SpeApplet` driven by `SpeConnection`, a peripheral
transport (serial, or a ser2net proxy in raw **or telnet** mode — the same
protocol bytes flow either way), plus a Peripherals settings row.
Status/telemetry decode with model identification, Operate/Standby toggle,
power-level cycle, TUNE, switch-off, the band/antenna/input keys, and remote
power-ON (§4 — the one feature that needs the proxy in RFC 2217 telnet mode).
No menu navigation (arrows/SET/DISPLAY), no manual L/C ATU stepping, no
CAT-configuration mirror.

---

## 1. Why this is a peripheral, not a backend

Same reasoning as the ACOM design note §1: the SPE Expert is a standalone
USB/RS-232 device the radio has never heard of. `SpeConnection` lives
directly under `src/core/`, outside the radio seam, and never touches
`IRadioBackend`/`invokeExtension`. Per the ACOM note's recorded design
reversal ("a peripheral with no relationship to an existing device's
model/view should get its own model and view from the start"), the SPE gets
a dedicated `SpeConnection`/`SpeApplet` pair from day one — `AmpModel`,
`AmpApplet`, `AcomConnection`, and `AcomApplet` are all untouched.

**Protocol authority (Principle I):** the manufacturer's own published
*"Application Programmer's Guide — Expert 1.3K-FA / 1.5K-FA / 2K-FA"*,
Rev 1.1 (2015-10-15), SPE s.r.l. — the primary source for packet framing,
the keystroke command set, and the Status string's field/warning/alarm
tables. The real-hardware cross-check is the contributing author's own
working control application for a 1.5K-FA over ser2net (see
`THIRD_PARTY_LICENSES` for the provenance record), which contributed three
facts the spec omits: commands need a trailing CR LF in practice, the
per-power-level bar thresholds for the 1.5K-FA (LOW 450/500/600, MID
950/1000/1100, HIGH 1450/1500/1600 — i.e. nominal−50 / nominal /
nominal+100 per level), and that a ser2net proxy in telnet mode works fine
on real hardware (the validation station runs one; see §3).

---

## 2. What gets added

```
SpeProtocol (src/core/SpeProtocol.h/.cpp)
  pure protocol layer, zero Qt-networking dependency, unit-tested:
    framing:  host->amp   0x55 0x55 0x55 | CNT | DATA | CHK  (+ CR LF)
              amp->host   0xAA 0xAA 0xAA | CNT | DATA | CHK...
              (ACK carries a 1-byte checksum; the 67-char Status string a
               16-bit one + CR LF — the parser dispatches on CNT)
    decodes:  Status string (19 CSV fields: ID, standby/operate, RX/TX,
              bank, input, band, TX ant + ATU state, RX ant, power level,
              output W, SWR ATU, SWR ANT, V PA, I PA, 3 temps,
              warning, alarm)
    builds:   front-panel keystroke commands (0x01..0x11), backlight,
              status request (0x90)
    tables:   band names, warning/alarm texts, per-model display scaling

SpeConnection (src/core/SpeConnection.h/.cpp)
  holds a QIODevice* — QSerialPort (115200 8N1; the amp auto-adapts, spec
  §1) or QTcpSocket (ser2net, raw or telnet mode) — chosen at connect time.
  Owns the 100 ms status poll loop (the SPE never pushes; it only answers),
  poll-silence detection (respondingChanged — with ser2net the TCP link
  outlives the amp being switched off), auto-reconnect, model
  identification from the Status ID field, and the power-ON pulse state
  machine (RFC 2217 over telnet, or local DTR/RTS on serial — §4).
  signals: connected/disconnected/connectionFailed, statusUpdated,
           modelChanged(id), respondingChanged(bool)

SpeApplet (src/gui/SpeApplet.h/.cpp)
  three permanent HGauge rows — Power / SWR(antenna) / SWR(ATU input) —
  since the Status string reports all three as independently real fields
  (same reasoning as AcomApplet's three-gauge layout). V/I/temperature are
  text readouts (no protocol-defined scale to size an axis against). Temps
  are shown with a bare ° sign: the amp reports degrees in whichever unit
  its own display is configured for, without saying which (spec §5).
  3-cell info grid (temp/V/I, band/antenna/input·level), status pill
  (OPR·TX / OPR·RX / STANDBY), fault banner (alarms + warnings), and two
  keystroke button rows: ON (hardware pulse, §4) / OPER-STBY toggle / power
  level (the button's label IS the current LOW/MID/HIGH) / TUNE / OFF, and
  INPUT / ANT / ▼ / ▲ —
  the arrows being the Expert's front-panel arrow keys, which adjust the
  drive power the amp requests from the radio over CAT. The power gauge
  rescales with the selected level (§5), matching the amp's own display.

AppletPanel (extended)
  registers SpeApplet as its own dockable panel (speApplet(),
  setSpeVisible()) — independent of setAmpVisible/setAcomVisible; a station
  can run a PGXL, an ACOM, and an SPE at once.

RadioSetupDialog::buildPeripheralsTab()  (extended)
  SPE row: Serial ⇄ Network mode toggle, structurally identical to the ACOM
  row (settings nested under PeripheralSettings device "SpeExpert").
```

---

## 3. Protocol summary

Everything is a front-panel keystroke or a poll — there is no richer command
envelope and no unsolicited telemetry. Serial setup: 8N1, no parity, no
handshake, up to 115200 (the amp auto-adapts to lower rates).

| Direction | Shape | Contents |
|---|---|---|
| host → amp | `55 55 55 CNT DATA CHK` + CR LF | Keystroke codes 0x01–0x11 (INPUT, BAND±, ANTENNA, L±/C±, TUNE, SWITCH OFF, POWER, DISPLAY, OPERATE, CAT, arrows, SET), backlight 0x82/0x83, status request 0x90. CHK = mod-256 sum of DATA (= the byte itself for these 1-byte commands). |
| amp → host | `AA AA AA 01 cmd CHK` | ACK: echoes the received keystroke. 1-byte checksum. |
| amp → host | `AA AA AA 43 <67 ASCII chars> CHKlo CHKhi CR LF` | Status string: 19 comma-separated fields behind a leading marker char. 16-bit mod-256/div-256 checksum. |

The trailing CR LF on host commands is not in the spec's packet diagram but
is required in practice — commands without it were intermittently ignored by
real 1.5K-FA hardware over a ser2net link. Two bytes outside the framed
packet are harmless to a sync-run-keyed parser, so they are always sent.

Poll cadence is 100 ms — the spec allows "several times every second", the
GUI refreshes readouts at 10 Hz anyway, and the reference application's
300 ms poll made the power bar visibly stair-step.

Network mode expects a ser2net proxy in **raw or telnet** mode — unlike the
ACOM (raw-only), both are supported and telnet is verified on real
hardware: the validation station's ser2net runs `accepter: telnet`. The
parser's sync-run resync shrugs off telnet negotiation, and the rare status
frame whose checksum byte is 0xFF (which telnet IAC-escapes) is dropped and
re-polled 100 ms later — harmless in a polled protocol. Remote power-ON (§4)
is the one feature that needs more than telnet framing: it drives the proxy's
control lines via RFC 2217, so that port must be
`accepter: telnet(rfc2217=true),<port>`. Everything else — polling,
telemetry, every keystroke — works over a raw port too.

Status fields decoded (spec §5): amplifier ID (`13K`/`15K`/`20K`),
STANDBY/OPERATE, RX/TX, memory bank (A/B, 1.3K/1.5K only), input port 1/2,
band index 00 (160 m)–11 (4 m), TX antenna + ATU state
(tunable/bypassed/enabled), RX-only antenna, power level L/M/H, output
power (W), SWR before the ATU, SWR at the antenna, PA voltage, PA current,
heatsink temperatures (upper/lower/combiner — lower and combiner are real
only on the 2K-FA), one warning letter, one alarm letter (both tables
transcribed in `SpeProtocol.cpp`).

---

## 4. Command scope for v1

| Tier | Included in v1? | Rationale |
|---|---|---|
| Status poll + decode (0x90) | Yes | Read-only, no risk. |
| OPERATE toggle, POWER-level cycle, SWITCH OFF | Yes | The functional slice for normal operation; each is one keystroke with the amp's own protections behind it. |
| TUNE | Yes | Explicit, user-initiated click — transmit-on-intent (Principle VI) is satisfied by it being a deliberate button, exactly like the amp's own front-panel key. The amp itself refuses to tune without drive ("Tuning with no power" warning). |
| ANTENNA, INPUT | Yes | One-keystroke conveniences the status display fully reflects on the next poll. |
| ◄/► arrow keys (▼/▲ buttons) | Yes | On the Expert these adjust the requested drive power from the radio over CAT — an operating-time control, not menu navigation. |
| BAND± | No | The amp follows the radio's band via CAT/RF sensing on its own; a manual band override from a radio-control app invites disagreement between the two. Deferred, not rejected. |
| Backlight on/off | Builder only | `buildBacklightCommand()` exists in the protocol layer (it's free) but no GUI surface yet — deferred, not rejected. |
| SET / DISPLAY (menu navigation), L±/C± manual ATU stepping | No | Blind menu navigation without the amp's display is a foot-gun; SPE reserves complex operations for their own KTerm application, and this integration respects that boundary. |
| Remote power-ON (ON button) | Yes | Not a protocol command — the Expert powers on via a pulse on a hardware line of the serial connector. Over the network `SpeConnection::powerOn()` drives the proxy's DTR/RTS lines via **RFC 2217** COM-port-control (needs `accepter: telnet(rfc2217=true),<port>` — the Peripherals row's tooltip carries the reference `ser2net.yaml`); on a local COM port it drives the lines directly. The pulse sequence (DTR on 100 ms, DTR off + RTS on 1000 ms, DTR on + RTS off) is carried verbatim from the field-proven reference application, so **RTS carries the 1 s power pulse** and the sequence ends with RTS low. The ON button is the one control that stays enabled while the amp is silent — that is its entire purpose. Because a proxy that is not in RFC 2217 mode silently discards the SET-CONTROL frames, `powerOn()` reads the peer's answer to `WILL COM-PORT-OPTION` and reports what it actually agreed to — `DO`, `DONT`, or no answer at all (raw mode) — instead of claiming the pulse landed. The pulse itself is always sent: sending into a proxy that ignores COM-port control is harmless, so the gate is on the *reporting*, not on the attempt. |
| Firmware upload, settings/antenna presets | **Never** | KTerm territory (spec §1); no legitimate use from a radio-control app. |

---

## 5. Model handling: the ID field makes it trivial

Unlike the ACOM (whose `0x11` type code is documented for exactly one model,
forcing that design into auto-ranging heuristics), the SPE reports its
identity in **every** Status reply: field 1 is `13K`, `15K`, or `20K`. So
`SpeConnection::modelChanged` fires on the first poll reply and the GUI
applies the right scale immediately — no dropdown, no heuristics, no tier
ratchet.

Per-model display constants (`SpeProtocol.cpp::modelTable()`):

| ID | Model | Nominal (W) | Yellow from (W) | Gauge max (W) | Banks | Combiner temps |
|---|---|---|---|---|---|---|
| 13K | 1.3K-FA | 1300 | 1250 | 1400 | A/B | no |
| 15K | 1.5K-FA | 1500 | 1450 | 1600 | A/B | no |
| 20K | 2K-FA | 2000 | 1950 | 2100 | — | yes |

The 1.5K-FA row is **hardware-validated** (the thresholds are carried from a
field-proven control application for that exact model). The 1.3K-FA and
2K-FA rows apply the same shape (yellow at rated−50, ceiling at rated+100)
to SPE's published rated output — derived, not measured; owners of those
models are invited to correct them. An unknown ID (an older or future model)
falls back to the 1.5K-FA entry and logs the raw ID at info level so it can
be reported.

The power gauge additionally rescales with the **selected power level**:
red starts at that level's nominal, yellow at nominal−50 W, ceiling at
nominal+100 W (LOW 500, MID 1000, HIGH = model nominal for the 1.3K/1.5K;
LOW 1000, MID 1500 derived for the 2K-FA). The 1.5K-FA level thresholds are
hardware-validated; the rest follow the same shape.

The whole axis comes from `Spe::levelGaugeRange()`, and at HIGH it is the
table row above **verbatim** — the GUI wiring never re-derives it. That is
deliberate: an owner correcting the 1.3K-FA or 2K-FA numbers, as invited
above, would otherwise be editing a table that nothing on screen reads.
LOW and MID have no tabulated thresholds, so they take the derived shape.
`spe_protocol_test` pins both halves, asserting the HIGH axis against the
table's own fields rather than against literals.

SWR gauges are fixed 1.0–3.0 regardless of model — a ratio needs no
per-model scaling (same convention as AcomApplet).

---

## 6. Poll-silence vs. disconnect

The SPE only speaks when spoken to, and with ser2net the TCP transport
happily stays up while the amplifier is switched off. So `SpeConnection`
distinguishes:

- **disconnected()** — the transport itself went away (socket drop, serial
  unplug). Applet hides, auto-reconnect arms.
- **respondingChanged(false)** — the transport is up but 30 consecutive
  polls (~3 s) went unanswered. The applet stays visible but greys its pill
  to "—", disables the command buttons (which would otherwise silently do
  nothing), and **blanks every reading** — gauges, supply V/I, heatsink
  temperature, band/antenna/level, and the alarm banner all go back to their
  not-yet-known state. Only the source label and the identified model
  survive, because those are still true. The reset is the point: over
  ser2net this is the *only* signal that arrives when an operator switches
  the amplifier off, and a panel frozen at the last poll's plausible-looking
  numbers reads as live telemetry. Polling continues; the first reply
  repopulates everything.

---

## 7. Touchpoint tagging

`core/SpeConnection.h` is tagged `peripheral(spe)` in
`docs/architecture/aetherd-touchpoint-tags.json`, matching the existing
`peripheral(acom)`/`peripheral(4o3a)` precedent. `src/gui/SpeApplet.h` needs
no entry — the manifest tracks `core/`/`models/` headers, not `gui/` files.

---

## 8. Testing

- `tests/spe_protocol_test.cpp` (pure, `Qt6::Core` only): host framing
  verified against the spec's literal OPERATE and Status-request byte
  sequences; ACK and Status parsing round-trips including the spec's own
  verbatim 67-character Status example; parser resync past noise, bad
  checksums, implausible CNT, and split feeds; the full field decode for
  both an RX/standby and a TX string; warning/alarm/band/model tables; and
  the RFC 2217 frame literals plus the COM-PORT-OPTION reply scan that
  `powerOn()` gates on.
- `tests/hgauge_range_test.cpp` (offscreen Qt Widgets): pins the gauge-rescale
  contract `SpeApplet::setPowerRange` depends on — a range change re-maps the
  current reading onto the new axis, and does so even when the reading itself
  never changes. Renders the widget and measures the painted fill, because
  the defect it guards against (PR #4531) was invisible to every value-level
  assertion: `value`/`min`/`max` all read correct while the bar was wrong.
- Hardware validation: developed and to be soak-tested against a real
  1.5K-FA over ser2net in telnet mode (the contributing author's station;
  §3). Serial-mode validation on real hardware is pending — the transport
  code is shared with the network path and structurally identical to
  AcomConnection's, but this is stated, not assumed.

## 9. Open questions (for maintainer input)

- **DTR idle level — RESOLVED on the bench (real 1.5K-FA, 2026-08-03).**
  With DTR held high after the power-ON pulse: no `R` ("Power switch held
  by remote") warning in the status stream, SWITCH OFF and every other
  keystroke work normally, and repeated ON→OFF cycles behave. The power
  switch rides the RTS pulse alone. Ruling: **DTR-high is the harmless
  idle state**; `connectSerial()` now comes up DTR-high/RTS-low to match
  the pulse's terminal step, so the resting state no longer depends on
  session history and reconnects produce no edge. Same bench session also
  established that ser2net 4.3.11 with a plain `accepter: telnet` port
  replies `DONT COM-PORT-OPTION` yet still **executes** SET-CONTROL —
  the pulse works even where negotiation says refused (the
  `telnet(rfc2217=true)` config stays the documented recommendation; the
  REFUSED log wording says "may not have reached" accordingly).
- 1.3K-FA / 2K-FA gauge thresholds are derived (§5) — need owners to
  confirm.
- Whether the ACK for a keystroke should drive optimistic UI updates.
  v1 deliberately waits for the next status poll (≤300 ms) instead —
  radio-authoritative live state (Principle II) applied to a peripheral.
