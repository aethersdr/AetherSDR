---
title: "Log Categories"
description: "The logging categories AetherSDR offers for support logs, generated from the source."
sidebar_position: 7
custom_edit_url: null
generated_by: "tools/docs/gen_reference.py"
generated_from: ["src/core/LogManager.cpp"]
---

:::info[Generated page]

This page is generated from `src/core/LogManager.cpp` by `tools/docs/gen_reference.py`. Do not edit it by hand: change the source, then run `python3 tools/docs/gen_reference.py`.

:::

These are the logging categories listed in **Help → Support & Diagnostics...**. Turn one on there to add its detail to the support log. The **Category** is the name the automation bridge's `log set` verb takes.

| Label | Category | What it logs |
|---|---|---|
| Discovery | `aether.discovery` | UDP radio discovery broadcasts |
| Connection / Commands | `aether.connection` | Raw TCP command channel lines: TX commands, RX responses, and socket state |
| Protocol / Status | `aether.protocol` | Parsed SmartSDR protocol handling and model status updates |
| Audio | `aether.audio` | RX/TX audio, device negotiation, volume |
| Audio Summary | `aether.audio.summary` | Default support log summaries for audio routing and sink/source negotiation |
| VITA-49 | `aether.vita49` | UDP packet routing: FFT, waterfall, meters, DAX |
| DSP | `aether.dsp` | NR2, RN2, CW decoder processing |
| RADE | `aether.rade` | FreeDV Radio Autoencoder digital voice |
| SmartLink | `aether.smartlink` | Auth0 login, TLS tunnel, WAN streaming |
| TCI / CAT / rigctld | `aether.cat` | TCI server (slice and DAX arming, TX audio summary, TX\_CHRONO stall summary at debug: maxGap/latePolls/catch-up), rigctld TCP servers, PTY virtual serial ports |
| DAX | `aether.dax` | Virtual audio bridge (PipeWire/CoreAudio) |
| Meters | `aether.meters` | Meter definitions and value conversion |
| Transmit | `aether.transmit` | TX state, ATU, profiles, power control |
| Firmware | `aether.firmware` | Firmware download, staging, upload |
| Tuner/AGM | `aether.tuner` | TGXL tuner, Antenna Genius state |
| Peripheral Authorization | `aether.peripheral.auth` | Credential store availability and save failures; code values are never logged |
| GUI | `aether.gui` | Window, applets, dialogs |
| DX Cluster | `aether.dxcluster` | DX cluster telnet connection and spot parsing |
| MQTT | `aether.mqtt` | MQTT telemetry client connection and messages |
| RBN | `aether.rbn` | Reverse Beacon Network connection and spots |
| Ext Devices | `aether.devices` | Serial port, FlexControl, MIDI, HID encoder |
| Performance | `aether.perf` | Render timing and CPU profiling data |
| Render | `aether.render` | Render pipeline: RHI/GPU path selection and fallback, paint stalls, texture upload churn |
| Propagation | `aether.propforecast` | Solar and propagation forecast updates |
| CW / netCW | `aether.cw` | CW keying, MIDI paddle, iambic, and netCW timing |
| S History | `aether.shistory` | Past-Signals voice detection: noise floor, region width, band-plan filter |
| AetherModem | `aether.ax25` | AX.25 modem lifecycle, RX/TX audio, demod, framing, and packet diagnostics |
| AX.25 Link | `aether.ax25.link` | Connected-mode data link: session open/close, measured round-trip vs configured T1, retransmits, idle-link polls |
| Waveform | `aether.waveform` | Docker waveform image install upload and local waveform helper lifecycle |
| KiwiSDR | `aether.kiwisdr` | KiwiSDR remote RX antennas: connect, handshake, audio/waterfall negotiation, reconnect, profile lifecycle |
| ANAN Protocol 2 | `aether.anan.p2` | ANAN/Saturn Protocol 2 wire session: DDC sequence gaps, speaker-audio FIFO level and underflow reports, unexpected sender ports |
| KiwiSDR Audio/DSP | `aether.kiwisdr.audio` | Verbose KiwiSDR receive audio: frame decode, resampler, jitter/FIFO under/overrun, mixing (high-rate; off by default) |
| Automation Bridge | `aether.automation` | Agent-drivable test bridge (#3646): QLocalServer verbs, widget snapshots, captures (AETHER\_AUTOMATION only) |
| External Services | `aether.network` | Failed requests to external services (update check, QRZ, maps, propagation, radar), once per host and error, with TLS errors and the TLS backend in use |
| QRZ Lookup | `aether.qrz` | QRZ.com callsign lookups: session, cache, CW callsign spotting, photos |
| AetherClock | `aether.clock` | WWV/WWVB time-signal decoder: state transitions, per-second alignment, frame decodes, voter verdicts |
| Hermes-Lite 2 | `aether.hl2` | HL2 backend: band changes, J16 companion-filter selection, LNA gain, and radio health telemetry |
| Hermes-Lite 2 TX | `aether.hl2.tx` | HL2 transmit telemetry — TWO different instruments on one toggle, and the difference is the point. The RADIO’s DSIQ FIFO depth with its underflow/overflow flags and the forward/reflected power counts; AND the HOST queue’s own starvation lines, which are NOT that FIFO and which that FIFO cannot see. Separate toggle — ticking "Hermes-Lite 2" does NOT enable this (high-rate) |
| Icom Session | `aether.icom.session` | Icom RS-BA1 session: handshake, token/auth, capabilities, keepalive |
| Icom Streams | `aether.icom.stream` | Icom UDP stream lifecycle: control/serial/audio handshakes and ports |
| Icom CI-V | `aether.icom.civ` | Every CI-V frame in and out, decoded — command, subcommand and payload bytes. The only way to tell 'the radio never sent it' from 'we sent it and dropped the reply' (high-rate) |
| Icom Scope | `aether.icom.pan` | Icom spectrum scope: sweep frames, division reassembly, bounds |
| Icom Link | `aether.icom.link` | Icom backend link state: connect/disconnect, model resolution, capability publication |
| Icom Credentials | `aether.icom.cred` | Icom credential storage and retrieval (no secret values are logged) |
| System Info | `aether.sysinfo` | Startup hardware/capability inventory: OS, CPU model + SIMD features, RAM, and the speech-engine ISA baseline check (#4986). A few lines once per launch |
