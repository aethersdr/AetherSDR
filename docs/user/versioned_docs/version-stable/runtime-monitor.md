---
title: "Runtime Monitor"
slug: "/runtime-monitor"
description: "The same group at the bottom of the Tools menu has two related diagnostic windows, Radio Health... and Network Diagnostics..., described further down."
---

**Tools → Runtime Monitor...** shows how AetherSDR itself is behaving on your computer: CPU use, the busiest threads, memory and how responsive the user interface is. Use it when the app feels sluggish, audio stutters, or the panadapter stalls, to see whether the cause is on the computer rather than the radio or the network.

The same group at the bottom of the **Tools** menu has two related diagnostic windows, **Radio Health...** and **Network Diagnostics...**, described further down.

> **Upgrading from an older version:** this window was called **System Info**.

## Using the Runtime Monitor

A **Timeframe** selector in the window header (1 minute, 5 minutes (default), 15 minutes or 1 hour) applies to every chart. It is hidden on the Threads and Logs tabs, which have no adjustable time axis.

### Overview

Four cards across the top, each with a chart below:

| Card | Meaning |
|------|---------|
| **CPU Total** | AetherSDR's share of the whole machine |
| **Max Thread** | The busiest single thread's share of **one** core, and its name |
| **Memory** | Resident memory the OS currently holds for AetherSDR |
| **GUI Tick Lag** | How late the user-interface thread's 50 ms heartbeat fired, at worst, during the last sample. Zero is on time; a large value means the interface was busy for that long. |

The charts show CPU (process and busiest thread), memory (resident and peak), the top five threads, and GUI tick lag (worst and mean) over the chosen timeframe.

**Why Max Thread matters:** AetherSDR's typical stall is one thread pinned at 100 % of one core while the others idle. The machine-wide CPU figure (and the status-bar CPU readout) can look low while that happens. Max Thread shows it directly.

### Threads

A live table of every AetherSDR thread with **Thread**, **TID**, **State**, **CPU %** (share of one core), **Peak 60 s**, **Total CPU (s)** and a **Last 60 s** sparkline. The summary line above the table turns red when any thread crosses 90 % of a core.

Windows does not report per-thread state, so the State column shows a dash there.

### Memory

Resident, peak resident, private memory and virtual address space, with a chart of resident, private and peak memory over the timeframe. Virtual address space is reserved space, not memory in use, so a large figure there is normal. Click a legend entry to show one series alone.

### Logs

A live tail of the current log file, filtered to the three performance-related categories: **Performance**, **Render** and **Audio**. The file path is shown above the viewer.

- **Live** follows new lines as they arrive. Scrolling up pauses following so you can read around a stall; press **Live** again to resume.
- A category that is switched off in **Help → Support & Diagnostics...** is shown dimmed. Only its warnings and errors reach the log until you turn it on there.

To capture more detail, turn on the **Performance** and **Render** categories in **Help → Support & Diagnostics...** before reproducing the problem. See [Support and Logging](./support-and-logging.md).

## Using Radio Health

**Tools → Radio Health...** lists the health and status registers that the connected radio itself reports, refreshed every 500 ms. A dash means the radio has not reported that value, which is not the same as zero. **Copy** puts the whole table on the clipboard as text for a bug report.

What appears depends on the radio:

- **FlexRadio:** Flex radios do not publish health registers through this window, so it reads "This radio reports no health registers." Use the **MTR** (Radio Vitals) applet for PA temperature, fan speed and supply voltage. See [Meters](./meters.md).
- **[Hermes-Lite 2](./hermes-lite-2.md):** link, temperature, supply, PA current and gateware, a transmit voice-chain section (mic level and gain, ALC, forward power) and converter (ADC) rows. Some rows, such as whether another client is holding the radio, whether it is reachable and its PA temperature, are available even before you connect.
- **[Networked Icom](./networked-icom.md):** includes the DATA OFF MOD and DATA MOD input selections and the MIC/USB/ACC/WLAN/LAN levels.
- **[RTL-SDR](./rtl-sdr.md):** the capture window (centre, rate and edges) and receive-pipeline counters such as queue drops and late frames.

Radio Health is a different thing from the **HLTH** (Antenna Health) applet, which trends SWR and forward power for your antenna system.

## Using Network Diagnostics

**Tools → Network Diagnostics...** shows the health of the link between AetherSDR and the radio. A **Connect by IP** connection also has a **Network Diagnostics** button beside the connect button.

Pages are grouped in a tree on the left:

| Group | Pages |
|-------|-------|
| **STATUS** | **Overview** (status, latency, packet loss and audio-buffer cards with summary charts), **Connection Details** |
| **TRENDS** | **Latency & Jitter**, **Stream Rates** (including the *Adaptive Throttle — FPS Cap* chart when the throttle is on), **Packet Loss**, **Audio Health** (RX audio buffer and timing, per-stream delivery table), **Digital Voice** (when a digital-voice waveform is active) |
| **SUPPORT** | **Application Logs** (category filters, Live/Paused), **TCI Clients** (when the TCI server is running: connected clients, their likely role, and a TCI message monitor you can pause, clear, filter and save) |

- The chart **Timeframe** offers 1 minute, 5 minutes, 15 minutes, 1 hour, 1 day and 1 week.
- **Search diagnostics** (Ctrl+F, ⌘F on macOS) filters the pages by symptom, stream, protocol or measurement. Try *jitter*, *underrun*, *UDP*, *packet gaps*, *bitrate* or *WSJT-X*.

For what to do about a poor link, see [Low Bandwidth Connections](./low-bandwidth-connections.md).

## Troubleshooting

### AetherSDR is sluggish but CPU use looks low

One thread is pinned at 100 % of one core while the others idle, so the machine-wide figure stays low.

1. Open **Tools → Runtime Monitor...** and read the **Max Thread** card, which names the busiest thread.
2. Check **GUI Tick Lag**. A large value means the interface thread was busy for that long.
3. Turn on the **Performance** and **Render** log categories in **Help → Support & Diagnostics...**, reproduce the stall, and attach the log to your report. See [Support and Logging](./support-and-logging.md).

### Radio Health says "This radio reports no health registers"

You are connected to a FlexRadio, which does not publish health registers through this window.

1. Open the **MTR** (Radio Vitals) applet for PA temperature, fan speed and supply voltage. See [Meters](./meters.md).

### A category in the Logs tab is dimmed

That log category is switched off, so only its warnings and errors reach the log.

1. Open **Help → Support & Diagnostics...** and tick the category.
2. Restart AetherSDR so the category is on from startup, then reproduce the problem.

## See also

- [Support and Logging](./support-and-logging.md): log categories, the support bundle and File an Issue
- [Low Bandwidth Connections](./low-bandwidth-connections.md)
- [GPU Rendering](./gpu-rendering.md): renderer details also appear in **Help → About AetherSDR**
- [Troubleshooting](./troubleshooting.md)
