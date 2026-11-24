---
title: "AetherLab"
slug: "/aetherlab"
description: "Connect a RigExpert Stick 230, 500, Pro or XPro over USB for an experimental antenna quick check."
status: "Experimental"
applies_to: ["All radios"]
platforms: ["macOS"]
---

:::info[Status]

**Status:** Experimental · **Applies to:** All radios · **Platforms:** macOS

:::

AetherLab is a USB proof of concept for **RigExpert Stick** analyzers. It opens an
antenna analyzer workspace in the main window, with an SWR plot, marker
readouts and separate series/parallel impedance models. Basic USB sweeps are verified on
one Stick Pro identifying as **Stick Pro 108** on macOS. Broader qualification
is pending. The workspace retains your radio session and does not inhibit radio TX.

**Stick 230**, **Stick 500** and **Stick XPro** are **Experimental — hardware
testing pending**. They share the USB command path, with a separate frequency
limit for each model. Their device entries and model details show the experimental
label. Software tests do not establish compatibility with their actual firmware.

## Requirements

- A normal-mode Stick 230, Stick 500, Stick Pro or Stick XPro connected by a data-capable USB cable.
- A build with USB HID support. Other RigExpert families and bootloader mode are
  excluded; Bluetooth is not implemented.
- AntScope and other analyzer clients closed.
- A known load or antenna physically isolated from any transmitting radio.

:::warning[Protect the analyzer]

The analyzer emits measurement RF and must not be connected to a transmitting
radio. AetherLab has no station-wide TX interlock. A successful Stop or USB
close does not confirm RF silence.

:::

## Setup

1. Open the **+** radio switcher and choose **AetherLab**, below **Rescan radios**.
   **Tools → AetherLab…** opens the same workspace.
2. Click **Refresh USB**, select your analyzer interface, then **Connect**.
3. Wait for **Ready** and the full analyzer identity. The identity must match
   the model detected from USB. Firmware identifying as
   **Stick Pro 108** uses a bounded compatibility path for its FULLINFO metadata.
   Connecting only requests
   device information; it does not start a sweep.
4. Set **Start**, **Stop** and **Intervals**, and confirm **Analyzer RF port
   is isolated from transmitters**.

## Using AetherLab

The **Antenna Quick Check** header places the yellow **Radio TX is not
inhibited** badge beside **Return to radio**. The badge is a warning, not a TX
interlock. RF-isolation guidance remains in the device group. Device status uses
capitalized labels such as **Complete · Stick Pro 108**; automation state tokens
remain lower-case.

Click **Run sweep** to acquire one finite trace. The SWR graph displays 1–5; values
above 5 are clipped on the graph but remain intact in the readout and saved
session. Move the marker slider with the keyboard or mouse to inspect each
returned sample, or click **Minimum SWR marker**.

**Stop** retains partial samples. Each sweep attempts an analyzer stop command
and closes USB. Click **Connect** again for another sweep and reconfirm RF
isolation. No measurement restarts automatically. A new Connect clears the previous
samples and protocol trace; save them first if needed.

**Save session…** writes JSON containing raw samples, derived values, status,
USB identity, model, experimental status, frequency limits and a bounded
protocol trace. Saved files contain device serial and path information. **Return to radio**, or selecting a radio tab, stops an open instrument
session and restores the radio workspace. The AetherLab tab and last trace
remain available. Opening the tab again does not reconnect or sweep. The radio
connection, tuning and audio session are retained; restore the antenna path
manually before transmitting. Minimal mode returns to the radio workspace and uses the full applet area,
even after visiting AetherLab. Opening AetherLab dismisses the radio connection
picker; reopen it from the radio switcher when needed.

Screen readers can read the current marker frequency, each measurement with
its units, and the current device status after acquisition or marker movement.

The layout places the plot beside the marker panel in wide windows and stacks
them in narrower windows. Scroll vertically when the window is short. L and C
are signed equivalents at the marker frequency, not simultaneous physical
components; undefined values have no finite result at that operating point.

## Reference

| Model | Start / Stop limits | USB serial prefixes | Qualification |
|---|---|---|---|
| Stick 230 | 0.1–230 MHz | 4230 / 4231 | Experimental — hardware testing pending |
| Stick 500 | 0.1–500 MHz | 4500 / 4501 | Experimental — hardware testing pending |
| Stick Pro | 0.1–600 MHz | 4600 / 4601 | Basic USB sweeps verified on one Stick Pro 108 on macOS; broader qualification pending |
| Stick XPro | 0.1–1000 MHz | 4999 / 4998 | Experimental — hardware testing pending |

| Control | Meaning |
|---|---|
| Start / Stop | Uses the selected model's limits above, entered to 1 Hz. Selecting a model with a lower limit clamps the editors to that limit. Use an even positive span in Hz. USB frequency stepping is unqualified. |
| Intervals | Experimental wire count N, 1–1000. The host expects N+1 returned rows. N=1, 2, 10 and 200 are verified over 14.000–14.350 MHz on the tested Stick Pro 108; other settings remain experimental. |
| R / X | Resistance and reactance in ohms returned by the device; no host calibration applied. |
| SWR / RL | Calculated at 50 Ω. Perfect match gives SWR 1 and infinite return loss; invalid/non-passive derived values are undefined. |
| &#124;Z&#124; / Phase ∠Z | Impedance magnitude and phase derived from device R/X. |
| Magnitude &#124;S11&#124; / Phase ∠S11 | Reflection magnitude and phase at 50 Ω. Reflection phase is undefined for a perfect match. |
| Series model | Device R/X and signed equivalents L = X / (2πf), C = −1 / (2πfX). |
| Parallel model | Equivalent R/X from admittance 1/Z, and their signed L/C equivalents. Engineering units distinguish nH from µH and pF from nF. |
| Session status | Shows Ready, setup, Sweeping, Complete, Cancelled or Error and the number of accepted rows. Complete means the host's count and endpoint checks passed. |

For testing through MCP, the existing `bridge_command` tool accepts `lab`
commands. Acquisition additionally requires `AETHER_AUTOMATION_ALLOW_LAB_RF=1`
on the app process and an explicit `isolated` request token; radio TX permission
alone does not authorize analyzer RF. See [Automation Bridge and MCP](./automation-bridge-and-mcp.md).

## Known issues

- [RFC #6471](https://github.com/aethersdr/AetherSDR/issues/6471): broader frequency/count
  coverage, timing, cancellation, calibration provenance and measurement accuracy
  require qualification. Physical RF shutdown is unverified; a station TX
  interlock is not implemented.

## Troubleshooting

### No USB devices are listed

The cable carries power only, another program holds the analyzer, or the
analyzer is not in normal USB mode. Only the Stick model serial prefixes in
the table above are listed in normal USB mode.

1. Check that the cable is a data cable.
2. Close AntScope and any other analyzer client.
3. Click **Refresh USB**.

### The analyzer will not open

Another AetherSDR instance or analyzer client holds the device, this build
lacks HID support, or the wrong interface is selected.

1. Close other AetherSDR instances and analyzer clients.
2. Select your analyzer in the interface list and click **Connect** again.

### A sweep stops with a deadline, count or endpoint error

The analyzer did not return the requested rows in time or in order. Partial
data is retained and never marked complete.

1. Click **Save session…** to keep the protocol trace.
2. Click **Connect** again.
3. Report the firmware, the requested range and the returned rows.

### Run sweep is disabled

The analyzer is not **Ready**, RF isolation is not confirmed, or the previous
sweep has finished.

1. After a finished sweep, click **Connect** again.
2. Wait for **Ready**, then confirm RF isolation.

### ERROR-20 when starting a sweep

On the tested Stick Pro 108, active front-panel measurement can cause the
analyzer to reject USB setup with **ERROR-20**.

1. Stop the front-panel measurement and return to the analyzer's main menu.
2. Click **Connect** again and wait for **Ready**.
3. Reconfirm RF isolation and click **Run sweep**.

The app retains the error and closes USB. It does not retry acquisition automatically.

## See also

- [Automation Bridge and MCP](./automation-bridge-and-mcp.md)
- [Before You Transmit](./before-you-transmit.md)
