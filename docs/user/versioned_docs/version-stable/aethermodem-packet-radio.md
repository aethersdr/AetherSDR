---
title: "AetherModem Packet Radio"
slug: "/aethermodem-packet-radio"
description: "AetherModem is AetherSDR's built-in AX.25 packet modem."
status: "Supported"
applies_to: ["FlexRadio", "Hermes-Lite 2 (experimental)", "Networked Icom (early; IC-7300MK2 supported)"]
---

:::info[Status]

**Status:** Supported · **Applies to:** FlexRadio, Hermes-Lite 2 (experimental), Networked Icom (early; IC-7300MK2 supported)

:::

AetherModem is AetherSDR's built-in AX.25 packet modem. It decodes and transmits packet without an external TNC or a sound-card modem such as Dire Wolf, and on top of it sit an **APRS client**, a **WIDE1-1 fill-in digipeater**, a **KISS-over-TCP TNC** for other packet programs, a connected-mode **terminal** for calling a BBS, and a **personal mailbox** that other stations can connect to.

AetherModem works on FlexRadio, on the [Hermes-Lite 2](./hermes-lite-2.md) and on [Networked Icom](./networked-icom.md) radios. On a FlexRadio it transmits through DAX TX: the modem keys PTT itself and restores your previous DAX state afterwards.

## Setup

Open it from **Tools → AetherModem…**

### Window layout

The window has six tabs: **APRS**, **Digipeater**, **KISS TNC**, **Terminal**, **Mailbox** and **D-STAR** (the D-STAR tab is covered on [D-STAR (ThumbDV)](./d-star-thumbdv.md) and is hidden on radios without waveform support). A slim status bar along the bottom shows the modem state, the receive gain and a packet-activity strip.

All AetherModem transmissions share one queue and key the radio one at a time, so the APRS client, digipeater, KISS clients, terminal and mailbox never talk over each other.

### Modem and profiles

The modem controls are on the **APRS** tab:

| Control | What it does |
|---|---|
| **BAUD RATE** | **300 baud** (HF, 1600/1800 Hz tones) or **1200 baud** (VHF Bell 202, 1200/2200 Hz tones) |
| **Enable Modem** | Starts decoding on the attached slice |
| **Autostart at launch** | Starts the modem when AetherSDR starts |

The 1200-baud receiver uses a Direwolf-derived AFSK demodulator. The 300-baud HF receiver is AetherModem's own multi-lane decoder.

Packet length, retry timing and preamble are derived from the baud rate, not fixed values sized for VHF. On HF the packet length defaults to 64 bytes. The 1200-baud preamble is shorter than the HF one; raise it if a transverter's T/R switching clips the start of a burst.

**Tips for receive level:** avoid clipping (peaks around −10 dBFS are fine) and don't chase every missed decode with AF gain once the tones are clearly visible.

## Using AetherModem

### APRS

The APRS tab is a lightweight APRS client.

- **MY CALLSIGN** (for example `N0CALL-9`), **SYMBOL** and **PATH**.
- **BEACON:** **Every** *n* **min**, a status text, and **Beacon Now**.
- **POSITION:** taken from the radio's GPS when it has a fix (for example a FlexRadio with the GPSDO option). Without GPS, type a Maidenhead locator in **GRID** (for example `JN48Qm`, which fills the latitude and longitude) or enter **MANUAL LAT** / **LON**.
- **Station table:** TIME, STATION, SYMBOL, AGE, PKTS, GRID, DIST, CRS/SPD and STATUS / COMMENT, with symbol icons and decoded weather reports.

#### Messaging

Type the recipient in **To**, the text beside it, and press **Send APRS Msg**. Messages ask for an acknowledgement and are retried until acknowledged; incoming messages are acknowledged automatically, and digipeated duplicates are dropped. The **✉** button opens the message view.

The **▾** button beside the To field addresses common APRS services and fills in the right syntax (hover an entry for its hint):

| Entry | Service |
|---|---|
| **SMS** | Text a phone through the SMS gateway: `@<number> <message>` |
| **EMAIL-2** | Send an email: `<address> <message>` |
| **WLNK-1** | Winlink APRSLink: `?` for help, or a one-line email/SMS |
| **WXBOT** | Weather forecast or METAR for a city, airport code or grid |
| **ISS / ARISS** | Path hint only: set the TX path to `ARISS` for the ISS digipeater |

Internet gateways (APRS-IS) are not part of AetherModem.

### Digipeater

A **WIDE1-1 fill-in digipeater**, for filling gaps in local APRS coverage.

- **Enable WIDE1-1 fill-in** arms it. It answers the first unused hop when that hop is `WIDE1-1`, and optionally your own call (**MYCALL**) or the legacy **RELAY** alias. It inserts your callsign with the "has been repeated" bit set.
- It needs the **1200 baud** profile and a valid callsign, and it must be **armed again each session**: your configuration is saved, but arming is never restored at startup.
- Switching to 300 baud, disabling the modem, changing the slice, disconnecting or closing AetherSDR disarms it.
- It does not decrement `WIDE2-n` hops; it is a fill-in digipeater, not a wide-area one.
- The tab has its own **CALL**, **ALIAS**, **BEACON** (with **Now**), **SYMBOL**, **PATH** and **TEXT** fields, **MESSAGES**, **DIGIPEATS** and **SKIPPED** traffic graphs, and a **RAW MESSAGES** list in TNC-2 format.

### KISS TNC

Turns AetherModem into a KISS-over-TCP TNC so other packet and APRS programs can use it, for example Xastir, YAAC, APRSdroid, UISS, or anything that talks to Dire Wolf's KISS port.

- **KISS TNC SERVER:** **Enable TNC**, **Start TNC on Startup** and **TCP PORT** (default **8001**, listening on all network interfaces). All three are saved.
- With **Start TNC on Startup**, the TNC runs in the background from launch and keeps running when the window is closed.
- Several programs can connect at once. Every decoded frame goes to all of them; frames they send are transmitted with the baud profile chosen on the APRS tab.
- Enabling the TNC turns the modem on, but a slice must be attached for traffic to flow.
- **TNC STATUS** shows the port, the number of clients and RX/TX frame counts.

### Terminal

A connected-mode AX.25 client for calling a packet BBS or another station's mailbox.

- **MY CALLSIGN** and **CONNECT TO**, with **Connect**, **Cmd Mode** and **MHeard**.
- **LINK PARAMETERS:** **Retry s**, **Tries**, **Paclen**, **TXD flags** and **TX Tail ms**. Retry, Paclen and TXD default to **Auto**, worked out from the current baud rate; set a number to override.
- **Log session to file** records the session.
- Each session's measured round-trip time is logged (the **AX.25 Link** log category), which tells a mis-sized timeout from a poor channel.
- Asking to disconnect a second time ends a disconnect that is still retrying. Turning the modem off stops transmitting and drops the session.

### Mailbox

A personal mailbox (PMS) in the style of the Kantronics KPC-3. One station at a time can connect to it over 1200-baud connected mode, read and leave messages, see who has been heard, and disconnect.

- **Enable Mailbox (PMS)**, the **LISTEN CALLSIGN** (full `CALL-SSID`, for example `N0CALL-10`), and an optional **VANITY ALIAS** of up to 6 characters (for example `AETBBS`) that it also answers on.
- **WELCOME / PTEXT** greeting, and an optional **Send hourly beacon** with its own **BEACON TEXT**.
- **MAILBOX STATUS**, **STATISTICS** and **LAST CALLERS**.

**Commands for callers** (first letter or the full word): `H` help, `B` bye, `I` info, `J` heard list, `L` list, `LM` list mine, `R n` read, `K n` kill, `S` / `SP call` send, `SB cat` send bulletin, `U` users. End a message with `/EX` or Ctrl-Z on its own line. Address a public message to `ALL`.

Messages, callers and the heard list are kept in `~/.config/AetherSDR/pms/` (`messages.json`, `callers.json`, `heard.json`).

### MQTT

Decoded frames are published on `aethersdr/ax25/rx`, and AetherModem transmits frames received on `aethersdr/ax25/tx`. See [MQTT Station Automation](./mqtt-station-automation.md).

## Troubleshooting

The **AetherModem** log category covers decoding, transmit and the KISS server; **AX.25 Link** covers connected-mode sessions. See [Support and Logging](./support-and-logging.md).

### Nothing decodes

The modem is off, listening to the wrong slice, set to the wrong baud rate, or the receive level is clipping.

1. On the **APRS** tab, check **Enable Modem** is on.
2. Check the right slice is attached.
3. Match the **BAUD RATE** to the channel: **300 baud** on HF, **1200 baud** on VHF.
4. Lower the receive level until it no longer clips; peaks around −10 dBFS are fine.

### A KISS client connects but nothing transmits

The modem has no slice attached, or the radio can't transmit.

1. Attach a slice to the modem. Enabling the TNC turns the modem on, but traffic only flows with a slice attached.
2. Check that the radio is able to transmit.

### The digipeater is off after a restart

Arming the digipeater is never restored at startup; only its configuration is saved. Switching to 300 baud, disabling the modem, changing the slice or disconnecting also disarms it.

1. Make sure the modem is on the **1200 baud** profile and has a valid callsign.
2. On the **Digipeater** tab, tick **Enable WIDE1-1 fill-in** again.

### The start of each 1200-baud burst is clipped

A transverter's T/R switching is cutting into the preamble, which is shorter at 1200 baud than on HF.

1. On the **Terminal** tab, under **LINK PARAMETERS**, set **TXD flags** (the preamble, in HDLC flags before every frame) to a number higher than **Auto** gives.

## See also

- [DAX Virtual Audio](./dax-virtual-audio.md)
- [D-STAR (ThumbDV)](./d-star-thumbdv.md)
- [MQTT Station Automation](./mqtt-station-automation.md)
- [Hermes-Lite 2](./hermes-lite-2.md)
- [Support and Logging](./support-and-logging.md)
- [docs/MODEM.md](https://github.com/aethersdr/AetherSDR/blob/main/docs/MODEM.md) and [docs/HFMODEM.md](https://github.com/aethersdr/AetherSDR/blob/main/docs/HFMODEM.md) (developer notes)
