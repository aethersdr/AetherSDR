---
title: "USB Cable Management"
slug: "/usb-cable-management"
description: "AetherSDR can configure USB-serial adapters plugged into the FlexRadio's rear-panel USB ports."
---

> **Applies to:** FlexRadio

AetherSDR can configure USB-serial adapters plugged into the FlexRadio's rear-panel USB ports. These cables provide CAT control, band decoder output, per-bit switching, LDPA amplifier control and raw serial passthrough to external devices, all managed remotely over the network.

The radio detects USB-serial adapters on its rear USB ports and reports them in status messages. AetherSDR lists them on the **USB Cables** page of Radio Setup, where you set each cable's type, serial parameters and behaviour.

These are the **radio's** USB ports, not your computer's. For USB-serial PTT/CW keying on your computer, see **Settings → Radio Setup... → Serial & Controllers** ([USB Control Surfaces](./usb-control-surfaces.md)). For accessories AetherSDR talks to directly, see [Peripherals](./peripherals.md).

## Setup

Open the **USB Cables** page:

- **Settings → USB Cables...** opens Radio Setup directly on the page.
- Or open **Settings → Radio Setup...** and pick **USB Cables** under CONTROLLERS & HARDWARE.

### Cable list

The **Cables** list on the left shows every cable the radio reports, with its type in brackets (for example `[BCD]`) and "(unplugged)" when it is not plugged in:

| Colour | Meaning |
|-------|---------|
| **Green** | Enabled and plugged in |
| **Yellow** | Enabled but unplugged |
| **Grey** | Disabled |

Cables appear and disappear automatically as the radio reports them. Click a cable to edit it on the right. Every cable page starts with its name, an **Enabled** checkbox, its plugged-in status, and the **Cable Type** selector.

### Choosing a cable type

**Cable Type** chooses the cable's protocol: **CAT**, **Bit**, **BCD**, **LDPA** or **Passthrough**. Changing it reconfigures the cable on the radio, so the mouse wheel cannot change it by accident: click and choose.

A freshly plugged cable with no type yet opens the **Unconfigured** page: "Select a cable type to configure this device." Pick a type to configure it, or use **Remove This Cable** to remove it from the radio.

Each cable keeps its own serial settings. Cable configuration is stored in the radio, not in AetherSDR.

## Using the cable types

- **Amplifier tracking:** a CAT cable sends frequency to your amplifier so it follows band changes.
- **Antenna switching:** a BCD cable drives a remote antenna switch.
- **Bandpass filters:** a Bit cable drives per-band relays on a filter bank.
- **VHF amplifier:** an LDPA cable sets a 2m/4m LDPA amplifier's band and preamp.
- **Remote serial devices:** a Passthrough cable tunnels serial data to a device at the radio site.

## Reference

### CAT

Sends CAT (Computer Aided Transceiver) frequency and mode data over the serial port. Amplifiers, antenna tuners and logging software plugged into the radio's USB port follow the radio.

- **Serial parameters:** Speed, Data Bits (7/8), Parity, Stop Bits (1/2), Flow (none, RTS/CTS, DTR/DSR, XON/XOFF)
- **Source:** which frequency to report: None, TX Pan, TX Slice, Active Slice, TX Ant, RX Ant or Ordinal Slice
- **Auto Report:** send updates automatically when frequency or mode changes

### BCD

Outputs Binary Coded Decimal band data for external band decoders: antenna switches, bandpass filter banks and amplifier band switches.

- **BCD Type:** HF (bcd), VHF (vbcd) or HF+VHF (bcd_vbcd)
- **Polarity:** Active High or Active Low
- **Source:** which frequency determines the band output

### Bit

Eight independent output bits (Bit 0–7) for per-band or per-frequency switching. Pick a bit in the **Bits** list and set it in **Bit Settings**:

- **Enabled**
- **Source**, and **Antenna/Slice** when the source needs one
- **Output:** `band` (matches a band) or `freq_range` (matches a frequency range)
- **Band** (for `band`), or **Low Freq (MHz)** and **High Freq (MHz)** (for `freq_range`)
- **Polarity:** High or Low
- **PTT Dependent:** the bit follows transmit
- **PTT Delay** and **TX Delay:** 0–10000 ms

### LDPA

Controls an LDPA amplifier on 2m or 4m.

- **Band:** 2m or 4m
- **Preamp:** on or off
- **Source:** which frequency the amplifier follows

### Passthrough

A raw serial tunnel: bytes sent to one end come out the other. Used to talk directly to a device plugged into the radio's USB port, tunnelled over the network to your client.

- **Serial parameters:** Speed, Data Bits, Parity, Stop Bits, Flow

## Known issues

- A CAT cable with **Source** set to TX Ant or RX Ant has no choice of which antenna port to follow; only Bit cables offer **Antenna/Slice** ([#4816](https://github.com/aethersdr/AetherSDR/issues/4816)).

## Troubleshooting

### A new cable shows the Unconfigured page

The radio has detected the adapter but it has no cable type yet.

1. Select the cable in the **Cables** list.
2. Pick a **Cable Type**, then set its serial parameters and source.

### A cable is listed in yellow

The cable is enabled but not plugged in to the radio.

1. Check the adapter is plugged in to the radio's rear USB port, not your computer.
2. The entry turns green when the radio reports it plugged in.

## See also

- [Peripherals](./peripherals.md)
- [USB Control Surfaces](./usb-control-surfaces.md)
- [ShackSwitch](./shackswitch.md)
- [Radio Setup](./radio-setup.md)
- [FlexRadio](./flexradio.md)
