---
title: "Supported Radios"
slug: "/supported-radios"
description: "AetherSDR is a client for FlexRadio transceivers, and FlexRadio is the supported target."
---

AetherSDR is a client for **FlexRadio** transceivers, and FlexRadio is the supported target. It also drives several other radio families through a vendor-neutral backend layer. Those families are newer, and each is labelled with its status below.

## Where to go next

- **FlexRadio:** [FlexRadio](./flexradio.md), then [First Connection](./first-connection.md) and [Your First Session](./your-first-session.md).
- **Another radio family:** [Hermes-Lite 2](./hermes-lite-2.md), [Networked Icom](./networked-icom.md), [ANAN-G2](./anan-g2.md), [RTL-SDR](./rtl-sdr.md) or [KiwiSDR and Web-888](./kiwisdr-and-web-888.md).
- **No radio:** [Demo Mode](./demo-mode.md).

## FlexRadio (supported)

Works with any FlexRadio transceiver, including:

- **FLEX-6000 series:** FLEX-6300, FLEX-6400, FLEX-6400M, FLEX-6500, FLEX-6600, FLEX-6600M, FLEX-6700
- **FLEX-8000 series:** FLEX-8400, FLEX-8400M, FLEX-8600, FLEX-8600M
- **Aurora series:** AU-510, AU-510M, AU-520, AU-520M
- ML-, CL- and RT-series devices

**Firmware:** the active test target is FLEX-8600 firmware 4.2.18 (SmartSDR protocol v1.4.0.0). Earlier 4.x firmware works; **v3.x is unsupported.** See [Firmware Update](./firmware-update.md).

The rest of this documentation describes FlexRadio operation unless a page says otherwise. Start with [First Connection](./first-connection.md).

## Other Radio Families

| Family | Status | Connects by | Page |
|--------|--------|-------------|------|
| Hermes-Lite 2 | **Experimental** | LAN discovery, or Connect by IP | [Hermes-Lite 2](./hermes-lite-2.md) |
| Networked Icom (IC-705, IC-7300MK2, IC-9700 …) | **Early**; the **IC-7300MK2 is supported** | Connect by IP | [Networked Icom](./networked-icom.md) |
| ANAN-G2 | **Experimental, receive-only** | LAN discovery, or Connect by IP | [ANAN-G2](./anan-g2.md) |
| RTL-SDR USB dongles | **Experimental, receive-only** | USB discovery (builds with the RTL-SDR backend) | [RTL-SDR](./rtl-sdr.md) |
| KiwiSDR and Web-888 | Public receivers used as receive-only antennas for a slice | Radio Setup → Antennas | [KiwiSDR and Web-888](./kiwisdr-and-web-888.md) |
| Demo | Built-in simulator, cannot transmit | The Connect to Radio list | [Demo Mode](./demo-mode.md) |

None of the other families is a supported family yet. The IC-7300MK2 over its built-in Ethernet (RS-BA1) is the one non-Flex model supported on its own; every other Icom model keeps the early, experimental treatment.

While a Hermes-Lite 2, an ANAN-G2 or an Icom other than the IC-7300MK2 is connected, an **EXPERIMENTAL** badge shows in the title bar, and an "Experimental radio support" notice appears when you connect. Tick "Don't show again" in the notice to stop it for that family.

## Setup

How you reach each family from the **Connect to Radio** window (**File → Connect to Radio...**):

- **On This Network** lists FlexRadio, Hermes-Lite 2 and ANAN-G2 radios on your LAN, RTL-SDR dongles on USB, and the demo.
- **Connect by IP** has a **Radio type:** list (FlexRadio, Hermes-Lite 2, ANAN-G2, Icom (network), RTL-SDR (USB)). A networked Icom is always reached this way. See [Manual Connection](./manual-connection.md).
- **KiwiSDR and Web-888** receivers are configured under **Tools → Configure KiwiSDR...** and chosen as a slice's RX antenna.

## Reference

### What works where

This table lists only what has been confirmed for each family. A **?** means not yet confirmed for that family, not that it is known to work or fail.

| | FlexRadio | Hermes-Lite 2 | Networked Icom | ANAN-G2 | RTL-SDR | Demo |
|---|---|---|---|---|---|---|
| **Transmit** | Yes | Yes (SSB voice, CW, data) | Yes on recognised models; none on an unrecognised model | No | No | No |
| **Receivers / slices** | As reported by the radio | Up to 4 (A–D) | Per model | 1 | 1 | 1 |
| **Panadapters** | As reported by the radio | One per receiver | 1 | 1 | 1 | ? |
| **DAX virtual audio and IQ** | Yes | No | No | No | No | No |
| **TCI server** | Yes | Yes | Yes (receive audio, FT8, WSPR) | ? | ? | ? |
| **CAT (rigctld)** | Yes | Yes | ? | ? | ? | ? |
| **SmartLink** | Yes | No | No | No | No | No |
| **Multi-Flex (several clients)** | Yes | No | No | No | No | No |
| **Firmware update from AetherSDR** | Yes | No | No | No | No | No |
| **Memory channels** | On the radio | On this computer | On this computer, with import from the radio | On this computer | On this computer | On this computer |
| **Radio Health dialog** | Yes | Yes | Yes | Yes | Yes | ? |

Notes:
- **Receivers on the HL2:** up to four receivers at spans up to 192 kHz, three at 384 kHz. The span is one value for the whole board.
- **Icom panadapters:** the IC-9700, IC-7610 and IC-785x offer one panadapter, the single scope stream AetherSDR implements.
- **Memory channels:** a FlexRadio keeps its memories. Every other family uses one shared bank stored on this computer. On an Icom whose model supports it, the radio's own memories can be read into that bank; the IC-9700's radio memories are read-only. See [Memory Channels](./memory-channels.md).

### Controls on non-Flex radios

On a Hermes-Lite 2, ANAN-G2, RTL-SDR or Icom, every control does one of three things:

1. **Works** through the radio's own route.
2. **Is dimmed with its reason** in the tooltip and the screen-reader description. For example, the title-bar headphone controls say "this radio has no headphone output".
3. **Refuses with a warning** and a once-per-session notice before anything is sent. This covers, for example, picking an antenna the radio does not publish, WFM, DAX TX, and split or CWX controller bindings.

FlexRadio-only surfaces, such as the Profile Manager, DAX, SmartLink and the ATU chain, are hidden on other radios. GPS controls appear only when the radio reports GPS hardware (a Flex with the GPSDO option, or the IC-705).

Radio Setup gains a page for some families: **Calibration** and **HL2 Hardware** (Hermes-Lite 2), **RTL Receiver** (RTL-SDR) and **Droop Correction** (ANAN-G2).

### Peripherals

Supported external devices include the 4O3A/FlexRadio PGXL power amplifier and TGXL tuner, plus ACOM S-series, SPE Expert and VK3AMP amplifiers. See [Peripherals](./peripherals.md) and [Amplifiers](./amplifiers.md).

## See also

- [FlexRadio](./flexradio.md)
- [First Connection](./first-connection.md)
- [Manual Connection](./manual-connection.md)
- [Demo Mode](./demo-mode.md)
- [Peripherals](./peripherals.md)
- [README: Supported Hardware](https://github.com/aethersdr/AetherSDR#supported-hardware)
