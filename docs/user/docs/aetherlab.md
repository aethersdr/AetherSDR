---
title: "AetherLab"
slug: "/aetherlab"
description: "Connect a RigExpert Stick Pro over USB for an experimental antenna quick check."
status: "Experimental"
applies_to: ["All radios"]
platforms: ["macOS"]
---

:::info[Status]

**Status:** Experimental · **Applies to:** All radios · **Platforms:** macOS

:::

AetherLab is a USB proof of concept for the **RigExpert Stick Pro**. It opens an
independent antenna analyzer window with an SWR plot and frequency, resistance,
reactance and return-loss marker readouts. Basic USB sweeps are verified on
one Stick Pro identifying as **Stick Pro 108** on macOS. Broader qualification
is pending. The window does not change radio settings or inhibit radio TX.

## Requirements

- A normal-mode Stick Pro connected by a data-capable USB cable.
- A build with USB HID support. Other RigExpert models and bootloader mode are
  excluded; Bluetooth is not implemented.
- AntScope and other analyzer clients closed.
- A known load or antenna physically isolated from any transmitting radio.

:::warning[Protect the analyzer]

The Stick Pro emits measurement RF and must not be connected to a transmitting
radio. AetherLab has no station-wide TX interlock. A successful Stop or USB
close does not confirm RF silence.

:::

## Setup

1. Open **Tools → AetherLab USB Proof of Concept...**.
2. Click **Refresh USB**, select the Stick Pro interface, then **Connect**.
3. Wait for **ready** and the full Stick Pro identity. Firmware identifying as
   **Stick Pro 108** uses a bounded compatibility path for its FULLINFO metadata.
   Connecting only requests
   device information; it does not start a sweep.
4. Set **Start**, **Stop** and **FRX intervals**, and confirm **Analyzer RF port
   is isolated from transmitters**.

## Using AetherLab

Click **Sweep** to acquire one finite trace. The SWR graph displays 1–5; values
above 5 are clipped on the graph but remain intact in the readout and saved
session. Move the marker slider with the keyboard or mouse to inspect each
returned sample, or click **Minimum SWR marker**.

**Stop** retains partial samples. Each sweep attempts an analyzer stop command
and closes USB. Click **Connect** again for another sweep and reconfirm RF
isolation. No measurement restarts automatically. A new Connect clears the previous
samples and protocol trace; save them first if needed.

**Save session…** writes JSON containing raw samples, derived values, status,
USB identity and a bounded protocol trace. Saved files contain device serial
and path information. **Return to radio** closes AetherLab and stops an open
instrument session; it does not issue radio commands.

## Reference

| Control | Meaning |
|---|---|
| Start / Stop | 0.1–600 MHz, entered to 1 Hz. Use an even positive span in Hz. USB frequency stepping is unqualified. |
| FRX intervals | Experimental wire count N, 1–1000. The host expects N+1 returned rows. N=1, 2, 10 and 200 are verified over 14.000–14.350 MHz on the tested Stick Pro 108; other settings remain experimental. |
| R / X | Resistance and reactance in ohms returned by the device; no host calibration applied. |
| SWR / RL | Calculated at 50 Ω. Perfect match gives SWR 1 and infinite return loss; invalid/non-passive derived values are undefined. |
| Session status | Shows ready, setup, sweeping, complete, cancelled or error and the number of accepted rows. Complete means the host's count and endpoint checks passed. |

For testing through MCP, the existing `bridge_command` tool accepts `lab`
commands. Acquisition additionally requires `AETHER_AUTOMATION_ALLOW_LAB_RF=1`
on the app process and an explicit `isolated` request token; radio TX permission
alone does not authorize analyzer RF. See [Automation Bridge and MCP](./automation-bridge-and-mcp.md).

## Known issues

- [RFC #6471](https://github.com/aethersdr/AetherSDR/issues/6471): broader frequency/count
  coverage, timing, cancellation, calibration provenance and measurement accuracy
  require qualification. Physical RF shutdown is unverified. The station tab and
  TX interlock are deferred.

## Troubleshooting

- **No USB devices:** check the data cable, close AntScope, then use **Refresh
  USB**. Only Stick Pro serial prefixes 4600/4601 in normal USB mode are listed.
- **Cannot open:** close another AetherSDR instance or analyzer client. The
  current build may lack HID support, or the selected interface may be wrong.
- **Deadline, count or endpoint error:** save the session trace, reconnect,
  and report the firmware, requested range and returned rows. Partial data is
  retained and never marked complete.
- **Sweep is disabled:** wait for **ready** and confirm RF isolation. A finished
  sweep requires a fresh **Connect**.

### ERROR-20 when starting a sweep

On the tested Stick Pro 108, active front-panel measurement can cause the
analyzer to reject USB setup with **ERROR-20**.

1. Stop the front-panel measurement and return to the analyzer's main menu.
2. Click **Connect** again and wait for **ready**.
3. Reconfirm RF isolation and click **Sweep**.

The app retains the error and closes USB. It does not retry acquisition automatically.

## See also

- [Automation Bridge and MCP](./automation-bridge-and-mcp.md)
- [Before You Transmit](./before-you-transmit.md)
