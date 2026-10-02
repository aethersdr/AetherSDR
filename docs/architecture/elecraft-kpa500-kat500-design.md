# Elecraft KPA500 / KAT500 Support — Design Note

**Status:** KPA500 shipped (this PR); KAT500 deferred to a follow-on commit
under the same issue (#4953).

**Scope:** A dedicated `Kpa500Applet` driven by `Kpa500Connection` (serial
transport + poll loop), a Peripherals settings row, and the protocol codec
(`Kpa500Protocol`). Telemetry decode, Operate/Standby control, fault clear,
and fan-speed adjustment. No band push to the amp, no automatic TX, no
shared controller with the KAT500 (see §1 and §6).

---

## 1. Why this is a peripheral, not a backend

`IRadioBackend`'s canonical amplifier path (`amplifierChanged(AmpDelta)`,
`invokeExtension("flex", "amp.operate", ...)`) exists because the PGXL is
*relayed through the radio* — SmartSDR's own `amplifier` object reports its
presence and proxies its operate command. The KPA500 has no such
relationship: it is a standalone RS-232/USB-serial device the radio has
never heard of.

So, same as `AcomConnection`/`SpeConnection`, `Kpa500Connection` lives
directly under `src/core/`, outside the radio seam, and never touches
`IRadioBackend`/`invokeExtension`. `AmpModel` (PGXL's model) is
untouched — `Kpa500Connection` feeds `Kpa500Applet` directly, not via any
shared amplifier model.

**Protocol authority (Principle I):** the *KPA500 Programmer's Reference
Rev. A2* (Elecraft) is the sole source for wire framing, command syntax,
field scalings, and fault code semantics. Every coded constant cites its
spec section. No third-party reference client code is incorporated.

---

## 2. What gets added

```
Kpa500Protocol (src/core/Kpa500Protocol.h/.cpp)
  FrameParser     — text-frame tokeniser: ^CMD[data]; envelope,
                    calls a callback for each (cmd, arg) pair.
  Status          — decoded snapshot: operate, faultCode, forwardPowerW,
                    swr, paTemperatureC, paVoltageV, paCurrentA, band,
                    fanMinSpeed, powerOn, firmwareVersion, serialNumber.
                    Each field is std::optional — absent until first seen.
  applyMessage()  — updates a Status from one (cmd, arg) pair; returns
                    true iff any field changed (used to gate statusUpdated).
  Builders        — buildOperate/Standby/ClearFault/SetFanSpeed/NullCommand/
                    buildQuery/buildPowerOff (^ON0; — see §4, power-off note).

Kpa500Connection (src/core/Kpa500Connection.h/.cpp)
  Owns the QSerialPort (under #ifdef HAVE_SERIALPORT), two poll timers
  (fast 200 ms, slow 2 s rotating), and reconnect logic (5 s, automatic
  on USB detach/reattach). On transport-up: sends a null command to verify
  communication before starting polls.
  signals: connected/disconnected/connectionFailed/statusUpdated/
           respondingChanged  — "amp went silent" vs. "link dropped"
           (a USB-serial link can outlive the amp being switched off).

Kpa500Applet (src/gui/Kpa500Applet.h/.cpp)
  Dedicated widget — NOT a variant of AmpApplet. Status pill, one HGauge
  (forward power, 0–500 W), SWR/TMP/V/I text readouts, STANDBY/OPERATE
  buttons, fan-speed combo, fault label + CLEAR button. Pill/button styles
  follow AmpAppletStyles.h's shared family vocabulary; the STANDBY key uses
  ampStandbyBtnStyle() — the amber {{color.accessory.key.standby.*}} token
  pair, matching AmpApplet's kPanelKeyStandbyStyle.

AppletPanel  — registers Kpa500Applet as its own dockable panel.

RadioSetupDialog::buildPeripheralsTab()  — KPA500 row: port + baud rate
  (4800/9600/19200/38400 per §^BRP), Connect/Disconnect button, responding
  indicator. Baud default 4800 per spec; configurable for units previously
  reconfigured via ^BRP.

MainWindow_Wiring (wireMeters / wireMiscApplets)
  KPA500 meter-suppression gates: radio's own txMetersChanged and
  directionalPowerMetersChanged are suppressed while the KPA500 is
  connected, responding, AND in operate — all three conditions required.
  If the amp goes silent (respondingChanged → false), isResponding() goes
  false, the gates open, and the radio's own meters resume immediately.
  Power scale pins to 500 W on the same three-way condition; respondingChanged
  reconnects to updatePowerScale so the scale restores when the amp goes
  silent.
```

---

## 3. Protocol summary

Text-based, single serial port, 4800 baud default (8N1, no handshake):

```
^CMD[data];
```

Responses are unsolicited replies to polls (`^WS;`, `^VI;`, etc.) or
direct-set echoes (`^OS1;` → amp echoes `^OS1;`). The amp never pushes
autonomously between poll cycles.

| Command | Direction | Description |
|---|---|---|
| `^OS` | host→amp / amp→host | Operate (`^OS1;`) / Standby (`^OS0;`) state |
| `^WS` | host→amp (query) / amp→host | Forward power (W) + SWR (÷10) |
| `^VI` | host→amp (query) / amp→host | PA voltage (÷10, V) + current (÷10, A) |
| `^TM` | host→amp (query) / amp→host | PA temperature (°C, 0–150) |
| `^BN` | host→amp (query) / amp→host | Band index (0–10, see bandName()) |
| `^FC` | host→amp set/query | Fan minimum speed (0–6) |
| `^FL` | host→amp (query) / amp→host | Fault code (decimal 00–99; 00 = none) |
| `^RVM` | host→amp (query) / amp→host | Firmware version string |
| `^SN` | host→amp (query) / amp→host | Serial number string |
| `^ON` | host→amp | Power state; `^ON0;` = power off (see §4) |
| `;` | host→amp | Null command — used to verify communication on connect |

**Fast poll (200 ms):** rotates through `^OS;`, `^WS;`, `^FC;`.
**Slow poll (2 s):** rotates through `^VI;`, `^TM;`, `^BN;`, `^FL;`.
**One-shot on connect:** `^RVM;`, `^SN;`, `^ON;`.

Poll-silence tracking: three consecutive fast-poll cycles with no reply
set `m_responding = false` → `respondingChanged(false)`.

---

## 4. Command scope

| Tier | Included? | Rationale |
|---|---|---|
| Telemetry decode (^WS, ^VI, ^TM, ^BN, ^FL, ^FC) | Yes | Read-only. |
| Operate / Standby (^OS) | Yes | Core operational control. |
| Fan minimum speed (^FC set) | Yes | User-adjustable from applet combo. |
| Fault clear (^FLC) | Yes | Operator recovery action. |
| Power off / on (^ON0 / ^ON1) | Builders only — no UI wire | Both work over the normal serial port (confirmed by KPA500 Remote and third-party applications). Power-off needs a confirmation step; power-on needs a verified round-trip test on hardware. Both are deferred to a follow-on commit. |
| Band push (^BN set) | **Never** | The amp senses band from the drive signal via its own frequency counter; AetherSDR explicitly does not push band to it (§5). |
| Baud-rate negotiation (^BRP) | Not implemented | The port-selector combo covers units already reconfigured; dynamic renegotiation via ^BRP is deferred. |

---

## 5. TX safety (Principle II)

`setOperate()` sends `^OS1;` and does nothing else — it does not latch any
local state. The OPERATE pill moves only when the `^OS` poll reply lands.
There is no auto-OPERATE on connect or reconnect; the poll loop is pure GET
traffic. No control method reads back its own last write to confirm state —
that is entirely the poll loop's job.

The amp is **never** told what band to use. Band display (`^BN;` poll) is
read-only; AetherSDR never sends `^BN` as a SET.

---

## 6. KAT500 — same issue, separate commit

Issue #4953 covers both the KPA500 amplifier and the KAT500 antenna tuner.
This PR delivers the KPA500 half. The KAT500 is deliberately **not** shared
via a combined `ElecraftStationController` — per #4953 triage's ruling:

> *"No shared ElecraftStationController. Each device is its own
> standalone peripheral — Kpa500Connection, Kat500Connection — each with
> its own applet, its own settings row, and its own poll loop."*

The KAT500's protocol, transport, applet, and settings row are left for the
follow-on commit. Nothing in this PR's architecture prevents them from being
added as a clean parallel structure.

---

## 7. Touchpoint tagging

`src/core/Kpa500Connection.h` is tagged `peripheral(elecraft)` in
`docs/architecture/aetherd-touchpoint-tags.json` and
`docs/architecture/aetherd-touchpoints.md`, matching the `peripheral(4o3a)`
precedent for PGXL/TGXL/Antenna Genius. `src/gui/Kpa500Applet.h` is not
in the touchpoint manifest — the manifest tracks `core/`/`models/` headers
the UI *includes*, not `gui/` files themselves.

---

## 8. Resolved decisions

- **Peripheral, not backend** — no `IRadioBackend` involvement (§1).
- **Dedicated `Kpa500Applet`/`Kpa500Connection`, not AmpApplet variant.**
  The ACOM design note (§2 of its design doc) records why the shared-applet
  approach causes the existing applet to appear with no corresponding device
  present; the same reasoning applies here.
- **No shared controller with KAT500** — per #4953 triage, two independent
  connections (§6).
- **No band push** — amp senses its own band; explicit triage ruling (§5).
- **Stale-readout safety:** `setResponding(false)` clears PWR/SWR/TMP/V/I
  labels and gauge immediately. The meter-suppression gates in
  `MainWindow_Wiring` check `isResponding()` as well as `isConnected()`, so
  a front-panel power-off while USB-serial stays enumerated does not freeze
  the radio's TX meters on the 500 W scale (#4953 triage, safety label).
- **Standby pill:** blue hex via `ampPillStyle(AmpPillState::Standby)` —
  matches ACOM/SPE family. STANDBY *button* uses `ampStandbyBtnStyle()` —
  amber token pair, matches AmpApplet's `kPanelKeyStandbyStyle`.
- **Hardware cross-check:** ^WS/^VI/^FC/^FL scalings could not be verified
  against the KPA500 Programmer's Reference Rev. A2 prior to this PR —
  hardware validation remains a maintainer/owner step before merge.
