# Elecraft KPA500 / KAT500 Support — Design Note

**Status:** KPA500 implementation in this change; KAT500 deferred to a separate PR
under the same issue (#4953).

**Scope:** A dedicated `Kpa500Applet` driven by `Kpa500Connection` (serial or raw TCP
transport + poll loop), a Peripherals settings row, and the protocol codec
(`Kpa500Protocol`). Telemetry decode, Operate/Standby control, fault clear,
fan-speed and speaker adjustment, explicit power control, and optional TX-band
following (enabled by default). No automatic TX or Operate, and no
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

**Protocol references:** the *KPA500 Programmer's Reference
Rev. A2* (Elecraft) defines framing, commands and field scaling. Read-only lab replies from
firmware 1.54 also establish the two-digit `^FC00;` fan reply. No proprietary
client code or decompiler output is incorporated.

---

## 2. What gets added

```
Kpa500Protocol (src/core/Kpa500Protocol.h/.cpp)
  FrameParser     — text-frame tokeniser: ^CMD[data]; envelope,
                    calls a callback for each (cmd, arg) pair.
  Status          — decoded snapshot: operate, faultCode, forwardPowerW,
                    swr, paTemperatureC, paVoltageV, paCurrentA, band,
                    fanMinSpeed, powerOn, speakerOn, firmwareVersion, serialNumber.
                    Each field is std::optional — absent until first seen.
  applyMessage()  — updates a Status from one (cmd, arg) pair; returns
                    true iff any field changed (used to gate statusUpdated).
  Builders        — buildOperate/Standby/ClearFault/SetFanSpeed/NullCommand/
                    buildQuery/buildPowerOff/buildPowerOn/SetBand/SetSpeaker.

Kpa500Connection (src/core/Kpa500Connection.h/.cpp)
  Owns the QSerialPort (under #ifdef HAVE_SERIALPORT) or QTcpSocket, two poll timers
  (one fast command every 200 ms, one slow command every 500 ms), and reconnect logic (5 s, automatic
  on transport failure when enabled). On transport-up: sends a null
  command and starts polling; opening a port does not prove a response.
  signals: connected/disconnected/connectionFailed/statusUpdated/
           respondingChanged  — "amp went silent" vs. "link dropped"
           (a USB-serial link can outlive the amp being switched off).

Kpa500Applet (src/gui/Kpa500Applet.h/.cpp)
  CanonApplet with confirmed-state controls, normal and mini views,
  five HGauge meters and a power peak marker. Expanded telemetry graphs use
  per-command samples, monotonic time, a ten-minute window and a hard cap of
  4,096 points per series. Missing replies introduce gaps. A CLCK-style
  Settings drawer contains Follow TX (default true), temperature units,
  transport/identity details and a nested debug log capped at 2,000 lines.
  Only UI/follow preferences persist in the Kpa500Applet settings document.

AppletPanel — registers Kpa500Applet as its own dockable panel. The compact
  bar label is KP500; the full Elecraft KPA500 name remains in the title and
  accessibility label. The stable KPA500 container/settings ID is unchanged.

RadioSetupDialog::buildKpa500Device() — Add > Elecraft KPA500: Serial/Network, host/TCP port or local port + baud rate
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

Shared meter selection additionally requires current power-on and forward-power
readbacks (`Kpa500Connection::hasLiveTxMeters()`). Individual `^WS` expiry releases
both radio-meter suppression gates and the amplifier scale even when other
replies keep the transport responding. A fresh measured zero is valid telemetry.

---

## 3. Protocol summary

Text-based, single serial port, 4800 baud default (8N1, no handshake):

```
^CMD[data];
```

GET commands return device state. SET requests are confirmed through
subsequent GET replies; sending a SET does not establish the new state.

| Command | Direction | Description |
|---|---|---|
| `^OS` | host→amp / amp→host | Operate (`^OS1;`) / Standby (`^OS0;`) state |
| `^WS` | host→amp (query) / amp→host | Forward power (W) + SWR (÷10) |
| `^VI` | host→amp (query) / amp→host | PA voltage (÷10, V) + current (÷10, A) |
| `^TM` | host→amp (query) / amp→host | PA temperature (°C, 0–150) |
| `^BN` | host→amp set/query / amp→host | Band index (0–10, see bandName()) |
| `^FC` | host→amp set/query | Fan minimum speed (0–6) |
| `^FL` | host→amp (query) / amp→host | Fault code (decimal 00–99; 00 = none) |
| `^RVM` | host→amp (query) / amp→host | Firmware version string |
| `^SN` | host→amp (query) / amp→host | Serial number string |
| `^ON` | host→amp | Power state; `^ON0;` = power off (see §4) |
| `P` | host→amp | Bootloader power-on, single uppercase byte, no terminator |
| `^SP` | host→amp set/query | Fault speaker enable |
| `;` | host→amp | Null command — used to verify communication on connect |

**Fast poll:** one command every 200 ms, rotating through `^WS;`, `^OS;`,
`^BN;`, `^FL;`; each field is requested every 800 ms.
**Slow poll:** one command every 500 ms, rotating through `^TM;`, `^VI;`,
`^ON;`, `^FC;`, `^SP;`; each field is requested every 2.5 s.
The speaker query is suppressed while power is unknown/off or after an OFF
request, since the bootloader interprets uppercase `P` as wake.
**Quiet mode:** connect first checks `^ON;`. An OFF request, `^ON0;` reply,
or silence suspends both normal poll schedules. Only `^ON;` is sent every
five seconds. A valid `^ON1;` after a power probe resumes normal polling;
in-flight non-power replies cannot revive it. Explicit wake still waits two
seconds before its first power query. No periodic probe contains `P`.
**One-shot on connect/recovery:** `^RVM;`, `^SN;`.
Fast fields expire individually after 3 seconds; slow fields after 8 seconds.

Poll-silence tracking accepts only valid decoded replies as evidence of
liveness, including unchanged values. Silence invalidates the whole cached
snapshot and any partial frame. Recovery refreshes identity and waits for
fresh values; an unrelated reply cannot resurrect old operate or meter state.
SWR `000` is unmeasured, not a numeric zero.

---

## 4. Command scope

| Tier | Included? | Rationale |
|---|---|---|
| Telemetry decode (^WS, ^VI, ^TM, ^BN, ^FL, ^FC) | Yes | Read-only. |
| Operate / Standby (^OS) | Yes | Core operational control. |
| Fan minimum speed (^FC set) | Yes | User-adjustable from applet combo. |
| Fault clear (^FLC) | Yes | Operator recovery action. |
| Power control | Explicit ON/OFF button | `^ON0;` powers off; bootloader `P` wakes. Polling waits two seconds after wake. No inferred ON/OFF confirmation. |
| Band selection (^BN set) | Manual or Follow TX | Follow TX defaults on; explicit user-approved lab scope. Confirmed dropdown, deferred during TX, no automatic Operate. |
| Baud-rate negotiation (^BRP) | Not implemented | The port-selector combo covers units already reconfigured; dynamic renegotiation via ^BRP is deferred. |

---

## 5. TX safety (Principle II)

`setOperate()` sends `^OS1;` and does nothing else — it does not latch any
local state. The OPERATE pill moves only when the `^OS` poll reply lands.
There is no auto-OPERATE on connect or reconnect. No control method reads back its own last write to confirm state —
that is entirely the poll loop's job.

Follow TX uses the TX-assigned slice (not the active RX slice), its XIT and
FM transmit offset, and the active BandPlanManager regions. A receive-only
backend or missing/unsupported TX source leaves the amplifier band alone.
Manual requests and automatic targets share one connection-level queue.
Radio TX or measured RF defers changes; unknown power/band/RF state waits for
readback. Only the newest target is sent after RX. Requests time out after
three seconds without automatic retry storms. Displayed state is never the
last requested state. Disconnect drops manual pending work.

The status banner derives **RF ACTIVE** only from fresh amplifier forward-power
readback (`^WS` > 0), not the radio TX flag. Rev. A2 exposes no RCA KEY IN/PTT
status command; `^TR` is T/R delay, not key state. The radio TX flag remains a
separate interlock for power/mode/band changes.

The applet's serial controls do not generate PTT, TUNE or RF. This is a
convenience band follower, not a hardware transmit interlock; the radio's
transmit gate and the amplifier's own protections remain independent.

---

## 6. KAT500 — same issue, separate PR

Issue #4953 covers both the KPA500 amplifier and the KAT500 antenna tuner.
This PR delivers the KPA500 half. The KAT500 is deliberately **not** shared
via a combined `ElecraftStationController` — per #4953 triage's ruling:

> *"No shared ElecraftStationController. Each device is its own
> standalone peripheral — Kpa500Connection, Kat500Connection — each with
> its own applet, its own settings row, and its own poll loop."*

The KAT500 applet will use **KT500** in the applet bar. The planned KXPA100
amplifier applet has the working short label **KX100**. Neither future button
is registered before its applet exists, and both must be gated on their own
device detection. Full model names remain in titles and device settings.

The KAT500's protocol, transport, applet, and settings row are left for the
separate PR. Nothing in this PR's architecture prevents them from being
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
- **TX band following and manual band selection** — user-approved lab expansion;
  maintainer UX/scope review remains separate from local deployment (§5).
- **Stale-readout safety:** `setResponding(false)` clears PWR/SWR/TMP/V/I
  labels and gauge immediately. The meter-suppression gates in
  `MainWindow_Wiring` check `isResponding()` as well as `isConnected()`, so
  a front-panel power-off while USB-serial stays enumerated does not freeze
  the radio's TX meters on the 500 W scale (#4953 triage, safety label).
- **Visual design:** canon tokens and controls, explicit state words, visible
  faults in mini mode, guarded band/fan dropdowns, no colour-only state.
- **Hardware evidence:** GET-only lab replies from firmware 1.54 confirm
  standby, zero RF, identity, voltage/current, temperature, band, fan and
  fault framing. Windows serial and Mac raw-TCP application checks also cover
  configuration backup, banner readback and stored fault history. Physical
  restore/banner-write persistence and transmitted-power accuracy remain unverified.

## Raw TCP serial proxy

Network mode connects directly to a raw TCP ser2net endpoint. The same bounded
frame parser, power-check mode, command guards and fresh-detection lifecycle
serve both transports. TCP has a ten-second connection deadline and optional
five-second reconnect; explicit disconnect cancels an in-flight attempt. The
server owns baud/8N1/flow-control configuration. No Telnet/RFC2217 negotiation,
Elecraft Remote Server framing, AetherD path or KAT500 transport is implied.

Peripherals stores ConnectionMode, ManualIp and ManualPort alongside existing
SerialPort and BaudRate in Peripherals.Kpa500. Missing mode preserves local
serial behavior. The applet Settings and Peripherals Device Information display
firmware/serial from amplifier readback, without persisting hardware identity as
configuration or restoring any amplifier setting. The Windows Utility version
is a separate program version and is not presented as amplifier firmware.


## KPA500 Advanced maintenance

`Kpa500Maintenance` implements the bounded EEPROM framing and `.KPAConfig`
codec; `Kpa500FaultHistory` decodes the stored ring. `Kpa500Connection` owns
single-operation scheduling, transport, cancellation and verification.
`Kpa500AdvancedWidget` presents those operations in Peripherals. The applet's
Advanced shortcut selects that device and expands the panel.

The maintenance format comes from clean-room runtime observations of KPA
Utility 1.13.7.16 over an isolated virtual COM pair, with an artificial device
reporting firmware 01.54. No proprietary code was disassembled or copied. The
firmware gate remains exact 01.54 pending evidence for other revisions; the
observed Utility behavior does not itself prove hardware write compatibility.

`^ER` reads and `^EW` writes encode a two-byte address, one-byte count, data
(for replies/writes) and two's-complement byte checksum as hexadecimal text.
Blocks are at most 64 bytes. Configuration spans 0x0000–0x00df (224 bytes);
backup files hold seven 32-byte `MEM2` records. CRC32 covers all preceding text
lines without CR/LF, polynomial EDB88320, initial FFFFFFFF and no final XOR.
Two separately generated Utility backups validate this checksum. Banner reads
use address 0x0050, length eight; writes use eight padded ASCII bytes plus NUL.

Fault reads cover 1024 bytes. Byte 0xfe identifies the next slot in a 128-entry
ring at 0x0100. Each six-byte record holds a 24-bit little-endian uptime,
band/code nibbles and 16-bit little-endian data. Only a six-byte all-FF record
is empty. Uptime can reset, so ordering follows the ring pointer. Invalid
pointers fail closed rather than guessing an order. Stored fault codes and
band indices are distinct from the normal `^FL` and `^BN` telemetry.

Writes require explicit user action, matching serial/firmware, known Standby
and zero RF. Every write block follows a fresh SN fence and ON/OS/WS queries;
all safety replies must be no older than three seconds. The protocol has no
transaction IDs: the fence relies on an exclusive ordered serial/raw-TCP
stream and cannot disambiguate arbitrary duplicated same-command responses.
Normal polling, follow-band and setters pause until the transaction ends.
Read requests have bounded retries; writes have none. The complete written
range must read back identically before success. Cancellation, TX, disconnect,
malformed/unexpected replies or timeout abort; partial writes are reported as
incomplete. No transaction resumes after reconnect, and no reboot command is
sent. Live telemetry is invalidated and reacquired after maintenance.
