---
title: "Radio Health Tools"
slug: "/radio-health-tools"
description: "An optional 110 KB tool on a FlexRadio that shows its free waveform storage, memory and CPU at the bottom of the Waveforms window."
status: "Experimental"
applies_to: ["FlexRadio"]
platforms: ["Linux", "macOS", "Windows"]
---

:::info[Status]

**Status:** Experimental · **Applies to:** FlexRadio · **Platforms:** Linux, macOS, Windows

:::

Radio health tools show how much room a FlexRadio has left for waveforms,
and how busy it is, at the bottom of the **Waveforms** window. On a
FLEX-6000 every legacy waveform shares a small partition (19 MB on a
FLEX-6400 or 6700), so free space decides what else fits.

The tools are a small program that AetherSDR installs on the radio as a
legacy waveform. They take about 110 KB on the radio. They don't depend on
any other waveform or container, and they don't change any radio setting.

## Requirements

- A FlexRadio that installs waveforms: a FLEX-6000 or FLEX-8000 series or
  Aurora radio.
- AetherSDR connected to the radio over its **local network**. The readout
  doesn't work over SmartLink or a tailnet.

It has been run on a FLEX-8600 (firmware 4.3.16). The FLEX-6000 build has
not yet run on a radio.

## Setup

1. Open **Tools → Waveforms...**.
2. At the bottom of the window, tick **Install radio health tools**.

AetherSDR installs the tools in a few seconds, with no download (they ship
inside AetherSDR), and starts showing readings.

## Using the radio health tools

The bar at the bottom of the **Waveforms** window reads, for example:

`Storage 824 MB free   Memory 1.7 GB free · CPU 91% idle`

- **Storage** is the free space where new apps go: the container store on a
  FLEX-8000 or Aurora, the shared waveform partition on a FLEX-6000.
- **Memory** is the radio's available memory. A FLEX-6000 doesn't report
  this directly, so AetherSDR shows an estimate: **Memory about … free**.
- **CPU** is the share of the radio's processor that is idle, across all
  its cores.

The readings refresh every 5 seconds while the window is open. Hover over
them for each storage area's free and total space, the number of cores, and
the tools' version.

The tools don't appear in the **Installed Waveforms** list: the checkbox is
their only control. Other programs, such as SmartSDR, still list them as
**AetherHealth**.

## Removing them

Untick **Install radio health tools**. AetherSDR uninstalls them from the
radio.

## Troubleshooting

- **The checkbox is grayed out:** the bar says why. Connect to the radio
  over its local network, not SmartLink or a tailnet.
- **"Waiting for the radio health tools to answer…" stays:** the tools start
  a few seconds after they're installed. If the message stays, check that
  nothing on your network blocks TCP port 48994 to the radio.

## See also

- [Menu Reference](./menu-reference.md)
- [Tailscale Remote Access](./tailscale-remote-access.md)
