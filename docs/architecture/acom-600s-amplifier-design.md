# ACOM S-Series Amplifier Support — Design Note

**Status:** Draft for maintainer review. Not a new `IRadioBackend` family under
the aetherd RFC — the ACOM has no FlexRadio awareness at all, so this adds a
**peripheral accessory** in the same sense as the existing 4O3A PGXL/TGXL/
Antenna Genius integrations (`peripheral(4o3a)` in
`docs/architecture/aetherd-touchpoint-tags.json`), which AGENTS.md's touchpoint
taxonomy explicitly exempts from the `IRadioBackend`-design-doc requirement
that gates a *new radio family*.

**Scope:** A dedicated `AcomApplet` (sibling of `AmpApplet`, not a variant of
it — see §2) driven by `AcomConnection`, a peripheral transport (serial or
ser2net-style TCP, same binary protocol either way), plus a Peripherals
settings row. Telemetry/status decode, Operate/Standby/Off, clear-faults, and
model auto-detection/auto-ranging across the whole current S-series line
(500S/600S/700S/1200S/1400S/2020S). No manual band/antenna override, no
CAT-passthrough config, no factory reset/service-mode/bootloader access, and
deliberately **no model-selector dropdown** (see §6) — see §4 for the full
scope rationale.

---

## 1. Why this is a peripheral, not a backend

`IRadioBackend`'s canonical amplifier path (`amplifierChanged(AmpDelta)`,
`invokeExtension("flex", "amp.operate", ...)`) exists because PGXL is *relayed
through the radio* — SmartSDR's own `amplifier` status object reports PGXL's
presence and proxies its operate command, which is also the only path that
works remote/SmartLink. The ACOM has no such relationship: it is a standalone
RS-232 device the radio has never heard of. So, same as
`PgxlConnection`/`TgxlConnection`/`AntennaGeniusModel`, `AcomConnection` lives
directly under `src/core/`, outside the radio seam, and never touches
`IRadioBackend`/`invokeExtension`. `AmpModel` (PGXL's model) is untouched by
this feature — see §2 for why an earlier draft that extended it got reverted.

**Protocol authority (Principle I):** the manufacturer's own published *"RF
Amplifier ACOM 600S Serial Port Communication Protocol"* (v1.1, 2014-12-04,
eng. Nikolay Nenov) is the primary source for wire framing, telemetry fields,
error-code bitfields, and the command/request envelope. A secondary,
MIT-licensed open-source reference client (`bjornekelund/ACOM-Controller`) was
consulted only as a practical cross-check — confirming the checksum sign
convention and that the spec's mandatory `0x86` acknowledgement is, in
practice, safely omittable on a direct RS-232 link. No code from either
source is incorporated — see `THIRD_PARTY_LICENSES` for the full provenance
record. ACOM's own public product pages (acom-bg.com) are a *third* source,
consulted for §6's rated-power figures — see that section for why the
reference client's embedded power constants turned out not to be trustworthy
for anything but the 600S.

---

## 2. What gets added — and a design reversal worth recording

```
AcomConnection (src/core/AcomConnection.h/.cpp)
  holds a QIODevice* — either a QSerialPort (local COM port) or a
  QTcpSocket (ser2net-style proxy, raw TCP mode) — chosen at connect time.
  One binary framing state machine reads/writes both identically.
    parses:  0x2F  unified telemetry (72 B: mode, power, SWR, temp, DC
                    voltages/currents, active LPF band, carrier freq, fault
                    code — see §3)
             0x21  error-code bitfields (10 words, WARNING/SOFT/HARD tiers)
             0x11  SystemConfig (amplifier type/firmware/serial — one-shot,
                    on request only; drives model auto-detection, §6)
    sends:   0x81  mode-change (Standby/Operate/Off) + clear-soft-faults
             0x91/0x92  telemetry disable/enable (re-armed via keepalive)
             0x02  request-specific-message (used to ask for 0x11)
  tracks:  currentModel() — the effective model tier, resolved by
           auto-detection + auto-ranging, never by user selection (§6)
  signals: connected/disconnected/connectionFailed, telemetryUpdated,
           rawErrorCodesUpdated, modelChanged(name, reason),
           systemConfigReceived(SystemConfig)

AcomApplet (src/gui/AcomApplet.h/.cpp)
  a dedicated widget, NOT built by extending AmpApplet. Three permanent
  HGauge rows — Power / Reflected / SWR — since ACOM's 0x2F frame reports
  all three as independently real fields (unlike PGXL, which only ever
  gave AmpApplet a reason to show Power/SWR/Id). PAM drain current (Id)
  is a text readout, not a gauge — no protocol-defined scale to size an
  axis against. 3-cell-per-row info grid (temp/HV/Id, band/uptime/clear),
  a status pill carrying the auto-detection/ranging diagnostic tooltip,
  and three separate STANDBY/OPERATE/OFF buttons (not one toggling button).

AppletPanel (extended)
  registers AcomApplet as its own dockable panel (acomApplet(),
  setAcomVisible()) — independent of setAmpVisible(), since a station can
  have both a radio-relayed PGXL and a direct-connected ACOM at once.

RadioSetupDialog::buildPeripheralsTab()  (extended)
  ACOM row: Serial ⇄ Network mode toggle, reusing the QSerialPortInfo/
  "Custom…" port-combo pattern from the CW/keying page and the existing
  IP:port fields for Network mode.
```

**Design reversal worth recording:** the first working version of this
feature extended `AmpModel` (PGXL's model) with a direct-connection path,
mirroring `TunerModel::setDirectConnection`, and rendered ACOM telemetry
through `AmpApplet` with additive fields and a click-to-toggle SWR/reflected
row. That shipped, ran against real 600S hardware, and worked — but two
problems surfaced under actual use: it couldn't be made to match the intended
layout without fighting `AmpApplet`'s PGXL-shaped assumptions (fixed 3-gauge
row count, fixed info-stack shape), and — more importantly — routing ACOM's
presence through `AmpModel`'s shared `present()` flag caused the **PGXL
applet to appear whenever an ACOM connected**, even with no PGXL anywhere in
the station, because `AmpModel::present()` couldn't distinguish "PGXL
relayed" from "ACOM direct." Both `AmpModel` and `AmpApplet` were reverted to
byte-identical with `main` and this dedicated-applet design took their place.
The lesson generalized: a peripheral with no relationship to an existing
device's model/view should get its own model and view from the start, not
share one for code-reuse's sake, even when the sharing looks cheap initially.

---

## 3. Protocol summary

Binary framing, symmetric both directions, 9600 8N1, no handshake, ~10 ms
inter-message pacing, 72-byte max message:

```
| 0x55 | Address | Length | ...Data... | Checksum |
```

`Checksum = 256 − (sum of all prior bytes & 0xFF)`; a valid frame sums to 0
mod 256.

| Address | Direction | Contents |
|---|---|---|
| `0x2F` | amp → host | Unified telemetry (72 B): mode (Reset/Init/Debug/Service/STB/OPR-RX/OPR-TX/ATAC/Off), PAM temp, DC power, input/forward/reflected power, SWR, dissipation power, VCC5/VCC26/HV1, PAM current, bias voltages, carrier frequency, active LPF band + fan speed, error code + parameter. |
| `0x21` | amp → host | 10 error-code words, each bit independently named, each tagged WARNING / SOFT FAULT / HARD FAULT. |
| `0x11` | amp → host | SystemConfig — amplifier type, firmware/hardware/bootloader versions, 12-byte serial number, hard-fault record count. **One-shot, on request only** — not pushed automatically like `0x2F`/`0x21`. Drives model auto-detection (§6). |
| `0x02` | host → amp | Request a specific one-shot message by address (used to ask for `0x11`). Sent up to 5 times total (the spec's own retry ceiling — 5 attempts, not 5 retries after an initial send) with an 800 ms timeout between attempts; gives up silently if the amp never replies — auto-ranging (§6) covers that case without blocking on it. |
| `0x81` | host → amp | Command envelope. Sub-command `0x02` = mode change (`0x05`=Standby, `0x06`=Operate, `0x0A`=Off); `0x08` = clear soft faults. (Sub-commands for CAT passthrough config, manual band/antenna override, buzzer, LOG dump, service-mode tests, factory reset exist in the spec and are **not implemented**; see §4.) |
| `0x91` / `0x92` | host → amp | Disable / enable automatic telemetry push. |
| `0x86` | either | Per-message acknowledgement. Sent only for the one-shot `0x11` SystemConfig reply, which has its own request/retry-with-timeout exchange (`0x02`) the ack model naturally fits. **Not** sent for `0x2F`/`0x21`, which are auto-pushed continuously (~10/s) rather than requested — the reference client never acks anything, on any message type, and is a documented-working real-hardware implementation over direct RS-232, so acking isn't required to keep the push streams flowing, and blanket-acking a ~10/s broadcast stopped making sense once the push-vs-request distinction was reconsidered. |

One transport gotcha for the ser2net path specifically: the data stream can
legitimately contain byte `0xFF` (e.g. "no error" is encoded as `0xFF` in the
error-code field). If ser2net is configured with `connection type: telnet`
rather than `raw`, telnet's IAC-escaping corrupts that byte. `AcomConnection`'s
network mode assumes/requires a **raw** TCP proxy.

**Protocol-parity assumption**, stated explicitly since §6 leans on it: the
wire framing, checksum, and the specific messages this project implements
(`0x2F`, `0x21`, `0x11`, `0x81` mode-change/clear-faults, `0x91`/`0x92`,
`0x02`) are assumed identical across the whole S-series — only power-scaling
constants and PAM2-field presence differ per model. This is the manufacturer's
own documented framing, not something invented per-model; nothing in the
public record suggests the bigger amps use a different wire format for the
same message addresses. What's explicitly **not** assumed shared: any
model-specific extras a bigger amp might expose beyond what's implemented
here (e.g. dual-PAM telemetry fields) — those are out of scope for v1
regardless of model, not silently assumed absent.

---

## 4. Command scope for v1

| Tier | Included in v1? | Rationale |
|---|---|---|
| Telemetry + fault decode (`0x2F`, `0x21`) | Yes | Read-only, no risk. |
| SystemConfig query (`0x11` via `0x02`) | Yes | One-shot, read-only, drives model auto-detection (§6). |
| Operate / Standby / Off, clear-soft-faults | Yes | The functional slice needed for normal operation; matches what the widely-used open-source reference client itself implements. |
| Manual band/antenna override, buzzer control | No | Convenience only — the amp senses its own band from the drive signal via its own frequency counter, so AetherSDR never *needs* to tell it the band. Deferred, not rejected. |
| CAT-passthrough config, user-flags settings mirror | No | The amp's own front panel already owns this; duplicating it in AetherSDR adds surface for the two to drift out of sync. |
| Factory reset, service-mode hardware tests, bootloader activation | **Never** (not just deferred) | Destructive/technician-only operations with no legitimate use from a radio-control app. |

---

## 5. Power/Reflected/SWR — three permanent gauges, no toggle

An earlier draft gave the SWR row a click-to-toggle between SWR and reflected
power, on the theory that PGXL's `AmpApplet` only had 3 gauge slots (Power/
SWR/Id) to work with. Once ACOM got its own dedicated applet (§2), that
constraint no longer existed — forward power, reflected power, and SWR are
**all three independently real fields from the same `0x2F` frame**, so
`AcomApplet` just shows all three permanently: no toggle, no interaction
needed to see reflected power, strictly more information at a glance. PAM
drain current (Id) moved to a plain text readout instead of competing for a
4th gauge slot.

- **Power / Reflected** — scaled per the model tier (§6), two-zone
  (green/red, no amber) at that tier's nominal/max.
- **SWR** — fixed 1.0–3.0, three-zone (green/amber/red), same scale
  regardless of model — SWR is a ratio, not a wattage, so it doesn't need
  per-model scaling.

---

## 6. Multi-model support: auto-detection + auto-ranging, not a dropdown

The protocol is assumed shared across the whole line (§3). Getting the right
**power scale** per model is the harder problem, and the design here is
worth spelling out in full since it went through several iterations.

### 6.1 The rated-power table is not what it looks like

Nominal (rated) output power, sourced from **ACOM's own current published
product pages** (acom-bg.com) — not the model name, and not the public
reference client's embedded constants, both of which turned out to be wrong:

| Model | Rated output (ACOM's own spec) | Model name | Reference client's constant |
|---|---|---|---|
| 500S | 500 W | 500 | 500 W — agrees |
| 600S | 600 W | 600 | 600 W — agrees, **and hardware-confirmed** |
| 700S | 700 W | 700 | 700 W — agrees |
| 1200S | **1000 W** | 1200 | 1200 W — **wrong** |
| 1400S | **1200 W** | 1400 | *(not in the reference client at all)* |
| 2020S | **1500 W** | 2020 | 1800 W — **wrong** |

The model name equals the rated wattage for the three smaller amps, but not
for the three bigger ones — 1200S is actually rated 1000W, 2020S is actually
rated 1500W. Neither "infer from the model name" nor "trust the reference
client" holds up as a methodology; ACOM's own current spec pages are the only
reliable source found, and they were checked individually for every model
in scope.

### 6.2 Deriving the gauge ceiling ("max")

ACOM's marketing publishes a single rated figure, not a separate red-zone
ceiling. The 600S is real hardware, so its ratio is real: 700W max is exactly
600W rated + 100W. Applying **that same +100W** to the other two "name equals
rated" models keeps the ceiling a round number and consistent with the one
confirmed data point:

- 500S: 500 + 100 = **600 W**
- 700S: 700 + 100 = **800 W**

For 1200S and 1400S, the model name itself turns out to be `rated + 200W` —
1200 = 1000 + 200, 1400 = 1200 + 200 — a clean enough pattern that the name
is used directly as the ceiling (1200W and 1400W respectively) rather than
computing a separate derived number. 2020S does **not** fit this pattern
(2020 ≠ 1500 + 200 = 1700), so its name isn't power-derived at all and isn't
trusted as a ceiling; it falls back to the 600S-derived ratio instead
(1500 × 1.167 ≈ 1750W).

Final table (`AcomProtocol.h`'s `modelTable()`):

| Model | Nominal (W) | Max (W) | Basis for max |
|---|---|---|---|
| 500S | 500 | 600 | 600S's +100W ratio |
| 600S | 600 | 700 | **confirmed (hardware)** |
| 700S | 700 | 800 | 600S's +100W ratio |
| 1200S | 1000 | 1200 | model name (fits `rated+200`) |
| 1400S | 1200 | 1400 | model name (fits `rated+200`) |
| 2020S | 1500 | 1750 | 600S's ratio (name doesn't fit) |

Reflected power (nominal/max) has no public source at all for any model —
derived proportionally from the 600S's own confirmed ratios (114/600 ≈ 19%
nominal, 150/700 ≈ 21.4% max) applied to each model's forward-power figures.
Temperature offset and PAM2-field presence are carried from the reference
client where no better source exists, defaulted to the majority/inferred
value for 1400S (which the reference client doesn't cover at all). None of
this is independently verified beyond 600S — it's a documented best effort,
not a claim of accuracy.

### 6.3 Why there's no model-selector dropdown

`0x11`'s `Amplifier Type` byte is the wire-level way to ask the amp what it
is — but the spec documents **only one value**: `1 = A600S`. Nothing public
(ACOM's own site, the reference client, forum posts, other technical
articles) confirms what the other five models report. Guessing a mapping —
even a plausible-looking one like "sequential by launch order" — was
considered and rejected: there's no evidence for any particular ordering, and
a wrong guess presented as confident would be worse than no guess at all.

Given the explicit product decision to support the whole line **without**
asking the user to pick their model from a dropdown, the design instead
leans on **auto-ranging from observed telemetry**, which needs no type-code
knowledge at all:

1. On every connect, `AcomConnection` resets to **"600S"** — the one
   confirmed model — as the starting tier, and requests `0x11` (sent up to 5
   times total — not 5 retries *after* an initial send, i.e. `0x11` is never
   sent a 6th time — 800 ms apart, per the spec's own retry ceiling; silently
   gives up if the amp never replies — some firmware may not support the
   query at all, and that's fine, auto-ranging still covers it).
2. If the reply's `amplifierType == 1`, the tier is confirmed as "600S"
   (already the default, but the GUI's diagnostic tooltip changes from
   "default" to "confirmed").
3. On every telemetry frame, observed forward power is compared against the
   *current* tier's max. A `tierForForwardPower()` pure function (unit
   tested against literal wattage values, no hardware needed) finds the
   smallest tier whose ceiling comfortably fits the reading (a 2% margin
   avoids boundary-flapping right at a tier edge). If that required tier is
   **higher** than the current one, the connection jumps directly to it —
   not incrementally through every intervening tier — and re-emits
   `modelChanged` so the GUI re-applies gauge ranges, reflected-power
   scaling, and the temperature offset together as one consistent tier.
4. The tier only ever moves **up**, never back down, for the life of a
   connection — a momentary dip in power is normal operation, not evidence
   of a smaller amp. It resets to "600S" fresh on every reconnect.

This directly satisfies "query the model and adjust scale to match
capability on connect" without needing a trustworthy type-code table at
all: a 500S *cannot* report 900W forward power, so observing 900W is itself
proof the amp is at least 1200S-class, regardless of what its `0x11` reply
says or doesn't say.

### 6.4 Gathering data to fill out the table properly

Nothing here is a substitute for real confirmed type codes. To make it
possible to replace derived numbers with real ones over time:

- **Logging** (`lcTuner` category, same as the rest of `AcomConnection`):
  every `0x11` reply logs the raw `amplifierType` byte, firmware/hardware
  version, and serial number at `qCInfo` level; every auto-ranging tier
  bump logs the forward-power reading that triggered it and which tiers
  were involved. This rides on the existing `SupportBundle` log-collection
  path — no new infrastructure needed to get this data out of a user's
  machine and into a bug report.
- **User-visible surface**: a tooltip on `AcomApplet`'s status pill (not a
  new field in the Peripherals settings row — no other device row there
  surfaces detected-hardware info, only connection status/errors, so a
  tooltip on the applet itself is the more consistent choice) shows the
  detected/current tier and reason (`default`/`confirmed`/`auto-scaled`),
  and after a `0x11` reply, the raw type byte, firmware version, and serial
  number — explicitly inviting a report if the type is unrecognized.
- **The ask**: if you own a 500S/700S/1200S/1400S/2020S, please file a
  GitHub issue with the tooltip's reported type byte and your actual model.
  Once even one more model has a confirmed type code,
  `modelNameForAmplifierType()` gets a second confident `==` case instead of
  relying on auto-ranging for it.

---

## 7. Touchpoint tagging

`core/AcomConnection.h` is tagged `peripheral(acom)` in
`docs/architecture/aetherd-touchpoint-tags.json`, matching the existing
`peripheral(4o3a)` precedent for PGXL/TGXL/Antenna Genius. `src/gui/AcomApplet.h`
needs no entry — the touchpoint manifest tracks `core/`/`models/` headers the
UI *includes*, not `gui/` files themselves. `AmpModel.h` and `AmpApplet.h/.cpp`
are untouched by this feature (§2's reversal put them back to byte-identical
with `main`), so their existing tags are unaffected.

---

## 8. Resolved decisions & open questions

**Resolved**
- Peripheral, not backend — no `IRadioBackend` involvement (§1).
- **Dedicated `AcomApplet`/`AcomConnection`, not a shared `AmpModel`/`AmpApplet`
  extension** — reversed from the original design after it caused PGXL's
  applet to appear with no PGXL present (§2).
- v1 command scope: telemetry + SystemConfig query + Operate/Standby/Off +
  clear-faults only (§4).
- Power/Reflected/SWR are three permanent gauges, no toggle (§5).
- Whole current S-series line supported (500S/600S/700S/1200S/1400S/2020S),
  via auto-detection + auto-ranging, deliberately **without** a model-selector
  dropdown (§6).
- Rated-power table sourced from ACOM's own product pages, not the model name
  or the reference client's constants, both shown to be unreliable (§6.1).
- **`0x86` acknowledgement:** sent only for the one-shot `0x11` SystemConfig
  reply, not for the auto-pushed `0x2F`/`0x21` streams — see §3's protocol
  table for the reasoning (the reference client's own no-ack-anywhere
  behavior, cross-checked as a working real-hardware precedent).
- **`systemClockSec` word order:** cross-checked against a real 600S —
  its front-panel `127:43:17` (H:MM:SS) reading matches the applet's
  decoded `5d 7h` display exactly (459,797s either way), confirming the
  current high-word/low-word order is correct, not swapped.

**Open (for maintainer input)**
- **Id gauge scale removed as a concern** — Id is a text readout now, not a
  gauge, so there's no axis to calibrate.
- **500S/700S/1200S/1400S/2020S power table** — derived, not measured; every
  entry except 600S needs a real owner to confirm it (§6.4).

---

## 9. Phasing

1. `AcomConnection` + binary framing/checksum + `AcomApplet` — shipped,
   iterated against real 600S hardware across several rounds (layout,
   button/info-grid grouping, uptime semantics).
2. Model auto-detection (`0x11`/`0x02`) + auto-ranging + corrected multi-model
   table — this round.
3. Hardware validation pass against real 600S: confirm `SystemConfig` request
   actually elicits a reply (settles the `0x02`-standalone vs.
   `0x81`-sub-command-`0x01` ambiguity the spec's translation left open),
   confirm auto-ranging behaves sanely if ever exercised, `0x86` ack behavior,
   ser2net raw-mode round trip.
4. Community data-gathering for the rest of the model table (§6.4) — ongoing,
   not gated on a release.
