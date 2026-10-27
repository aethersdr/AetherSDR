---
title: "Amplifiers"
slug: "/amplifiers"
description: "AetherSDR controls several linear amplifiers directly, independent of the radio: the ACOM S-series, the SPE Expert 1.3K-FA / 1.5K-FA / 2K-FA and the original 1K-FA, the VK3AMP 600 / 1000 / 2000 W, the Elecraft KPA500 / KPA1500, and the RF-KIT."
---

AetherSDR controls several linear amplifiers directly, independent of the
radio: the ACOM S-series, the SPE Expert 1.3K-FA / 1.5K-FA / 2K-FA and the
original 1K-FA, the VK3AMP 600 / 1000 / 2000 W, the Elecraft KPA500 / KPA1500, and the RF-KIT. Each is configured in
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

In Peripherals, leave the **Amplifier Model** on **1.3K / 1.5K / 2K-FA** (the
default). For the original 1K-FA, see
[Using the original SPE Expert 1K-FA](#using-the-original-spe-expert-1k-fa).

The amplifier only answers when polled, so AetherSDR polls it every 100 ms.
The exact model is identified from its status. If the amplifier goes silent
behind a live ser2net link, the applet reports it as off.

**ser2net:** monitoring and control work with the proxy port in raw or telnet
mode. Powering the amplifier **on** over the network drives the proxy's
DTR/RTS lines, which needs an RFC 2217 telnet port
(`accepter: telnet(rfc2217=true),<port>`). A sample configuration is in the
tooltip on the network address field.

The **SPE Expert Amplifier** applet (button **SPE**):

- Gauges for output power (rescaled with the LOW / MID / HIGH level), antenna
  SWR, and the SWR seen before the ATU.
- Temperature, voltage and current readouts, and a warning / alarm banner.
- **ON** (a DTR/RTS power pulse; works over the network with RFC 2217; if the
  network bridge cannot switch the lines, a note under the buttons says so),
  **OFF**, **OPER / STBY**, the power level (click to cycle LOW / MID / HIGH),
  **TUNE** (the ATU), **INPUT** 1/2, **ANT** (cycles the antenna for the
  current band), and drive up / down.

Popped out into its own window, the applet adds a live mirror of the
amplifier's LCD and a **FRONT PANEL** key group: **BAND −** / **BAND +**,
**SET**, and the manual ATU steps **L −** / **L +** / **C −** / **C +**. These
keys work only while the LCD mirror is current; if its data goes stale they
are dimmed, and the display stays visible.

## Using the original SPE Expert 1K-FA

The original 1K-FA (the model with only an RS-232 port, not the newer 1K-FA
"Taurus") speaks an older protocol at **9600 8N1** and cannot be detected, so
you choose it: in Peripherals, set **Amplifier Model** to **1K-FA (original,
RS-232)**. The fixed serial settings shown next to it change to 9600 8N1. For
a ser2net proxy, set the proxy's serial side to 9600 baud; raw mode is
preferred, because the 1K-FA's binary status loses an occasional frame in
telnet mode.

The **SPE Expert Amplifier** applet works as for the newer models, with these
differences:

- The amplifier streams its status on its own, 5–8 times a second, once
  AetherSDR connects.
- The power level reads **HALF** / **FULL**; clicking it toggles between them.
- The antenna SWR is estimated from forward and reflected power while the
  amplifier is in OPERATE, and is shown as **SWR ≈**. In STANDBY it is the
  value the amplifier reports.
- There is no SWR before the ATU and no LCD mirror, so that gauge row is
  dimmed, the popped-out display says so, and the **FRONT PANEL** keys stay
  dimmed. Hover over a dimmed control for the reason.

### Power on and off

The 1K-FA stays powered while the serial port's DTR line is held high, and
AetherSDR holds it only while it is connected:

- **ON** raises DTR. The amplifier boots in a few seconds.
- **OFF** sends the amplifier's OFF key and then releases DTR.
- **Disconnect**, quitting AetherSDR, or losing the serial link releases DTR,
  and an amplifier that **ON** switched on powers off. Reconnecting, by hand
  or with **Reconnect automatically**, does not switch it back on; press
  **ON** again.
- In the Windows bench, an amplifier switched on at its own front panel
  stayed on when connecting. The final line policy still needs bench
  verification, including the brief DTR pulse during serial open on macOS
  and Linux.
- While AetherSDR holds DTR, the amplifier refuses its own front-panel power
  switch. Use **OFF** in the applet.

Avoid disconnecting or quitting while transmitting: the amplifier then powers
off mid-transmission.

Over the network, **ON** and **OFF** need ser2net with RFC 2217
(`accepter: telnet(rfc2217=true),<port>`) and a serial adapter that has a DTR
line. A plain serial-to-Ethernet converter only passes data: monitoring and
the keys work, but **ON** cannot, so switch the amplifier on at its front
panel.

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

## Using the Elecraft KPA500

Choose **Add → Elecraft KPA500** in **Peripherals**, then choose **Connection Type**:

- **Serial:** choose the local port and speed in **TCP Port/Speed**. Supported
  rates are 4800, 9600, 19200 and 38400 baud, using 8N1 with no handshake.
  Close any other program using that port first.
- **Network:** enter a ser2net host and raw TCP port (initial value 2000).
  Configure 8N1, no flow control, and the amplifier's baud rate on the server.
  The local serial speed does not configure the remote port. Use a trusted LAN
  or VPN; this raw connection has no authentication or encryption.

Click **Connect**. The device list shows **DIRECT** for a connected serial port,
**NETWORK** for a connected TCP bridge, and **OFFLINE** when disconnected.
**Cancel** stops a pending network connection. Disconnect
before editing the endpoint. **Connect automatically** controls startup
connection; **Reconnect automatically** also needs to be enabled to retry a
failed or dropped link. Serial and network targets are saved separately.
**Device Information** shows the connected endpoint, amplifier firmware and
serial number from readback; unreported values remain unknown. The same
information appears in the applet's **Settings**.

<img src="/img/kpa500/kpa500-applet-drv.png" width="360" alt="KPA500 Full view with output power, SWR and the radio-sourced DRV meter; illustrative replay values." />

*Full view with replayed telemetry illustrating the DRV meter.*

*The KPA500 applet shows amplifier readback. This capture was taken without RF.*

The **KP500** applet-bar button opens **Elecraft KPA500 Amplifier**. Its shorter
label fits the standard button width and preserves the full name in the applet title.
The button appears after a valid amplifier reply, not just an open connection.
Once detected, it stays available while that connection remains open, including during
power-off or lost replies, so you can wake the amp or inspect **Debug**. Closing
the connection hides the button; reconnecting requires a fresh reply.
The applet shows power with a peak marker, SWR, temperature,
voltage, current, confirmed band and amplifier state. **Standby**, **Operate**,
**Fan min**, **Speaker alarm**, **Clear fault**, and the **ON**/**OFF** button
send requests; amplifier replies confirm their displayed state. The power button
shows **ON** when the amp is on and **OFF** when it is off; **—** means its
power state is unknown. Click it to request the opposite state. Its tooltip
explains the action, and the label changes only after amplifier readback.
Waking the amp requires its rear power switch to be on. After a power-off request, a confirmed off reply,
or loss of replies, normal telemetry polling stops. A power-status query every
five seconds detects front-panel power-on; a confirmed on reply resumes normal
polling. The check never wakes the amp. Silence does not prove it is off.
**RF ACTIVE** and the red status banner mean the amplifier reports forward
power above zero. Radio TX alone does not indicate amplifier activity. The
published serial protocol does not report the RCA **KEY IN** / PTT input state,
so the applet cannot confirm that the radio has keyed the amplifier. Radio TX
still holds power, mode and band changes until RX.

Active features use the same green, Standby amber, and unavailable-control
dimming as the PGXL applet.
**Auto** fan minimum permits the fan to stop; the amplifier still manages
cooling automatically.

**Settings → Follow TX flag** is enabled by default. The amplifier follows
the TX-assigned slice, including split selection, XIT and FM transmit offset,
within the active band plan. Receive-only and Demo radios supply no TX band.
The top band pill shows the confirmed amplifier band and **Follow TX**.
Hover over it, or open **Settings**, for the source frequency, target band and
pending state. Disable the setting to park the amp with the band dropdown
that replaces the pill; it also shows confirmed amplifier readback. Band changes wait while the radio reports TX or the amplifier reports
RF, then send only the latest requested band. Missing readback stops automatic
retries; select again or toggle **Follow TX flag** to retry. Following never
powers the amp on, selects Operate, or keys the radio.

The meter readouts use compact pills beside scales with major and minor ticks.
In **Full**, **DRV** shows the connected radio's measured forward watts before
cable losses. It is not a measurement at the KPA500 input, an RF-power slider
setting, or amplifier output. The scale is **0–100 W**; the pill retains readings
above the scale. **DRV —** means the radio is disconnected or its forward-power
measurement is missing or more than two seconds old. Other fresh meters cannot
keep an old drive reading alive. The source stays radio forward power when the
shared **VU** and **PWR** meters switch to **AMP**. **TX Controls** continues
to show the radio’s forward power and SWR on the radio scale, so drive remains
visible. Its **RF Power** and **Tune Pwr** sliders show percentages.
**Mini** has no DRV row.

The shared **VU** and **PWR** meters use KPA500 output only while its power-on,
Operate and forward-power readbacks are current. If amplifier power telemetry
expires while other replies continue, the shared meters and scale return to
the radio until a fresh amplifier measurement arrives.

The KPA500 SWR scale runs from **1 to 5**, matching the amplifier faceplate;
its display turns amber at 2 and red at 2.5. No RF means SWR is unmeasured.

**Mini** shows the power control, band header, amplifier mode and faults, with
power and SWR meters at the bottom. It hides mode controls and the bottom
**History** and **Settings** buttons. **Full** restores those controls, the other
meters, and any previously expanded drawers; use it to change Standby/Operate
or clear a fault. **History** adds graphs for power, SWR, temperature,
voltage and current over 1, 5 or 10 minutes. Collection continues with the
history collapsed or in Mini mode. Gaps represent unavailable measurements.
The graphs use fixed scales and display measured peaks; values beyond a
scale are clipped. Floating or placing the applet on the canvas opens the
full view with History. Meters and graphs follow the window width, and History
uses extra height for taller graphs. Controls stay grouped at the top. A vertical
scrollbar keeps expanded History and Debug accessible in a smaller window.

<img src="/img/kpa500/kpa500-applet-mini.png" width="360" alt="KPA500 Mini mode with power, SWR, band and amplifier status." />

*Mini mode keeps the essential controls and readings visible.*

<img src="/img/kpa500/kpa500-applet-history.png" width="420" alt="KPA500 expanded history graphs for power, SWR, temperature, voltage and current." />

*History uses amplifier measurements; SWR remains unmeasured without RF.*

**Settings** includes **Temperature in °F**, connection endpoint (and local serial speed), last valid
reply age, firmware and serial number, plus an explanation of history gaps and
scales. **Debug** normally shows connection and
power transitions, control requests, errors and pending band changes. Enable
**Verbose** to include routine queries, raw amplifier replies and decoded telemetry.
Non-printable replies are discarded before they reach device status or diagnostics.
Raw received bytes are escaped in Verbose so they cannot insert log lines or control sequences.
**TX** in this log means protocol writes, not RF transmission. Switching Verbose
changes the displayed history; polling chatter does not displace normal events. **Pause** freezes the display while each bounded 2,000-line view
continues collecting; **Resume** catches up. **Copy** and **Save…** export the
visible paused log or the current buffer.



*Settings and Debug. Station identifiers are redacted.*

**NO RESPONSE** means the connection is open without valid amplifier replies.
Cached readings clear after silence; individual missing fast and slow fields
expire after 3 and 8 seconds. The power button shows **—** and remains available
to request wake while the connection is open. A missing mode response is never treated as Standby. **SWR: NO RF**
means SWR is unmeasured, rather than a measured 1:1 ratio.

Serial connects locally; Network connects directly to a raw TCP serial proxy
and does not require AetherD. Telnet/RFC 2217 and Elecraft Remote Server are
not supported. KAT500 tuner control is not implemented.

### KPA500 Advanced maintenance

Open **Peripherals → Elecraft KPA500 → Advanced**, or **Settings → Advanced…**
in the KP500 applet. Maintenance supports confirmed firmware **01.54**. The
amplifier must be powered on, responding and reporting zero RF; radio TX
blocks all maintenance. Reads run only when requested.

Only one maintenance file chooser opens at a time. Canceling it leaves the
amplifier and files unchanged. Closing Peripherals also closes its chooser;
reopening Advanced lets you start a new selection.

- **Save backup…** reads the 224-byte configuration and saves a `.KPAConfig`
  file with firmware, serial number and CRC32. It includes the power-on banner.
- **Choose backup…** validates a file and shows its identity without changing
  the amplifier. To restore, put the amp in **Standby**, select a backup with
  the same serial number and firmware, check **Replace this amplifier’s stored
  configuration**, then click **Restore selected backup**. AetherSDR verifies
  the written bytes by reading them back. Power-cycle the amplifier manually
  after successful verification; it does not restart automatically.
- **Read banner** loads the confirmed power-on text. Edit up to eight printable
  ASCII characters, then click **Save banner** while in Standby. The confirmed
  label changes only after verified readback.
- **Refresh fault history** reads up to 128 stored events, newest first, with
  amplifier uptime, band, fault code, raw data and a description. Uptime is not
  a calendar timestamp. **Export CSV…** saves the captured table. This history
  is separate from the applet’s live telemetry graphs and **Clear fault**.

Polling, band following and amplifier controls pause during maintenance.
**Cancel operation** stops further requests; it cannot undo bytes already
written. A timeout, disconnect, unexpected response or TX aborts the operation.
Writes are never retried or resumed automatically. An incomplete restore may
have changed part of the configuration; reconnect and inspect the amplifier
before attempting another restore. Fault history is not part of a configuration
backup and is never overwritten by Restore.

<img src="/img/kpa500/kpa500-advanced-window.png" width="720" alt="Peripherals with a NETWORK-connected KPA500 and Advanced backup, restore, banner and stored fault history controls." />

*Advanced maintenance after reading the banner and stored fault history. Station identifiers are redacted.*

## Using the Elecraft KPA1500

> **Not yet validated on real hardware.** The KPA1500 support follows
> Elecraft's published programming reference but has not been run against a
> physical amplifier ([#4097](https://github.com/aethersdr/AetherSDR/issues/4097)).
> Reports from owners are welcome.

The **Elecraft KPA1500** applet (button **KPA1500**):

- A status pill, **PWR**, **REF** and **SWR** gauges.
- An info grid with PA temperature, band and the ATU state.
- A fault banner showing the code in hex (for example `Fault B0`) and a
  **CLR FAULT** button.
- **OPERATE / STANDBY**. The label changes only when the amplifier confirms.
- The internal ATU: **ATU IN / ATU BYP**, and **TUNE**, which reads
  `TUNING` while a tune runs; press it again to cancel. A full-search tune
  needs RF, so key the radio's own TUNE: nothing in this applet keys the
  radio.
- **ANT** **1** / **2**, the amplifier's two antenna connectors. An `ANT n`
  label appears when an external antenna switch reports an antenna number
  from 3 to 32.

There is no network PTT and no lost-link TX inhibit.

## Using the RF-KIT

> **Tested on an RF2K-S, firmware G200C267.** The RF-KIT support follows the
> vendor's published REST API documentation (RFKIT API 0.9.0). Other RF-KIT
> models have not been tried. Reports from owners are welcome.

Add it with **Peripherals → Add → RF-KIT Amplifier**, enter the amplifier's IP
address (or host name) and HTTP port (default 8080), and press **Connect**.
AetherSDR talks to the amplifier's REST API over plain HTTP; there is no login.

The **RF-KIT Amplifier** applet:

- A status pill, the amplifier's name, and **PWR**, **REF** and **SWR** gauges.
  The scales are fixed, like the amplifier's own display: forward power
  0–1600 W, reflected power 0–200 W, and SWR on the amplifier's 1 / 1.5 / 2 / 3 /
  5 / ∞ scale. The reflected-power and SWR bars turn orange from 1.5:1 and red
  from 2:1 (on the reflected-power bar these points follow the forward power).
- An info grid with temperature, supply voltage, current, band, frequency and
  the operational interface (`UNIV`, `CAT`, `UDP` or `TCI`).
- A **TUNER** line showing the tuner's mode, setup and L/C values, and a
  **TUNER ON** (green) / **TUNER OFF** (red) indicator beside OPERATE. The tuner
  is read-only: the amplifier's API has no tuner control, so the indicator
  cannot be pressed.
- The amplifier's own status text and, when it reports one, an error banner.
- **OPERATE / STANDBY**. The label changes only when the amplifier confirms.
- **ANT** buttons for the internal antennas (1–4). Antennas the amplifier
  reports as disabled, or does not list, are dimmed (hover for the reason), and the amplifier may refuse a change at
  times (it answers 409). **EXT** is dimmed unless the amplifier lists an external
  antenna, and lights, with the switch port ("EXT 16") when the amplifier gives
  one, while the external antenna is in use. It is an indicator only: the
  external antenna cannot be selected from AetherSDR.
- **RESET ERR**, which asks for confirmation and then clears the amplifier's
  error state. Make sure the cause has been dealt with first.

The operational interface is shown but cannot be changed from AetherSDR:
switching it could cut the amplifier off from the radio driving it. There is no
network PTT.

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
| SPE Expert Amplifier | Serial, or network through ser2net (raw or telnet) | 115200 8N1 (original 1K-FA: 9600 8N1); TCP 7000 |
| VK3AMP Amplifier | Network only: TCP control, UDP telemetry | TCP 5005; UDP 5010 |
| Elecraft KPA500 | Serial, or network through a raw TCP ser2net proxy | 4800 8N1; selectable locally, configured on the server for TCP; TCP 2000 |
| Elecraft KPA1500 | Network only: TCP | TCP 1500 |
| LP-100A Meter | Serial, or network through a ser2net proxy in raw mode | 115200 8N1; TCP 2000 |

## Known issues

- The original 1K-FA briefly disconnects the radio's antenna path while it powers up, and AetherSDR does not yet hold off transmitting during that time ([#6231](https://github.com/aethersdr/AetherSDR/issues/6231)).

- An SPE amplifier set to connect at startup shows connected but no data until you disconnect and reconnect it ([#4893](https://github.com/aethersdr/AetherSDR/issues/4893)).

## Troubleshooting

### The SPE amplifier will not power on over the network

Powering on drives the ser2net proxy's DTR/RTS lines, which a plain raw or
telnet port cannot do.

1. Configure the ser2net port as an RFC 2217 telnet port:
   `accepter: telnet(rfc2217=true),<port>`. The tooltip on the network address
   field has a sample configuration.
2. Reconnect the amplifier in **Peripherals** and press **ON**.

### The original 1K-FA will not power on over the network

The network bridge cannot switch the serial port's DTR line. A note under the
applet's buttons says so when you press **ON**.

1. If the bridge is a plain serial-to-Ethernet converter, switch the
   amplifier on at its front panel; monitoring and the keys still work.
2. For remote power on, use ser2net with `accepter: telnet(rfc2217=true),<port>`
   and a serial adapter that has a DTR line, then reconnect and press **ON**.

### The original 1K-FA switches off when I disconnect

AetherSDR holds the 1K-FA's power line only while it is connected, so an
amplifier that **ON** switched on powers off on **Disconnect**, when you quit
AetherSDR, or when the serial link drops.

1. To keep the amplifier on without AetherSDR, switch it on at its front panel
   instead of with **ON**.

### The VK3AMP takes a long time to connect on Windows

After the amplifier has been idle, the first connection can take 9–12
seconds, because the amplifier answers only broadcast ARP.

1. Wait: AetherSDR waits up to 18 seconds before trying again.

### The RF-KIT will not connect

AetherSDR declares the link up only when the amplifier answers `GET /info` with
an RF-KIT document, so a wrong address or port fails rather than half-connecting.

1. Check the address and port in **Peripherals**; the default HTTP port is 8080.
2. Open `http://<address>:<port>/info` in a web browser on the same network. An
   RF-KIT answers with a short JSON document.
3. If the link drops, AetherSDR marks it down after three failed requests in a
   row; enable **Reconnect automatically** to have it retry.

### The SPE front-panel keys are dimmed

The popped-out **FRONT PANEL** keys work only while the LCD mirror is current.
Its data has gone stale, or the amplifier is an original 1K-FA, which has no
LCD mirror; on the 1K-FA the keys stay dimmed.

1. Check the amplifier's connection in **Peripherals**.
2. Wait for the LCD mirror to update; the keys come back with it.

## See also

- [Peripherals](./peripherals.md)
- [TGXL Tuner Control](./tgxl-tuner-control.md)
- [Meters](./meters.md)
- [AetherSweep](./aethersweep.md)
