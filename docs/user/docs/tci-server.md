---
title: "TCI Server"
slug: "/tci-server"
description: "AetherSDR includes a built-in TCI (Transceiver Control Interface) server."
---

AetherSDR includes a built-in [TCI (Transceiver Control Interface)](https://github.com/ExpertSDR3/TCI) server. TCI is an open protocol that carries CAT control, RX/TX audio, IQ data, CW keying and spots over a single WebSocket connection, so you don't have to set up separate CAT ports, virtual audio devices and serial ports.

TCI is also the simplest way to connect WSJT-X on **Windows**, where AetherSDR ships no DAX driver, and on radios that have no DAX at all, such as the Hermes-Lite 2.

TCI was created by Expert Electronics for ExpertSDR3 and is now widely supported by SDR programs. It uses a WebSocket connection: text frames for commands and binary frames for audio and IQ. Any program on your computer or LAN can connect with no audio routing to configure.

| Task | Traditional | TCI |
|------|------------|-----|
| CAT control | rigctld TCP or virtual serial port | Single WebSocket |
| RX audio | DAX virtual devices | Same WebSocket |
| TX audio | DAX TX device | Same WebSocket |
| IQ data | DAX IQ devices | Same WebSocket |
| Spots | Separate DX cluster connection | Same WebSocket |
| CW keying | Serial DTR/RTS or rigctld | Same WebSocket |

<img src="/img/screens/tci-applet.png" width="248" alt="TCI Server applet. Level bars for RX1, assigned to Slice A, RX2 to RX4, unassigned, and TX, assigned to Slice A, sit above the port field set to 50001, marked stopped, and a Disabled button that starts the server." />

*The TCI Server applet in the applet panel.*

## Setup

### Turning the server on

- **Settings → Autostart TCI with AetherSDR** starts the server on every radio connect.
- Or use the **Enabled** button in the **TCI Server** applet (applet-tray button **TCI**).

### The TCI Server applet

| Control | Meaning |
|---|---|
| **RX1 … RX8** | One row per receive channel: the slice it carries and a combined level meter / gain slider. Rows above the radio's slice capacity are hidden. |
| **TX** | Level meter and gain for transmit audio arriving from TCI clients. **Right-click** it to open **TX overflow handling** (see [TX overflow handling](#tx-overflow-handling)). |
| **Port:** | TCP port the server listens on. Default **50001**. |
| **Enabled / Disabled** | Starts or stops the server. |

### Connecting a TCI client

Point your program at:

```
ws://localhost:50001
```

or, from another machine on your LAN, `ws://<computer-ip>:50001`.

### Testing with wscat

```bash
npm install -g wscat
wscat -c ws://localhost:50001
```

You'll see the init burst with the full radio state. Then try:

```
vfo:0,0;                 # read the frequency
modulation:0;            # read the mode
vfo:0,0,14074000;        # tune to 14.074 MHz
modulation:0,digu;       # switch to DIGU
drive;                   # read the drive level
drive:25;                # set drive
trx:0,true;              # key PTT
trx:0,false;             # unkey PTT
audio_start:0;           # start RX audio for receiver 0
audio_stop:0;            # stop it
```

## Using TCI with other programs

### WSJT-X

Use a WSJT-X build with TCI support (WSJT-X Improved, or WSJT-X 3.x).

1. **File → Settings → Radio**
2. **Rig:** `Expert Electronics (TCI)`
3. **Network Server:** `localhost:50001`
4. **PTT Method:** `CAT`
5. Click **Test CAT**; it turns green.

WSJT-X takes CAT, PTT **and audio** over TCI, so there are no sound-card devices to set up. WSJT-X's **Pwr** slider attenuates the transmitted audio. See [WSJT-X Integration](./wsjt-x-integration.md).

### MSHV

**Settings → Interface**, select **TCI**, enter `localhost` and port `50001`, and enable audio streaming if wanted.

### Log4OM v2

**Settings → Radio**, choose **TCI** and enter `localhost:50001`. Log4OM receives spots, and clicking a spot in AetherSDR sends `clicked_on_spot` back to it.

### CW Skimmer and SDC

Point SDC or CW Skimmer at AetherSDR's TCI port. IQ flows as DAX IQ at 24, 48, 96 or 192 kHz (SDC negotiates 96 kHz), and `dds:` follows the panadapter centre. Up to **four** IQ subscriptions can run at once, so several skimmers can share one server. Receivers on the same panadapter share its IQ stream, so four independent bands need four panadapters. IQ is not offered as int16 or over `wss`.

### JTDX

JTDX has TCI support but is no longer maintained and can crash with some TCI servers. WSJT-X is recommended instead.

### Other TCI clients

Any program that implements the [TCI protocol](https://github.com/ExpertSDR3/TCI) can connect, for example:

- **HamDeck** — Stream Deck automation via TCI
- **TCI-Hamlib Adapter** (DL3NEY) — bridges TCI to Hamlib for programs that only support rigctld
- **eesdr-tci** — a Python TCI client library for scripting
- **AH-4-style tuner interfaces** and band-pass switch controllers that watch `tune:` (for example HB9DUT's TCI_ICOM_Tuner_Interface)

## Using several clients at once

Several TCI programs can be connected at once, and each keys the slice it asked for:

- `trx` keys **the receiver the client named**. A plain `trx:0` resolves against the receiver the client declared in its `audio_start`, so a second WSJT-X instance does not take transmit from the first.
- An externally selected TX slice only wins when split was requested, or when VFO B set up a route for that exact receiver, so satellite and cross-band split still work.
- A receiver that cannot be resolved is **declined**, never keyed on slice A instead.
- **Every refused PTT is answered with `trx:<n>,false;`**, including a second client trying to key while another holds TCI PTT. WSJT-X shows this as "TCI failed to set ptt".
- **Receiver numbers are stable.** If a slice closes mid-session, the other slices keep their receiver numbers, and the freed number goes to the next new slice.
- A VFO B request from one client does not retune another client's slice.

## Audio streaming

TCI audio is sent and received as binary frames on the same WebSocket.

- **Audio doesn't flow until the client asks for it** with `audio_start:<receiver>;`.
- **PC Audio does not need to be on.** On a FlexRadio, TCI receive audio comes from DAX channels that the TCI server claims for itself; your speaker volume and mute don't affect it. On other radios it comes straight from the slice.
- RTL-SDR receive audio follows the stable receiver map and remains independent of speaker gain and mute; receiver squelch still applies. Normal launches offer one RTL receiver. Extra receivers belong to the [process-only evaluation](./rtl-sdr.md#multi-receiver-evaluation), not a higher production limit. RTL has no transmit or IQ export.
- If a client reconnects more than 10 seconds later, its receive audio is re-bound to the right DAX channels automatically.
- TX audio from a client is resampled from the rate the client declares, so 8, 12 and 24 kHz programs are not mis-pitched.

### Sample rate and format

The default is **48 kHz**. A client can request 8000, 12000, 24000 or 48000 Hz:

```
audio_samplerate:24000;
```

Any other rate (44.1 kHz, for example) is refused and the previous rate is echoed back. Each client has its own rate and converter.

Audio is float32 stereo by default. A client can ask for int16 and/or mono:

```
audio_stream_sample_type:int16;
audio_stream_channels:1;
```

### TX overflow handling

Right-click the **TX** meter in the TCI Server applet to choose how transmit samples beyond ±1.0 are treated:

| Mode | Behaviour |
|---|---|
| **Clip (saturating ±1.0)** | Default. Hard-clamps overshoots to ±1.0. |
| **NaN guard (zero NaN/Inf only)** | Passes samples through bit-exact, only zeroing NaN/Inf. Best for digital-mode tone fidelity. |
| **Measure only (true bypass)** | Never changes samples; counts overshoots for telemetry. |

FT8/WSJT-X users who want exact tones can choose NaN guard or Measure only.

## Reference

### Server identity

The init burst announces `device:AetherSDR;` and `protocol:ExpertSDR3,1.5;`, followed by the full radio state and `ready;`.

### Protocol coverage

AetherSDR implements the TCI 2.0 command set, including:

| Category | Commands |
|----------|----------|
| VFO / mode | `vfo`, `modulation`, `rx_filter_band`, `dds`, `if`, `vfo_limits`, `if_limits`, `vfo_lock` |
| TX | `trx`, `tx_enable`, `tune`, `drive`, `tune_drive`, `tx_frequency`, `tx_gain` |
| RIT / XIT | `rit_enable/offset`, `xit_enable/offset`, `split_enable` |
| Audio | `volume`, `mute`, `rx_volume`, `rx_mute`, `rx_balance`, `mon_enable`, `mon_volume`, `mic_level` |
| AGC / SQL | `agc_mode`, `agc_gain`, `sql_enable`, `sql_level`, `lock` |
| DSP | `rx_nb_enable/param`, `rx_nr_enable`, `rx_anf_enable`, `rx_apf_enable`, `rx_bin_enable`, `rx_anc_enable`, `rx_dse_enable`, `rx_nf_enable` |
| CW | `cw_macros_speed`, `cw_keyer_speed`, `cw_macros_delay`, `cw_msg`, `cw_macros`, `cw_macros_stop`, `cw_terminal`, `keyer` |
| Audio streaming | `audio_start/stop`, `audio_samplerate`, `audio_stream_sample_type`, `audio_stream_channels`, `audio_stream_samples` |
| IQ | `iq_start/stop`, `iq_samplerate` |
| Spots | `spot`, `spot_delete`, `spot_clear`, `clicked_on_spot` |
| Sensors | `rx_sensors_enable`, `tx_sensors_enable`, `rx_channel_sensors`, `tx_sensors` |
| Digital | `digl_offset`, `digu_offset` |

Notes:

- `volume` is in dB, as the spec defines it; the older percent form is still accepted. `mic_level` is a global 0–100 %.
- The server also broadcasts which slice has focus in the GUI, plus DSP, squelch, RIT/XIT and TX power when they change.
- Malformed arguments (for example to `volume:` or `modulation:`) are rejected, never applied to slice 0.
- A `vfo:` confirmation reports the frequency the radio actually reached.
- **Panadapter spectrum forwarding:** subscribed clients receive the panadapter's FFT rows.

### Tune notifications

`tune:<trx>,true|false;` is broadcast to **every** client on every tune edge, whether the tune came from the TX applet, any TCI client or the radio, and it follows the matching `trx:`. A `tune:` SET always gets an answer, and the init burst includes the current `tune:` state. A tune that a TCI client started stops when that client disconnects or the server stops. `tune:-1,true` keys nothing (#6200).

### Sensor telemetry

```
rx_sensors_enable:true;    # S-meter updates
tx_sensors_enable:true;    # power, SWR, mic and ALC while transmitting
```

RX sensors broadcast each receiver's S-meter in dBm:

```
rx_channel_sensors:0,0,-97.3;
```

TX sensors broadcast during transmit, including ALC.

### CW keying

```
cw_macros:0,CQ CQ CQ DE N0CALL;   # send text
cw_macros_stop;                   # stop
cw_keyer_speed:25;                # speed
keyer:0,true;                     # key down
keyer:0,false;                    # key up
```

## Known issues

- If a client that holds PTT dies without closing its connection (power loss, a pulled cable, a dropped Wi-Fi link), the radio stays keyed until the operating system gives up on the TCP connection ([#5985](https://github.com/aethersdr/AetherSDR/issues/5985)).
- Transmit audio from a TCI client passes through the Phone/CW microphone chain, so the mic profile, **MIC** level and **PROC** change what a digital-mode client puts on the air ([#5844](https://github.com/aethersdr/AetherSDR/issues/5844)).
- TCI receive audio can stop after moving to a voice frequency with DAX off and back to a data frequency with DAX on, while CAT keeps working ([#4824](https://github.com/aethersdr/AetherSDR/issues/4824)).
- After a TCI voice keyer (for example OpsLog) transmits, DAX stays on, so the next microphone transmission has no audio until you turn DAX off ([#5524](https://github.com/aethersdr/AetherSDR/issues/5524)).
- A cross-band tune from a TCI client does not recall the radio's band stack, so the per-band antenna, mode and filters are not restored ([#3543](https://github.com/aethersdr/AetherSDR/issues/3543)).
- The TCI port listens on every network interface with no password, so any machine that can reach it can tune and key the radio ([#5296](https://github.com/aethersdr/AetherSDR/issues/5296)).

## Troubleshooting

The **TCI / CAT / rigctld** category in **Help → Support & Diagnostics…** logs the TCI server. See [Support and Logging](./support-and-logging.md).

### The server won't start because port 50001 is in use

Another program already listens on the TCI port.

1. Change the **Port:** field in the TCI Server applet.
2. Point each TCI client at the new port.

### A client connects but gets no audio

Audio only flows after the client sends `audio_start`.

1. Check that the client has audio streaming turned on. WSJT-X sends `audio_start` automatically.
2. Test by hand with [wscat](#testing-with-wscat): send `audio_start:0;` and watch for binary frames.

### WSJT-X shows "TCI failed to set ptt"

AetherSDR refused the PTT request. This happens when another TCI client already holds PTT, or when the requested receiver can't be resolved to a slice.

1. Stop the other client's transmission, then try again.
2. Check that each WSJT-X instance uses its own receiver.

### TCI keys the wrong slice

The PTT routing needs a log to diagnose.

1. Start AetherSDR with `QT_LOGGING_RULES="aether.cat.info=true"`. This logs one line per PTT decision: the requested receiver, the mapped slice, the chosen slice, the slice holding TX and any cached route.
2. Reproduce the problem and include that log when you report it.

### JTDX crashes

This is a known JTDX bug; JTDX is no longer maintained.

1. Use WSJT-X instead.

### A client on another computer can't connect

A firewall is blocking the TCI port, or the client is using `localhost`.

1. Open the TCI port (TCP) in the firewall of the computer running AetherSDR.
2. Point the client at `ws://<computer-ip>:<port>`.

## See also

- [WSJT-X Integration](./wsjt-x-integration.md)
- [DAX Virtual Audio](./dax-virtual-audio.md)
- [DAX IQ Streaming](./dax-iq-streaming.md)
- [CAT Control](./cat-control.md)
- [SpotHub](./spothub.md)
- [TCI protocol specification](https://github.com/ExpertSDR3/TCI)
