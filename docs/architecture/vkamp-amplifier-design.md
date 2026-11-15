# VK3AMP Amplifier Support — Design Note

**Status:** Draft for maintainer review. Not a new `IRadioBackend` family under
the aetherd RFC — the VK3AMP has no FlexRadio awareness at all, so this adds a
**peripheral accessory** in the same sense as the existing 4O3A PGXL/TGXL/
Antenna Genius integrations and the ACOM S-series amplifier
(`docs/architecture/acom-600s-amplifier-design.md`), which AGENTS.md's
touchpoint taxonomy explicitly exempts from the `IRadioBackend`-design-doc
requirement that gates a *new radio family*. This doc follows that same
precedent closely — VK3AMP and ACOM are the same shape (standalone amp, own
wire protocol, no radio relay) — and calls out where VK3AMP's protocol forces
a different shape.

**Scope:** A dedicated `VkampApplet` (sibling of `AmpApplet`/`AcomApplet`, not
a variant of either) driven by `VkampConnection`, a peripheral transport (TCP
or serial, two genuinely different wire formats — see §3), plus a Peripherals
settings row. Telemetry (forward/reflected/input power, SWR, current, supply
voltage, temperature), Bypass, Cooling override, Antenna-port select (1–3),
Voltage-rail select (low/high), and Reset (hold-to-confirm). No band control
(the amp doesn't expose one remotely — see §1), no arbitrary-voltage dial (the
protocol only has two fixed rails — see §5), no antenna/band-table editing (no
known remote write path exists at all).

---

## 1. Why this is a peripheral, not a backend

VK3AMP is a standalone Ethernet/serial-controlled RF amplifier with **zero
radio awareness** — no CAT link to AetherSDR, no shared session, no
`amplifierChanged(AmpDelta)`-style relay the way PGXL rides through the
FlexRadio's own `amplifier` status object. It sits downstream of the radio in
the RF chain and is addressed directly, exactly like ACOM. Some VK3AMP units
*do* have their own CAT-follow logic for band tracking (see §3), but that's
the amp sensing the radio's TX frequency on its own front-end hardware, not
anything AetherSDR participates in or can read back — there is no remote
command that returns the amp's actual RF/LPF band (see the "band" discussion
in §3). So, same as `AcomConnection`/`PgxlConnection`/`TgxlConnection`,
`VkampConnection` lives directly under `src/core/`, outside the radio seam,
and never touches `IRadioBackend`/`invokeExtension`.

**Protocol authority (Principle I) and clean-room compliance (Principle IV):**
unlike ACOM, there is no manufacturer protocol document for VK3AMP (VK3AMP/
Helios DX, TCI_VKAMP firmware family) — the wire format here comes from a
companion reverse-engineering project (`vkamp_client.py`, a separate personal
project, not affiliated with this repo), built from packet captures against
real hardware — capturing and studying the protocol as it actually behaves on
the wire, one of Principle IV's explicitly clean inputs. Every field/command
below is tagged with its confirmation level in that project's own comments;
nothing here is a vendor spec, so treat "confirmed" claims as "confirmed
against the specific unit(s) that project tested," not as guaranteed true of
every VK3AMP/TCI_VKAMP-family amp in the field.

**One piece is explicitly NOT clean and must NOT be ported into this
codebase:** the companion project's `ERROR_CODE_NAMES` fault-name table (which
numeric error code means "Error Forward!" vs. "Error Voltage!" etc.) was
sourced by decompiling the vendor's own Windows control app
(`Helios DX.exe`) with `ilspycmd` and reading its `UpdateErrorDisplay` switch
statement — not from packet capture, not from public documentation. Per
Principle IV, "transcribed, translated, or paraphrased from [decompiled]
output... must never enter the codebase, however correct or convenient it
is," and unlike ACOM's reference-client cross-check (consulted, not
incorporated), this table's *only* source is the decompile itself, with no
independent clean confirmation behind it. **v1 must therefore surface the
numeric `error_code` field as-is (plain "Error 3", not a fault name) until a
clean-room fault-name mapping exists** — e.g. deliberately triggering each
fault against real hardware and reading the name off the amp's own front-panel
display or a public manual, if one ever surfaces. This is called out
explicitly here rather than silently carried over, since the whole point of
Principle IV is that contamination isn't a local defect that's easy to spot
later — it has to be kept out at the door.

**Provenance ordering — was any *retained* protocol knowledge decompile-
derived, even indirectly?** Principle IV's contamination clause isn't scoped
to the one excluded table: "a contribution that began clean but pulled in
decompiler output at any point is contaminated... it travels to everything
written by reading it." Since `vkamp_client.py`'s author is also the one who
decompiled `Helios DX.exe`, that's a fair question to ask explicitly rather
than leave implicit — answered here directly from the companion project's own
git history, not from memory:

- The wire protocol retained here — bare 2-digit ASCII commands, the status/
  telemetry CSV formats, and the antenna/bypass behavior this design doc
  documents in §3.1/§4 — is already fully present, and narrated as live
  hardware discovery ("hardware disproved that: sending `3{n}` only ever
  produces...", "there is no known way to...") in that project's **very first
  commit**, dated 9 days before any commit references the decompile at all.
- The decompile is introduced in a single later commit titled "add
  unconfirmed fault-name hint from TCI_VKAMP RE" — exactly the
  `ERROR_CODE_NAMES` table this doc already excludes above, not a rewrite or
  extension of the already-established protocol work.
- That project's own `.gitignore`, written contemporaneously (not as
  after-the-fact justification), states the decompile dump was pulled in "to
  cross-reference against this project's own reverse-engineering (e.g.
  `vkamp_client.py`'s `ERROR_CODE_NAMES`)" — scoped to the fault table by the
  author's own note at the time, matching this doc's exclusion exactly.
- The calibration curves (design doc §3.2) are least-squares fits against
  named, independently-dated packet-capture files cross-referenced with
  external meter and front-panel readings — a numeric curve fit isn't
  information a decompiled UI switch statement could produce, regardless of
  which commit date the final fitted constants landed on.

So: capture-based protocol work came first and stands independent of the
decompile; the decompile's only role, then or since, is the one table this
doc already refuses to carry over.

---

## 2. What gets added

```
VkampConnection (src/core/VkampConnection.h/.cpp)
  holds a QIODevice* — either a QTcpSocket (control port, default 5005) or a
  QSerialPort (local COM port) — chosen at connect time, same
  either-transport pattern as AcomConnection. Unlike AcomConnection, the two
  transports are NOT one framing state machine reading both identically —
  TCP and serial are two unrelated wire formats here (see §3), so this class
  owns two independent parsers behind one connection-state/telemetry-signal
  surface, picking one path at connect time and never mixing them.
  A second QUdpSocket (port 5010) handles live telemetry — TCP/serial only
  carry status (temp/volts/band/antenna/error/tx/cooling/bypass), not
  power/SWR, which is UDP-only and TX-gated (see §3).
    parses (TCP): ASCII CSV status broadcast, comma-separated, no
                    terminator — temp_C, volts×10, band(f1), antenna(1-3),
                    error_code, tx, cooling_override, bypass
              (UDP): ASCII CSV telemetry frame — output, reflected, current,
                    input_raw (all raw/uncalibrated — see §3 for the
                    calibration curves)
              (serial): 15-byte binary frames, different fields entirely and
                    far less complete — see §3.3
    sends:   "21"/"22"  bypass on/off
             "45"/"46"  cooling-override on/off
             "41"/"42"  voltage rail low/high (two fixed setpoints, not a
                        dial — see §5)
             "3{1,2,3}" antenna port select
             "23" (repeated, ~9-12s hold)  reset — see §4's hold-to-confirm
                        note
             "11" (UDP) telemetry trigger/keepalive — see §3's retrigger note
  tracks:  currentBand()/currentAntenna()/isBypassed()/isCoolingOverride()/
           voltageIsLow() from the live TCP/serial status stream
  signals: connected/disconnected/connectionFailed, statusUpdated,
           telemetryUpdated, resetProgress(remaining, finalizing)

VkampApplet (src/gui/VkampApplet.h/.cpp)
  a dedicated widget, NOT built by extending AmpApplet or AcomApplet. Three
  permanent HGauge rows — Power / Reflected / SWR — same reasoning as
  AcomApplet §5: this protocol's UDP telemetry frame reports all three as
  independently real fields, not a fixed 3-slot layout inherited from PGXL.
  Current (A) is a text readout, same as ACOM's Id — no protocol-defined
  axis to scale a gauge against, just a calibrated linear reading. Info grid:
  temp (°C), supply voltage, band (read-only display — see §1), antenna port
  (1/2/3, read-only display mirroring the amp's own state, since a
  successful select_antenna() call is not a guarantee it sticks — see §3.1),
  and a fault indicator showing the raw numeric error code only (e.g.
  "Error 3"), NOT a fault name — see §1's clean-room note on why no name
  table ships in v1.
  Controls: BYPASS/COOLING toggle buttons (mirroring the amp's own two
  confirmed status bits), three ANT1/ANT2/ANT3 buttons, a LOW/HIGH voltage
  toggle (disabled while bypass is active — see §5's safety note, itself
  ported directly from a real-hardware finding in the companion project),
  and a RESET button gated behind a hold-to-confirm dialog (mirroring
  AetherSDR's existing destructive-action confirm pattern, not a bespoke
  one).

AppletPanel (extended)
  registers VkampApplet as its own dockable panel (vkampApplet(),
  setVkampVisible()) — independent of setAmpVisible()/setAcomVisible(),
  since a station can have a radio-relayed PGXL, a direct ACOM, and a direct
  VK3AMP all present at once, each fully independent hardware.

RadioSetupDialog::buildPeripheralsTab()  (extended)
  VK3AMP row: Serial ⇄ Network mode toggle, reusing the QSerialPortInfo/
  "Custom…" port-combo pattern already established for ACOM/CW-keying, plus
  IP:port fields for Network mode (default port 5005).
```

This is architecturally identical to ACOM's dedicated-applet design (see that
doc's own §2 "design reversal" — extending a shared `AmpModel`/`AmpApplet`
caused the PGXL applet to spuriously appear whenever an unrelated peripheral
connected). VK3AMP gets the same treatment from the start, not after
discovering the same problem a second time.

---

## 3. Protocol summary

VK3AMP genuinely has **three separate wire formats**, not one — this is the
biggest structural difference from ACOM's single symmetric binary frame.

### 3.1 TCP control/status (port 5005)

Plain ASCII, ~100–300 ms broadcast latency **only while something prompts a
reply** — confirmed the amp is request/response for status when idle, not a
spontaneous broadcaster: 20 straight seconds of pure idle silence produced
zero status replies in testing, and re-sending the "11" ping produced one
instantly, every time. This matters for the connection-health design (§6):
a naive "no status in N seconds = dead link" watchdog will misfire constantly
unless it accounts for genuine idle silence between whatever this client last
sent.

```
Status broadcast: "<temp_C>,<volts x10>,<band>,<antenna>,<error_code>,<tx>,<cooling>,<bypass>"
Commands (bare ASCII, no terminator): "21"/"22" bypass, "45"/"46" cooling,
  "41"/"42" voltage rail, "3{1-3}" antenna select, "23"×N reset hold
Command ack: 2 or 6 NUL bytes (not meaningful beyond "something replied")
```

- **`antenna`** (field 4) is a direct antenna-relay select (1–3), confirmed
  live to do nothing at all for values 4–8 (no reply, no relay click) — it is
  **not** a band selector, despite an earlier (later-corrected) assumption in
  the companion project that it was. The amp also has a firmware-resident
  "antenna per band" table (front-panel-editable only, no known remote read
  or write) that can silently revert an accepted antenna-select command
  within ~50 ms if it disagrees — so `VkampApplet`'s antenna-port display
  must reflect the live status field, not just latch the button the user
  clicked.
- **`band`** (field 3) is the amp's real RF/LPF band indicator, 8 confirmed
  codes (160/80/40/30/20/17-15/12-10/6 — two relay-group pairs share a code).
  It is **read-only from this protocol** — driven by the amp's own CAT-follow
  logic sensing the connected radio's TX frequency, or by the physical
  front-panel band knob — there is no command that sets it remotely. (A nice
  potential follow-up, out of scope for v1: if `VkampConnection` can observe
  AetherSDR's own current TX frequency locally, the applet could show
  "expected vs. actual" band as a mismatch warning — but that's a second
  feature layered on top of read-only display, not something to build into
  v1's scope.)
- **`voltage`** has no dedicated confirmation bit in the status frame at
  all (unlike bypass/cooling, which each get one) — only two fixed rails
  exist (~41.6 V low / ~57.8–57.9 V high, confirmed 4-for-4 across two
  live low/high cycles), so the only way to know which is active is
  inferring it from the live `volts` reading against the rail midpoint
  (~49.7 V) — see §5's real-hardware safety finding for why this inference
  needs a Bypass guard.

### 3.2 UDP telemetry (port 5010)

Separate ASCII CSV frame, TX-gated — **the amp only streams real values while
transmitting**; it is silent at all other times. Triggered/kept alive by
sending `"11"` to the telemetry port; the amp presumably tracks "where to
send" by `(ip, port)` of the last trigger it saw, so this needs periodic
re-triggering (a few seconds) on the **same** socket for the life of the
connection — churning sockets/ports on a fixed interval creates a real risk of
losing telemetry right as a TX starts, exactly when a user would notice.

```
"<output>,<reflected>,<current>,<input_raw>"   (all raw/uncalibrated)
```

All four values need a calibration curve to become real units — quadratic
fits against external reference measurements (output/reflected/input power)
and a linear fit (current), all independently re-derivable from a fresh set
of reference readings if this project doesn't want to trust the companion
project's own fitted constants outright. SWR is **not** a raw wire field —
it's computed client-side from calibrated output/reflected watts
(`ρ = √(reflected/output)`, `SWR = (1+ρ)/(1−ρ)`), returning 1.0 when there is
no calibrated forward power to divide by, matching the amp's own idle display.

**Out-of-range behavior — one rule for all three quadratics.** A
least-squares fit is only meaningful where it is monotonically *increasing*;
below its own vertex it turns decreasing (more raw counts, less power), which
is unphysical for these sensors and is the fit signalling that it has left the
data it was built from. So `VkampProtocol.cpp`'s `calibratedPower()` follows
each curve above its vertex and tapers linearly to 0 W at 0 raw counts below
it. Two things this deliberately does **not** do, both of which the first cut
of this design got wrong and are worth stating so they don't come back:

- It does not scale a real in-range reading by a fraction of itself. An
  earlier revision floored the output curve just under the confirmed 157–166
  cluster and ramped over 3 counts beneath that, which turned an ordinary
  1-count ADC dip to raw 156 into a 109 W → 72 W jump — the same flicker on a
  steady carrier that moving the anchor was meant to cure, relocated eight
  counts down. Multiplying a measurement by a fraction never made it truer.
- It does not let an intercept masquerade as a reading. The reflected curve
  bottoms out around 2.0 W and returns ~2.43 W at 0 raw counts, so before the
  taper a perfectly matched load reported ~2 W reflected and an inflated SWR
  (1.35:1 at 109 W forward). Zero counts now means zero watts on every curve,
  which is why no code anywhere needs a "no carrier" special case.

The constants themselves are unchanged; only the out-of-range handling is.

### 3.3 Serial (COM port) status — a different format, and far less complete

If VK3AMP support is extended to serial in a later phase: this is a
**completely different, fixed-width 15-byte binary frame**, not the TCP
format over a different pipe. Only supply voltage, temperature, a likely
TX-flag byte, and calibrated output power are meaningfully decoded in the
companion project so far; antenna/band/error/cooling/bypass status bits are
**not present in this frame at all** (they're TCP-only fields). Only the
bypass command is confirmed to work over this transport — voltage/cooling/
antenna/reset commands are untested over serial. Recommend treating serial
support as an explicit v2, gated behind its own real-hardware confirmation
pass, rather than assuming TCP-confirmed commands work identically over the
wire just because the framing looks similar to ACOM's serial-or-TCP-same-
protocol case — VK3AMP's serial format is demonstrably **not** the same
protocol as its TCP format, unlike ACOM's.

---

## 4. Command scope for v1

| Tier | Included in v1? | Rationale |
|---|---|---|
| Telemetry (UDP, TX-gated) + status (TCP) | Yes | Read-only, no risk, protocol well-confirmed. |
| Bypass, Cooling override | Yes | Each has a dedicated, confirmed status-bit round-trip. |
| Antenna select (1–3) | Yes | Confirmed hard 1–3 range; applet displays live state, not an optimistic latch, since the amp's own antenna/band table can revert it. |
| Voltage rail (low/high) | Yes, with a hard interlock | Two confirmed fixed setpoints only — **never expose as a continuous dial**, and disable entirely while Bypass is active (see §5's real-hardware finding: commanding a rail change while bypassed was observed pulling the amp toward ~0 V). |
| Reset | Yes, hold-to-confirm | The amp's own firmware requires a ~9–12s held command stream before it acts — a single "23" send does nothing; the applet must reflect a hold/progress state, not a fire-and-forget button. |
| Manual band override | **No** | Not exposed remotely by this protocol at all — the amp senses band via CAT-follow or the physical front panel; there is no command to set it (§3.1). |
| Arbitrary/continuous voltage | **No** | The protocol has exactly two fixed setpoints — a slider or numeric-entry control would misrepresent hardware capability that doesn't exist. |
| Antenna/band-table edit | **Never** (not just deferred) | No known remote write path exists; even the vendor's own PC app edits this locally with zero network traffic on Save. |
| Serial-transport control beyond Bypass | Deferred to v2 | Unconfirmed against real hardware for every other command (§3.3). |
| Fault **name** table (code → "Error Forward!" etc.) | **No — numeric code only** | The only known mapping is decompile-derived (Principle IV violation) — see §1. Ships as a bare numeric code until a clean-room source exists. |

---

## 5. Voltage rail: two fixed setpoints, and a real hardware safety finding

Same "don't invent capability the protocol doesn't have" reasoning as ACOM's
own §6/§5 discussion, but the concrete shape is different: there's no
multi-model auto-ranging problem here (one amp, one protocol, no product
line to detect), just a single interlock that a real live test on the
companion project surfaced and that this design carries forward as a hard
requirement, not a suggestion:

- Bypass produces a distinct standby voltage reading (~6.3 V observed) that
  is **neither** rail target — a naive "`volts` below the rail midpoint means
  low rail selected" inference misreads this as "low rail active" even
  though nobody touched the voltage control.
- Sending a voltage-rail command **while bypassed** was observed live pulling
  the amp's supply down toward ~0 V — an actively bad outcome, not just a
  cosmetically wrong reading.
- The fix carries over directly: `VkampApplet`'s voltage buttons show
  **neither** state highlighted while bypass is active, are **disabled**
  (not just visually muted) for the entire time bypass is on, and
  `VkampConnection`'s own command path refuses to send a voltage command
  while the last-known status says bypassed — a second guard below the UI
  layer, not reliance on the button's disabled state alone.

---

## 6. Connection health / idle behavior — a real gotcha worth designing around up front

Two timing facts, both confirmed the hard way (i.e., wrong assumptions
shipped, then had to be corrected) in the companion project, worth stating
here so this implementation doesn't rediscover them independently:

- **The amp is silent during genuine idle time** — it does not broadcast
  spontaneously; TCP status only arrives in reply to something this client
  sent (a command, or a periodic keepalive ping). A watchdog that assumes
  "healthy connection = periodic unprompted status" will misfire on every
  quiet stretch.
- **Reply timing has no correlation ID at all** — the wire protocol has no
  sequence/request-ID field, so a command's "confirmation" is only ever
  "the next status reply that arrives after I sent something," which breaks
  down if more than one command is ever in flight unconfirmed at once (a
  late reply for command N can land on N+1's wait instead). The safe
  pattern, confirmed necessary via live testing: send at most one command at
  a time and wait for it to either be confirmed or time out before sending
  the next, rather than a naive "send immediately store timestamp, match by
  elapsed time" scheme.

Neither of these needs re-deriving from scratch against real hardware again —
they're stated here as design constraints for `VkampConnection`'s reconnect/
command-pacing logic, sourced from a project that already burned real time
confirming them live.

---

## 7. Touchpoint tagging

`core/VkampConnection.h` and `core/VkampProtocol.h` are tagged
`peripheral(vkamp)` in `docs/architecture/aetherd-touchpoint-tags.json`,
matching the existing `peripheral(4o3a)`/`peripheral(acom)`/`peripheral(spe)`
precedent, and `docs/architecture/aetherd-touchpoints.md` is regenerated
(`python3 tools/gen_touchpoint_manifest.py`). Nothing in CI checks that
manifest, so it has to be regenerated by hand with any change that adds or
removes an engine header the UI includes. `src/gui/VkampApplet.h` needs no
entry, same rule as `AcomApplet.h` (§7 of the ACOM doc). `AmpModel.h`/
`AmpApplet.h/.cpp` and `AcomConnection`/`AcomApplet` are untouched by this
feature.

---

## 8. Resolved decisions & open questions

**Resolved**
- Peripheral, not backend — no `IRadioBackend` involvement (§1).
- **Dedicated `VkampApplet`/`VkampConnection`**, not a shared `AmpModel`/
  `AmpApplet`/`AcomApplet` extension — same reasoning as ACOM's own §2
  reversal, applied from the start this time.
- v1 command scope: telemetry + status + Bypass + Cooling + Antenna select +
  Voltage rail (with the bypass interlock) + Reset. No band control, no
  continuous voltage, no antenna/band-table editing (§4).
- Voltage rail is two fixed setpoints with a hard bypass interlock, not a
  dial (§5) — carries forward a real-hardware safety finding, not a
  theoretical concern.
- TCP and serial are two **different** wire formats, not one framing
  reused over two transports (unlike ACOM) — serial support is deferred to
  v2 pending its own real-hardware confirmation pass (§3.3, §4).
- Connection-health design must account for genuine idle silence and the
  lack of any command/reply correlation ID (§6).
- **The companion project's decompile-derived fault-name table is excluded
  from this codebase entirely** (Principle IV) — v1 ships the raw numeric
  error code only (§1, §4).

**Open (for maintainer input)**
- **Protocol authority is a companion reverse-engineering project, not a
  vendor spec** (§1) — worth the maintainer's own judgment on whether that
  evidence bar meets this project's Constitution Principle I the same way
  ACOM's manufacturer-documented protocol did, or whether additional
  independent confirmation is wanted before merge. The narrower question of
  whether that project's own decompile work (§1's Provenance ordering
  paragraph) contaminated the *retained* protocol knowledge is answered
  there directly from its git history — capture-based work demonstrably
  predates the decompile by 9 days, and the decompile's only role is the
  already-excluded fault-name table. What remains open is the broader
  evidence-bar judgment call above, not that narrower provenance question.
- **Calibration constants** (output/reflected/input/current curves, §3.2)
  are fitted against one (or a small number of) real unit(s) — like ACOM's
  own multi-model power table, these may need adjustment as more hardware
  reports back.
- **UI treatment of the read-only band field** — plain text display for v1
  (§3.1); the "expected vs. actual band" mismatch idea is a possible v2
  feature, not decided here.

---

## 9. Phasing

1. `VkampConnection` (TCP control/status + UDP telemetry only) +
   `VkampApplet` (Power/Reflected/SWR gauges, current/temp/voltage/band/
   antenna info grid, Bypass/Cooling/Antenna/Voltage/Reset controls) — this
   round.
2. Hardware validation pass: confirm calibration curves against this
   maintainer's own unit(s), confirm the bypass/voltage interlock behaves
   correctly live, confirm reset's hold-to-confirm timing feels right in the
   applet's own progress UI.
3. Serial-transport support (§3.3) — separate round, gated on its own
   real-hardware confirmation of which commands actually work over that
   wire.
