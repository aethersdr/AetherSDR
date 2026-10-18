---
title: "KXPA100 Amplifier and Tuner"
slug: "/kxpa100-amplifier-control"
description: "Connect an Elecraft KXPA100, control its amplifier and optional tuner, and associate its amplifier metering with a radio."
status: "Experimental"
applies_to: ["All radios"]
platforms: ["Linux", "macOS", "Windows"]
---

:::info[Status]

**Status:** Experimental · **Applies to:** All radios · **Platforms:** Linux, macOS, Windows

:::

The **KX100** applet combines KXPA100 amplifier controls, meters and optional KXAT100 tuner controls. Selections and measurements come from device readback. Connecting does not key the radio or load stored client-side amplifier settings.

## Requirements

Use a local serial connection or raw TCP serial bridge such as ser2net or the companion **aether-serial-bridge** project for remote access. Only one client can own the serial port. Network mode does not support Telnet/RFC 2217 negotiation. The bridge controls baud, 8N1 format and handshake lines.

Configuration, backup restore and memory tools support the verified firmware **01.39** layout. Other firmware shows an availability explanation. Use Elecraft's official Utility for firmware updates; AetherSDR has no firmware updater or raw Command Tester.

Connect the radio PTT output to the KXPA100 **PA KEY** input as required by your station. Software association does not create that physical keying connection. Turn on the amplifier locally: the protocol has no remote power command. **Standby** bypasses amplification; it does not turn off DC power.

## Setup

1. Open **Settings → Radio Setup... → Peripherals**, click **Add**, and select **Elecraft KXPA100**.
2. Select **Serial** and the port/baud, or **Network** and your bridge host/port. Click **Connect**.
3. The **KX100** applet button becomes available after identification. It remains available while the transport is open, including stale replies, and disappears when the connection closes.
4. To use amplifier output on the shared **VU** and **PWR** meters, select the intended radio and click **Use with current radio** in the peripheral page or applet **Settings**. This explicitly associates the amplifier serial number with that radio.

Connection preferences are client settings. The metering association belongs to the selected radio and works with or without the optional KXAT100 tuner. Switching radios does not transfer that association.

For **Network**, enter a hostname or IP address without a URL scheme, path, spaces or control characters, and a port from 1 to 65535. Invalid entries are rejected before connecting.

## Using the applet

**Standby** and **Operate** control the PA. **Auto**, **Manual** and **Bypass** control the separate tuner mode. **ANT 1** and **ANT 2** select the tuner antenna. The selected state changes after readback. Missing tuner hardware leaves tuner controls dimmed with an explanation while PA controls remain available.

**Tune** starts a tuner search; it does not generate a radio carrier. Supply RF according to the Elecraft instructions. **Cancel** requests an end to the search. **Clear fault** clears the current fault, leaving stored fault history intact.

Full view shows output power and SWR, followed by **DRV** input power measured by the KXPA100 (0–10 W), reflected power, dissipated power, temperature, supply voltage and drain current. Power meters use watts; SWR uses a 1–5 scale. The pill retains the exact reading beyond the scale. SWR is the last observed measurement and can remain after RF stops. Press and hold the **SWR** pill to preview bypass SWR without changing any relays; release to return to adjusted SWR. A dash means the reading is unavailable.

**Mini** keeps the **OPR/STBY** PA toggle, **Tune/Cancel**, a mode/antenna/status readout, output power and SWR. **Full** restores the other controls. Expanded drawers and small popouts scroll when needed.

The **ATU BYP** pill toggles the physical matching-network bypass relay. It leaves Auto/Manual mode unchanged; a recall or tune may engage the network again. Use the main **Bypass** mode button to disable tuning. **ATT** toggles the input attenuator when software control is available. The rear-panel switch or firmware protection can override it. Hover over these pills for the complete meaning. Requests require fresh, idle readback with no RF.

<img src="/img/kxpa100/kxpa100-full-replay.png" width="260" alt="KXPA100 Full applet with amplifier and tuner controls, DRV and output meters; illustrative replay values." />

*Full view with replayed telemetry illustrating the meters.*

<img src="/img/kxpa100/kxpa100-mini.png" width="260" alt="KXPA100 Mini view with PA, tune, mode and antenna status, output power and SWR." />

### Manual, History and Settings

**Manual** exposes capacitance in pF and inductance in nH, with each relay labeled by its component value. Numeric **Set** selects the nearest available relay combination to the entered total. The capacitor-side control changes which side of the matching network contains the capacitors. **Match frequency** selects the kHz frequency for **Recall** and **Memorize**. These commands require 1800–54000 kHz; an unknown frequency sends nothing. Recall loads a stored match; Memorize saves and verifies the current match. Neither generates RF.

**History** plots forward power, reflected power and last-observed SWR, with a selectable time window, scales and legends. Missing replies leave gaps. The graph shrinks as well as grows with its popout.

**Settings** shows radio association, identity, firmware, serial, DIRECT/NETWORK endpoint, freshness and operation results. **Open Advanced settings** opens the same maintenance page as Peripherals. **Debug** shows bounded diagnostics; **Verbose** includes wire traffic. New entries follow the bottom unless you scroll up or pause. Debug TX means serial output, not RF transmission.

### Radio controls and shared metering

**TX Controls** always operates the radio's own tuner and memories. Connecting or associating a KXPA100 never replaces ATU/MEM, adds an overlay, or bypasses the internal tuner. A radio without an internal tuner shows those controls dimmed with an explanation. Use **KX100 → Tune** and **Manual → Memorize** for the external tuner.

The shared **VU** and **PWR** meters use associated KXPA100 output on the amplifier scale when PA Operate and idle-tuner readback are fresh. This does not require an installed KXAT100. An operating radio-integrated amplifier has priority, followed by an operating KPA500; confirmed KAT500 KEY BLOCK retains the radio fallback for that KPA500 path. Otherwise fresh eligible KXPA100 telemetry is used, falling back to the radio when unavailable. The KX100 applet always shows the amplifier's own readings. **TX Controls** keeps radio watts/SWR and the radio scale; its RF Power and Tune Pwr readouts show percentages.

<img src="/img/kxpa100/tx-controls-radio-only.png" width="300" alt="Radio-side TX Controls with ATU/MEM, radio power and SWR scales, and percentage power sliders." />

*Current Mac capture while disconnected; no RF or simulated meter values.*

## Using Advanced

Open **Peripherals → Elecraft KXPA100 → Advanced**, or the applet shortcut. Opening the page performs no maintenance action.

### Configuration

Click **Read configuration** before editing. The matrix shows enabled ANT1/ANT2 choices for eleven bands and an **All bands** row. Checkboxes stage edits; **Readback** stays authoritative. Mixed values remain mixed until edited. At least one antenna must remain enabled on every band. **Apply changed fields** sends only deliberate changes and verifies readback. **Cancel edits** discards staging.

### Backup and restore

**Read and save backup…** reads both EEPROM regions, totaling 17,408 bytes, and atomically writes a native `.KXPAConfig` file. **Choose backup…** validates size, coverage and checksum locally and previews identity. Choosing a file sends nothing to the amplifier.

Restore requires firmware 01.39, matching backup serial/firmware, fresh identification, idle state and no RF. Review the preview, check **Confirm backup replacement**, and click **Restore and verify**. Each permitted block is acknowledged and read back; identity and fault-history regions are preserved. Confirmed blocks remain applied if canceled or interrupted. There is no blind retry. Successful restore restarts the device; reconnect and inspect readbacks before use.

### Faults, memories and reset

**Read fault history** shows newest records first, elapsed device seconds, frequency, fault-specific value/unit and raw bytes. Elapsed time is not a calendar timestamp. Unknown records remain visible. **Export CSV…** saves the displayed records.

**Inspect bin** reads stored matches at the selected kHz frequency without changing the live matching network. For erasure, choose band and antenna, review the target, check **Confirm selected erase**, and click **Erase selected memories**. Single-frequency erasure is not offered. **Factory reset** requires its separate confirmation and clears settings and matches, then restarts the unit. These actions never run merely because the device connects.

## Troubleshooting

### A control is disabled

Read its tooltip or open Settings for the reason. Wait for identification and fresh idle measurements. Stop RF before switching antennas, relays or configuration. Tuner-only controls require the optional KXAT100. Advanced schema-dependent tools require firmware 01.39.

### VU/PWR does not show KXPA100 output

Confirm **Use with current radio** was applied to the intended radio, the amplifier reports Operate, and power/idle readbacks are fresh. A higher-priority amplifier or active tuner search prevents KXPA100 metering. A missing physical PA KEY cable cannot be fixed by association.

### Radio association cannot be saved

A rejected settings write leaves the previous association in effect and logs an `aether.kxpa100` warning. Applet **Settings** also displays the failure beside the operation result. Resolve the read-only or busy settings store, then click **Use with current radio** again. Association changes are not retried automatically.

### An operation fails or the connection closes

A timeout closes the stream to prevent a late response from confirming a later request. Reconnect, inspect the current state and make a new deliberate request. Writes are not replayed automatically. A canceled restore can leave some confirmed blocks applied; use the retained backup to recover deliberately.

For a repeated idle disconnect, enable **Settings → Debug → Verbose** and retain the replies around the timeout. SWR replies such as `^SW01.3;` and `^SW013;` both mean 1.3:1 and are supported. The documented zero reply `^SW00.0;` is also valid and does not cause a disconnect.

### ANT1 appears after power-off

The physical off-state routes to ANT1. Reconfirm the desired antenna after power cycling and before transmitting. Do not infer ANT2 from a previous session or saved UI preference.

## See also

- [Peripherals](./peripherals.md)
- [Amplifiers](./amplifiers.md)
- [KAT500 Tuner Control](./kat500-tuner-control.md)
- [Elecraft KXPA100 software](https://elecraft.com/pages/kxpa100-software)
