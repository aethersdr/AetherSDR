---
title: "Peripherals"
slug: "/peripherals"
description: "the station accessories it talks to directly, over the network or a serial port, rather than through the radio."
---

> **Applies to:** All radios · the TGXL and PGXL radio relay, and the PGXL applet, need a FlexRadio

**Settings → Radio Setup... → Peripherals** is where AetherSDR is told about
the station accessories it talks to directly, over the network or a serial
port, rather than through the radio. It covers the 4O3A Tuner Genius XL
(TGXL), Power Genius XL (PGXL) and Antenna Genius (AG), the ShackSwitch, ACOM,
SPE Expert, VK3AMP and Elecraft KPA1500 amplifiers, and the TelePost LP-100A
wattmeter.

Each device has its own applet once it is connected. Those are described on
[TGXL Tuner Control](./tgxl-tuner-control.md), [Amplifiers](./amplifiers.md) and [ShackSwitch](./shackswitch.md); the PGXL and
Antenna Genius applets are covered below. The Green Heron Everyware switch is
set up from its own applet instead (see [Green Heron Everyware](./green-heron-everyware.md)).

## Setup

Open **Settings → Radio Setup...** (on macOS, **AetherSDR → Preferences...**)
and pick **Peripherals** under CONTROLLERS & HARDWARE.

### Layout

The **Devices** list sits on the left and the selected device's settings on
the right. Each row shows the device name, its address, and where its data is
coming from:

| Word | Meaning |
|---|---|
| `● DIRECT` | Connected directly to the device |
| `● RADIO` | Not connected directly, but the radio relays the device (TGXL and PGXL only) |
| `● OFFLINE` | No connection |
| `Connecting…` | A connection attempt is in progress |

The TGXL, PGXL and Antenna Genius applets use the same words. A device that
needs you (a rejected code, a failed connection, a keychain problem) also
shows **Needs attention**. The detail page repeats the state in a status line
that is always visible.

**Connection Help** at the bottom of the list explains local and remote
(VPN, SmartLink) connections in a few lines.

### Adding a device

1. Click **Add** and pick a device type. A type already in the list stays in
   the menu, greyed, marked "(already added)".
2. Add only creates the entry and selects it. Nothing connects yet.
3. Enter the address (and port, or serial port) and click **Connect**.

The list persists across restarts. Disconnect before changing a device's
address or port; the fields are locked while it is connected.

A TGXL or PGXL that the radio reports, and that is waiting for an access code,
can appear as a temporary row so you can enter the code. Retrying it at its
reported address does not save that address as a manual target; editing the
address or port does.

Serial-capable devices have a **Connection Type** choice, an
**Address/Serial Port** field, **TCP Port/Speed**, and **Refresh Serial
Ports**. The ShackSwitch page has a **⚙ Web UI** button that opens the
device's own web interface. Connection defaults for every device type are
under [Reference](#reference).

### Connecting automatically

TGXL, PGXL, Antenna Genius and ShackSwitch each have a toggle, **on by
default**:

- **Connect directly when available** (TGXL, PGXL). Off, the device is
  controlled through the radio's relay whenever the radio offers one, and its
  applet shows `RADIO`. A live direct session carries on until it drops.
- **Connect automatically** (Antenna Genius, ShackSwitch). These have no
  relay, so off leaves the device offline until you click **Connect**.

With the toggle off, nothing connects that device by itself: not startup, not
discovery, not the radio reporting it, and not a reconnect after a drop.
**Connect** always works and does not change the toggle. To stop a discovered
device from connecting, keep its row and turn the toggle off.

**Reconnect automatically** on the same page retries every peripheral after a
connection drops. (The VK3AMP reads this setting only at startup.)

### Access codes (AUTH)

A TGXL, PGXL or Antenna Genius can require an access code before it accepts a
direct connection. When the device's greeting asks for `AUTH`, AetherSDR sends
the code from the **Auth. Code** field and holds all other commands until the
device accepts it.

- An accepted code is saved in the system keychain. It is bound to how you
  named the device: a literal IP address binds it to that IP and port; a host
  name (for example a DDNS name) binds it to the lower-cased name and port, so
  a changing home IP behind the name does not lose it. Only save a code for a
  name you control.
- Leave the field blank to reuse the saved code for that address. A new code
  replaces the saved one only after the device accepts it.
- Builds without keychain support keep the code for the session only
  ("Code for this session only").
- The code is hidden unless you click **Show**, is concealed again when you
  change page, and is kept out of logs, the automation bridge and widget
  grabs. **Clear code** deletes the saved code.
- After repeated failures with a saved code, the device is blocked:
  "Authorization blocked; enter a code and click Connect to retry".
- If direct authentication fails, a TGXL or PGXL falls back to the radio
  relay. Its applet shows which path it is on.

Antenna Genius and ShackSwitch share one credential slot.

### Removing a device

**Remove** asks for confirmation first. Confirming disconnects the device,
clears its saved connection settings and its stored code, and returns its
toggle to the default. Remove does not stop discovery: a device the radio or
the network reports may connect again, unless you keep its row with the toggle
off.

For a device with a saved code, Setup waits for the keychain to confirm the
deletion and blocks every reconnect of that device meanwhile. If the keychain
does not answer within 15 seconds, Setup reports the deletion as unconfirmed;
close and reopen Setup to retry. If the keychain refuses, the row stays and
says "Saved code remains in keychain; retry Remove".

The full outcome table is in
[docs/peripherals-settings-ui.md](https://github.com/aethersdr/AetherSDR/blob/main/docs/peripherals-settings-ui.md).

## Using the PGXL applet (AMP)

The **AMP** applet (title "PGXL", Amplifiers category) appears when the radio
reports a Power Genius XL.

Docked in the applet panel it is a compact tile:

- **PWR**, **SWR** and **Id** (drain current, read from the radio's `ID`
  meter) bar gauges, plus a drive readout from the radio's `DRV` meter.
- **PA** and **HL** (Harmonic Load) heatsink temperatures. Click either to
  switch between °C and °F.
- **Vac** (mains) and **Vdd** (drain) voltages. `Vdd` reads `0.0 V` most of
  the time the amplifier is idle: it keeps its drain rail down until it
  enters operate. A dash means there is no direct connection.
- One row with **MEffA**, the **Fan** pull-down (STANDARD / CONTEST /
  BROADCAST) and **OPERATE**, above a fixed 2×2 grid of PA/HL and Vac/Vdd.
- The source indicator (`● DIRECT`, `● RADIO`, `● OFFLINE`) at the end of the
  readout row.

Popped out, or placed on the [Workspace Canvas](./workspace-canvas.md), the applet lays itself out
like the amplifier's front panel: larger meters, a status strip for each RF
port showing its band, bias profile and the radio feeding it, a **STBY** key,
and a fan key that cycles `S` / `C` / `B`. The port carrying transmit is
outlined. In standby a single banner replaces both strips. A port with no band
reads `N/A`, meaning nothing is driving it.

The fan control, MEffA, port detail and the voltages need the **direct**
connection. Over the radio relay the applet still shows power, SWR, Id and the
operate state. Meters update at about 60 Hz while transmitting and 4 Hz while
receiving.

**MEffA** is the amplifier's Maximum Efficiency Algorithm: lit while it is
optimising, shown in a standby style while the PA idles in class AAB.

## Using the Antenna Genius applet (AG)

The **AG** applet (Antennas & Switching) controls a 4O3A Antenna Genius.

- It discovers devices on the LAN by UDP broadcast and connects to the first
  one found. A device selector appears when there is more than one.
- An **IP address** field and **Connect** reach a device that discovery cannot
  see (remote, VPN). Remote devices usually need an access code; see
  [Access codes](#access-codes-auth).
- **Port A** and **Port B** each show the current band and antenna and a grid
  of antenna buttons. Click to select; click the selected antenna again to
  deselect it. Port B is hidden on a single-port device.
- An antenna held by the other port is dimmed and cannot be selected. Antennas
  that are receive-only on the current band are marked `RX`.
- **AUTO** per port turns the device's own auto mode on or off for that port.

## Reference

### Device types and defaults

| Device | Connection | Default port |
|---|---|---|
| Tuner Genius XL | TCP, address reported by the radio or entered manually | 9010 |
| Power Genius XL | TCP, address reported by the radio or entered manually | 9008 |
| Antenna Genius | TCP; also found by UDP broadcast on the LAN | 9007 |
| ShackSwitch | TCP (Antenna Genius protocol); always port 9007 | 9007 |
| ACOM Amplifier | Serial (9600 8N1) or a ser2net host in raw mode | 7000 |
| SPE Expert Amplifier | Serial (115200 8N1) or a ser2net host | 7000 |
| VK3AMP Amplifier | TCP control, UDP telemetry | 5005 (UDP 5010) |
| Elecraft KPA1500 | TCP | 1500 |
| LP-100A Meter | Serial (115200 8N1) or a ser2net host in raw mode | 2000 |

### Settings

The TGXL, PGXL, Antenna Genius and ShackSwitch addresses keep their own keys
(`TGXL_ManualIp`, `PGXL_ManualIp`, `AG_ManualIp`, `SS_ManualIp` and the
matching port keys). The device list, **Reconnect automatically**, the
per-device toggles (`Peripherals.<id>.AutoConnect`, ids `tgxl`, `pgxl`, `ag`,
`shackswitch`) and the ACOM, SPE, VK3AMP, KPA1500 and LP-100A connection
settings live in the `Peripherals` document in `AetherSDR.db`. Access codes
live only in the OS keychain. See [Settings and Backups](./settings-and-backups.md).

## Known issues

- The AMP applet stays hidden for a PGXL connected directly by its IP address unless the radio also reports the amplifier ([#6010](https://github.com/aethersdr/AetherSDR/issues/6010)).
- With a PGXL and TGXL on the local network, the AMP applet can show no PWR or SWR while transmitting, although Id is shown ([#4805](https://github.com/aethersdr/AetherSDR/issues/4805)).
- A PGXL reached directly over the internet accepts its access code, shows Connected, then drops straight away ([#6222](https://github.com/aethersdr/AetherSDR/issues/6222)).
- TUNE can fail on a remote FLEX-8600 while the Genius peripherals are connected in AetherSDR ([#6001](https://github.com/aethersdr/AetherSDR/issues/6001)).
- MEffA is not restored when AetherSDR starts; turn it on again each session ([#5956](https://github.com/aethersdr/AetherSDR/issues/5956)).
- The Antenna Genius applet's own **Connect** always uses port 9007 and ignores the port set in Peripherals ([#5997](https://github.com/aethersdr/AetherSDR/issues/5997)).
- The Antenna Genius applet can show a band on Port A that the device did not report, taken from the active slice ([#6151](https://github.com/aethersdr/AetherSDR/issues/6151)).

## Troubleshooting

### A device shows "Authorization blocked"

Repeated failures with the saved access code have blocked the device.

1. Select the device in **Settings → Radio Setup... → Peripherals**.
2. Type the correct code in **Auth. Code**.
3. Click **Connect**. The new code replaces the saved one once the device
   accepts it.

### A device keeps reconnecting after Remove

Remove does not stop discovery, so a device the radio or the network reports
connects again.

1. Keep the device's row in the list (add it again if you removed it).
2. Turn off **Connect directly when available** (TGXL, PGXL) or **Connect
   automatically** (Antenna Genius, ShackSwitch).

### Remove says "Saved code remains in keychain; retry Remove"

The system keychain refused to delete the stored code, so the row stays.

1. Make sure the keychain is unlocked.
2. Click **Remove** again.
3. If Setup reports the deletion as unconfirmed, close and reopen Setup, then
   retry.

### The address and port fields cannot be edited

The fields are locked while the device is connected.

1. Disconnect the device.
2. Change the address or port, then click **Connect**.

## See also

- [Radio Setup](./radio-setup.md)
- [Amplifiers](./amplifiers.md)
- [TGXL Tuner Control](./tgxl-tuner-control.md)
- [ShackSwitch](./shackswitch.md)
- [Green Heron Everyware](./green-heron-everyware.md)
- [AetherSweep](./aethersweep.md)
