---
title: "Amplifiers"
slug: "/amplifiers"
description: "AetherSDR controls several linear amplifiers directly, independent of the radio: the ACOM S-series, the SPE Expert 1.3K-FA / 1.5K-FA / 2K-FA, the VK3AMP 600 / 1000 / 2000 W, and the Elecraft KPA1500."
---

AetherSDR controls several linear amplifiers directly, independent of the
radio: the ACOM S-series, the SPE Expert 1.3K-FA / 1.5K-FA / 2K-FA, the VK3AMP
600 / 1000 / 2000 W, and the Elecraft KPA1500. Each is configured in
[Peripherals](./peripherals.md) and gets its own applet in the **Amplifiers** category. Any
combination can be connected at once, alongside a PGXL.

The 4O3A Power Genius XL is relayed by the FlexRadio and has its own applet,
described on [Peripherals](./peripherals.md) (PGXL applet). The TelePost LP-100A wattmeter is
covered at the end of this page.

An amplifier's applet button stays hidden from the applet bar until that
amplifier is configured and present; it then appears on its own.

> **SWR sweeps:** [AetherSweep](./aethersweep.md) only knows how to check a PGXL. Put any of
> the amplifiers on this page into standby or bypass yourself before sweeping.

## Setup

1. Open **Settings → Radio Setup... → Peripherals**.
2. Click **Add** and choose the amplifier.
3. Fill in the connection (see [Reference](#reference)) and click **Connect**.

**Reconnect automatically** on the Peripherals page retries after a dropped
connection.

For serial-capable amplifiers, choose the **Connection Type**, then either
pick the serial port (**Refresh Serial Ports** rescans) or enter the ser2net
host and **TCP Port/Speed**.

## Using the ACOM S-series

Supports the current S-series line: 500S, 600S, 700S, 1200S, 1400S and
2020S. The model is detected from the amplifier, so there is no model
selector.

The **ACOM Amplifier** applet shows:

- A status pill with the amplifier mode (OPERATE, STANDBY, OFF).
- **PWR**, **REF** (reflected) and **SWR** gauges. They rescale with the
  detected model.
- An info grid with temperature (click to switch °C / °F), high voltage,
  drain current, band and the amplifier's total operating time.
- A fault banner while a fault stands, with **CLEAR** to clear it.
- **STANDBY**, **OPERATE** and **OFF** buttons.

## Using the SPE Expert 1.3K-FA / 1.5K-FA / 2K-FA

The amplifier only answers when polled, so AetherSDR polls it every 100 ms.
The model is identified from its status. If the amplifier goes silent behind a
live ser2net link, the applet reports it as off.

**ser2net:** monitoring and control work with the proxy port in raw or telnet
mode. Powering the amplifier **on** over the network drives the proxy's
DTR/RTS lines, which needs an RFC 2217 telnet port
(`accepter: telnet(rfc2217=true),<port>`). A sample configuration is in the
tooltip on the network address field.

The **SPE Expert Amplifier** applet (button **SPE**):

- Gauges for output power (rescaled with the LOW / MID / HIGH level), antenna
  SWR, and the SWR seen before the ATU.
- Temperature, voltage and current readouts, and a warning / alarm banner.
- **ON** (a DTR/RTS power pulse; works over the network with RFC 2217),
  **OFF**, **OPER / STBY**, the power level (click to cycle LOW / MID / HIGH),
  **TUNE** (the ATU), **INPUT** 1/2, **ANT** (cycles the antenna for the
  current band), and drive up / down.

Popped out into its own window, the applet adds a live mirror of the
amplifier's LCD and a **FRONT PANEL** key group: **BAND −** / **BAND +**,
**SET**, and the manual ATU steps **L −** / **L +** / **C −** / **C +**. These
keys work only while the LCD mirror is current; if its data goes stale they
are dimmed, and the display stays visible.

## Using the VK3AMP 600 / 1000 / 2000 W

A network amplifier controlled over TCP, with live telemetry on UDP. There is
no serial option.

In Peripherals, pick the **Amplifier Model** (600 W, 1000 W or 2000 W; default
2000 W). It sets the forward-power gauge's scale.

The **VK3AMP Amplifier** applet:

- A status pill (`CONNECTED` / —).
- **PWR**, **REF** and **SWR** gauges.
- An info grid with temperature, voltage, current, band and antenna.
- A fault banner. Faults are shown as the amplifier's numeric code; there is
  no fault-name table.
- **BYPASS / AMP ON** (the label shows the current state) and **COOLING**
  (cooling override).
- **ANT** 1–3 antenna select.
- **RAIL LOW / HIGH** voltage-rail buttons. They are unavailable while the
  amplifier is bypassed.
- **RESET**, which asks for confirmation and then holds the reset command for
  about ten seconds.

The VK3AMP reads **Reconnect automatically** only at startup.

## Using the Elecraft KPA1500

> **Not yet validated on real hardware.** The KPA1500 support follows
> Elecraft's published programming reference but has not been run against a
> physical amplifier ([#4097](https://github.com/aethersdr/AetherSDR/issues/4097)).
> Reports from owners are welcome.

The **Elecraft KPA1500** applet:

- A status pill, **PWR**, **REF** and **SWR** gauges.
- An info grid with PA temperature, band and the ATU state.
- A fault banner showing the code in hex (for example `Fault B0`) and a
  **CLR FAULT** button.
- **OPERATE / STANDBY**. The label changes only when the amplifier confirms.
- The internal ATU: **ATU IN / ATU BYP**, and **TUNE**, which reads
  `TUNING` while a tune runs; press it again to cancel. A full-search tune
  needs RF, so key the radio's own TUNE: nothing in this applet keys the
  radio.
- **ANT 1** / **ANT 2**. An `ANT n` label appears when an external antenna
  switch reports an antenna number from 3 to 32.

There is no network PTT and no lost-link TX inhibit.

## Using the LP-100A wattmeter

The TelePost LP-100A is a wattmeter, not an amplifier, but it is set up the
same way: **Peripherals → Add → LP-100A Meter**, over a local serial port
(115200 8N1) or a ser2net proxy in raw mode (default TCP 2000).

The **LP-100A Meter** applet is in the **Metering** category and is
**read-only**: it does not change the meter's range, mode or alarm.

- **Power** and **SWR** gauges, plus dBm, return loss, impedance (Z), phase
  and the meter's power range.
- A status pill: `LIVE` (AetherSDR is polling the meter), `SHARED` (another
  program is already polling it; AetherSDR rides along instead of doubling
  the traffic), `NO DATA` (connected but the meter stopped answering) or
  `OFFLINE`.
- The meter reports which range it is on but not its full-scale value. Right
  click the applet to set each range's full scale, or **Reset to defaults**.

## Reference

| Amplifier | Connection | Defaults |
|---|---|---|
| ACOM Amplifier | Serial, or network through a ser2net proxy in raw mode | 9600 8N1; TCP 7000 |
| SPE Expert Amplifier | Serial, or network through ser2net (raw or telnet) | 115200 8N1; TCP 7000 |
| VK3AMP Amplifier | Network only: TCP control, UDP telemetry | TCP 5005; UDP 5010 |
| Elecraft KPA1500 | Network only: TCP | TCP 1500 |
| LP-100A Meter | Serial, or network through a ser2net proxy in raw mode | 115200 8N1; TCP 2000 |

## Known issues

- An SPE amplifier set to connect at startup shows connected but no data until you disconnect and reconnect it ([#4893](https://github.com/aethersdr/AetherSDR/issues/4893)).

## Troubleshooting

### The SPE amplifier will not power on over the network

Powering on drives the ser2net proxy's DTR/RTS lines, which a plain raw or
telnet port cannot do.

1. Configure the ser2net port as an RFC 2217 telnet port:
   `accepter: telnet(rfc2217=true),<port>`. The tooltip on the network address
   field has a sample configuration.
2. Reconnect the amplifier in **Peripherals** and press **ON**.

### The VK3AMP takes a long time to connect on Windows

After the amplifier has been idle, the first connection can take 9–12
seconds, because the amplifier answers only broadcast ARP.

1. Wait: AetherSDR waits up to 18 seconds before trying again.

### The SPE front-panel keys are dimmed

The popped-out **FRONT PANEL** keys work only while the LCD mirror is current.
Its data has gone stale.

1. Check the amplifier's connection in **Peripherals**.
2. Wait for the LCD mirror to update; the keys come back with it.

## See also

- [Peripherals](./peripherals.md)
- [TGXL Tuner Control](./tgxl-tuner-control.md)
- [Meters](./meters.md)
- [AetherSweep](./aethersweep.md)
