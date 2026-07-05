# aetherd migration — gui→engine touchpoint manifest

<!-- GENERATED FILE — regenerate with `python tools/gen_touchpoint_manifest.py`. Edit the tags/status JSON sidecars, never this table. -->

Burndown manifest for the engine/UI decoupling ([RFC](../aetherd-headless-engine-design.md) §2, §10). One row per engine header the UI includes; converting a touchpoint means the UI reaches that surface through the versioned protocol instead of the header.

**Totals:** 140 touchpoint headers (117 core, 23 models) — 140/140 tagged, 0/140 converted.

| Header | Includers | Tag | Status |
|---|---:|---|---|
| `core/AdaptiveFilterEngine.h` | 1 | universal — GUI-thread adaptive RX passband coordinator over SliceModel/spectrum frames; canonical radio state, no vendor ties. | unconverted |
| `core/AgcTCalibrator.h` | 1 | universal — Engine algo sweeping slice AGC threshold vs audio RMS/S-meter to recommend a value; only canonical state. | unconverted |
| `core/AppSettings.h` | 90 | ui-support — Client-side XML settings store (SSDR.settings-style key/value persistence); app plumbing, not radio state. | unconverted |
| `core/AudioEngine.h` | 44 | mixed(flex) — Client audio I/O + full RX/TX DSP chain (universal) fused with Flex VITA-49/DAX/Opus TX and Kiwi buffering | unconverted |
| `core/AudioOutputRouter.h` | 1 | ui-support — Registry fanning the user-selected QAudioDevice to local playback sinks; OS device plumbing, no radio state | unconverted |
| `core/AutomationServer.h` | 1 | ui-support — QLocalServer GUI-automation/test bridge (dumpTree/grab/invoke on QWidgets); dev tooling, stays in gui shell | unconverted |
| `core/BandStackSettings.h` | 4 | universal — Per-radio band-stack memories (freq/mode/filter/AGC/NB-NR) — canonical radio state; only the XML store is client-side | unconverted |
| `core/CallsignInfo.h` | 1 | universal — Callsign lookup result struct (name/QTH/grid, JSON cache form); radio-agnostic spot/logging data. | unconverted |
| `core/CallsignLookupService.h` | 3 | universal — Orchestrates QRZ/cty.dat callsign lookups + cache; radio-agnostic spot/logging integration. | unconverted |
| `core/CallsignUtils.h` | 1 | universal — Stateless callsign parsing/validation helpers; radio-agnostic. | unconverted |
| `core/CatPort.h` | 2 | ui-support — CAT server (TCP+PTY, rigctld/TS-2000/FlexCAT dialects) for external apps; desktop integration, needs a home | unconverted |
| `core/ChannelStripPresets.h` | 1 | universal — Named-preset save/recall/import-export for the engine audio DSP chain; operates on core-profile DSP state. | unconverted |
| `core/ClientComp.h` | 12 | universal — Client-side TX dynamics DSP (compressor/limiter/drive/phase rotator + GR meters); radio-agnostic engine DSP. | unconverted |
| `core/ClientDeEss.h` | 10 | universal — Client TX DSP de-esser (sidechain bandpass + dynamics, meters); radio-agnostic engine DSP chain stage | unconverted |
| `core/ClientEq.h` | 14 | universal — Client-side parametric EQ DSP in AudioEngine (RX/TX paths); radio-agnostic engine DSP, no vendor protocol ties. | unconverted |
| `core/ClientFinalLimiter.h` | 1 | universal — Final-stage brickwall limiter in client TX DSP chain (ceiling/trim/DC-block + meters); radio-agnostic DSP. | unconverted |
| `core/ClientGate.h` | 12 | universal — Client TX-chain downward expander/noise gate DSP (thresh/ratio/hold + meters) — radio-agnostic engine DSP. | unconverted |
| `core/ClientPudu.h` | 11 | universal — Client-side TX audio exciter (PooDoo dual-band DSP) — engine TX DSP chain module, no radio-family coupling | unconverted |
| `core/ClientPuduMonitor.h` | 1 | universal — TX-audio monitor: record/replay post-DSP client TX chain output without keying; radio-agnostic DSP feature | unconverted |
| `core/ClientQuindarTone.h` | 1 | universal — Engine-side TX DSP stage: Quindar tone/Morse intro-outro on PTT; works over any radio's TX audio path. | unconverted |
| `core/ClientReverb.h` | 7 | universal — Client TX-chain Freeverb DSP (size/decay/damping/predelay/mix + meters); radio-agnostic engine DSP feature. | unconverted |
| `core/ClientTube.h` | 11 | universal — Client TX-chain tube saturator DSP (drive/tone/env follower); radio-agnostic engine DSP, no vendor ties. | unconverted |
| `core/ClientTxTestTone.h` | 1 | universal — 1 kHz TX test-tone generator injected at head of client TX DSP chain; radio-agnostic engine DSP feature | unconverted |
| `core/CommandParser.h` | 2 | vendor(flex) — Stateless parser/builder for SmartSDR TCP wire lines (V/H/C/R/S/M, FlexLib semantics) — pure Flex protocol. | unconverted |
| `core/CwCallsignSpotter.h` | 1 | universal — Spots callsigns from the client-side CW decoder stream; radio-agnostic engine feature. | unconverted |
| `core/CwDecoder.h` | 1 | universal — Client-side CW/Morse decoder (ggmorse) over generic 24kHz PCM; radio-agnostic engine DSP feature. | unconverted |
| `core/CwSidetoneGenerator.h` | 2 | universal — Engine-side low-latency CW sidetone DSP driven by keying intent; radio-agnostic (DAX only in comment). | unconverted |
| `core/CwTrace.h` | 4 | ui-support — Header-only helpers minting monotonic ms timestamps + trace IDs for CW keying latency diagnostics, not radio state | unconverted |
| `core/CwxLocalKeyer.h` | 2 | universal — Local CW sidetone keyer: text+WPM in, key-down edges out; radio-agnostic despite Flex 'CWX' naming. | unconverted |
| `core/DaxTxPolicy.h` | 1 | vendor(flex) — Policy deciding when to claim a Flex dax_tx stream vs deferring to SmartSDR DAX2; whole surface is DAX/VITA-49. | unconverted |
| `core/DeviceDiagnostics.h` | 1 | ui-support — Host audio-device diagnostics (Qt device BT/USB heuristics, JSON snapshots for troubleshooting), not radio state | unconverted |
| `core/DvkWavTransfer.h` | 2 | vendor(flex) — DVK WAV upload/download via SmartSDR 'dvk' verbs + ad-hoc TCP port streaming; FlexLib 5MB limit baked in | unconverted |
| `core/DxClusterClient.h` | 3 | universal — Telnet DX cluster spot client (Spider/AR/CC); emits radio-agnostic DxSpot on canonical freq state. | unconverted |
| `core/DxccColorProvider.h` | 2 | universal — DXCC worked-status from ADIF log, colors cluster spots by call/freq/mode; radio-agnostic spot/logging feature | unconverted |
| `core/FirmwareStager.h` | 1 | vendor(flex) — Downloads SmartSDR installers from flexradio.com, extracts .ssdr firmware for 6x00/9600 upload — pure Flex | unconverted |
| `core/FirmwareUploader.h` | 1 | vendor(flex) — SmartSDR firmware upload: 'file upload' cmd, .ssdr files, Flex TCP ports 4995/42607 — pure Flex protocol | unconverted |
| `core/FlexControlManager.h` | 2 | vendor(flex) — FlexControl USB knob serial driver (VID 0x2192); Flex ecosystem hardware but client-side input, home=gui shell | unconverted |
| `core/FreeDvClient.h` | 4 | universal — FreeDV Reporter spot client (qso.freedv.org); radio-agnostic spotting fed by canonical freq/TX + RADE SNR | unconverted |
| `core/GpuSelector.h` | 2 | ui-support — GPU enumeration + persisted QRhi render-adapter choice applied at app startup; pure client rendering plumbing | unconverted |
| `core/HidEncoderManager.h` | 2 | ui-support — USB HID control-surface driver (RC-28, StreamDeck+, TMate 2): desktop input device plumbing, not radio state | unconverted |
| `core/IConnectionAutomation.h` | 1 | ui-support — Gui-free connect/disconnect/dialog hook the automation bridge drives; bridge plumbing, not radio state. | unconverted |
| `core/IambicKeyer.h` | 3 | universal — Radio-agnostic software iambic state machine for local sidetone + CW paddle/keying intent; no vendor coupling. | unconverted |
| `core/KiwiPublicDirectory.h` | 1 | vendor(kiwi) — Fetches/parses kiwisdr.com/public directory + per-sysop ext_api policy; KiwiSDR ecosystem discovery only. | unconverted |
| `core/KiwiSdrClient.h` | 2 | vendor(kiwi) — KiwiSDR WebSocket protocol client (SND/WF streams, ADPCM, camp/monitor states) — the kiwi backend itself | unconverted |
| `core/KiwiSdrManager.h` | 8 | vendor(kiwi) — KiwiSDR connection/profile manager: Kiwi protocol state, telemetry, waterfall/audio streams; vendor extension. | unconverted |
| `core/KiwiSdrProtocol.h` | 9 | vendor(kiwi) — KiwiSDR websocket wire protocol: SND/W/F frame decode, ADPCM, MSG tokens, camp/auth, kiwi command formatting | unconverted |
| `core/LogManager.h` | 21 | ui-support — App-wide diagnostic logging: category registry, log file/retention, runtime toggles. Plumbing, not radio state. | unconverted |
| `core/MacMicPermission.h` | 1 | ui-support — macOS mic permission dialog at app startup; OS permission plumbing, no radio state — belongs in the client app | unconverted |
| `core/MaidenheadLocator.h` | 4 | ui-support — Header-only Maidenhead grid/lat-lon/distance/bearing math; stateless shared utility, not a protocol surface | unconverted |
| `core/MemoryCsvCompat.h` | 1 | vendor(flex) — SmartSDR memory-CSV import/export codec (exact 22-col format); Flex ecosystem interchange, not core protocol. | unconverted |
| `core/MemoryFieldValues.h` | 2 | mixed(flex) — Canonical memory-channel value lists + sanitizers; concepts universal, wire forms mirror FlexLib protocol | unconverted |
| `core/MemoryRecallPolicy.h` | 2 | mixed(flex) — Memory-recall intent (offset/tone math) is core, but builders emit SmartSDR slice command strings (flex) | unconverted |
| `core/MidiControlManager.h` | 3 | ui-support — RtMidi input-device manager (ports, bindings, MIDI Learn) driving client setters; control-surface plumbing | unconverted |
| `core/MidiSettings.h` | 3 | ui-support — MIDI controller binding/profile persistence (XML files) — client input-device config, not radio state. | unconverted |
| `core/MqttAntennaAlias.h` | 3 | ui-support — MQTT topic parsing/queue for antenna-alias pushes from a broker; external integration plumbing, not radio state | unconverted |
| `core/MqttClient.h` | 3 | ui-support — Generic libmosquitto MQTT pub/sub wrapper for external integrations; no radio state, needs a home not a protocol msg | unconverted |
| `core/MqttSettings.h` | 6 | ui-support — Settings store for the MQTT broker bridge (conn config, topics, buttons, keychain); external integration plumbing | unconverted |
| `core/NetRecurrence.h` | 1 | ui-support — Pure RRULE math for net reminders; NetEntry is operator-scoped client state, so aligns with NetScheduler/NetEntry | unconverted |
| `core/NetScheduleStore.h` | 2 | ui-support — Local JSON persistence of NetEntry, which its header calls operator-scoped client state — not radio state | unconverted |
| `core/NetScheduler.h` | 1 | ui-support — Timer engine firing net-reminder alerts from NetEntry list; pure client calendar plumbing, no radio state. | unconverted |
| `core/NetworkPathResolver.h` | 1 | ui-support — Local NIC/IPv4 enumeration + interface-pick helper for connection setup; host networking plumbing, not radio state | unconverted |
| `core/NetworkSettings.h` | 1 | ui-support — AppSettings JSON wrapper persisting VITA-49 SO_RCVBUF tuning; settings storage, not radio state — engine config knob, no protocol msg | unconverted |
| `core/NvidiaAfxPack.h` | 1 | ui-support — Download/install manager for NVIDIA AFX BNR runtime pack (CUDA/TensorRT); deployment plumbing, no radio state | unconverted |
| `core/NvidiaBnrSettings.h` | 1 | ui-support — AppSettings-backed JSON store for Maxine BNR intensity + licence acceptance; persistence, not radio state | unconverted |
| `core/PanadapterStream.h` | 4 | vendor(flex) — SmartSDR VITA-49 UDP receiver (FlexLib PCCs, DAX/IQ routing, SmartLink WAN reg); emits core-profile data | unconverted |
| `core/PerfTelemetry.h` | 3 | ui-support — Singleton perf-instrumentation logger for client render/UDP/input timing; diagnostics, not radio state. | unconverted |
| `core/PeripheralSettings.h` | 3 | ui-support — Client settings blob (AutoReconnect + legacy-key migration) for peripheral devices atop AppSettings; no radio state. | unconverted |
| `core/PgxlConnection.h` | 2 | vendor(flex) — Direct TCP client (port 9008) for 4O3A Power Genius XL amp telemetry — Flex-ecosystem accessory protocol. | unconverted |
| `core/PipeWireAudioBridge.h` | 3 | mixed(flex) — Linux virtual-audio bridge to WSJT-X etc.; audio routing is core, DAX channel model/rates are flex | unconverted |
| `core/PotaClient.h` | 2 | universal — POTA spot poller (api.pota.app) emitting DxSpot frequency/mode spots; radio-agnostic spot feature, no vendor ties. | unconverted |
| `core/ProfileTransfer.h` | 1 | vendor(flex) — SmartSDR .ssdr_cfg database import/export over Flex TCP transfer protocol (meta_subset, ports 4995/42607) | unconverted |
| `core/PropForecastClient.h` | 3 | ui-support — Web client fetching NOAA SWPC/hamqsl propagation data for dashboards; no radio state, external integration | unconverted |
| `core/PskReporterClient.h` | 1 | ui-support — pskreporter.info HTTP/MQTT fetcher for reception reports of our call; external service feed for a map dialog, no radio state | unconverted |
| `core/QrzLookupSettings.h` | 1 | ui-support — QRZ credential/settings holder (keychain-backed); client-side settings plumbing, not radio state. | unconverted |
| `core/QsoRecorder.h` | 1 | universal — QSO WAV recorder gated by MOX using canonical slice freq/mode + audio streams; no vendor protocol ties. | unconverted |
| `core/RADEEngine.h` | 4 | universal — Engine-side RADE/FreeDV digital-voice codec (PCM in/out, EOO, sync/SNR); DAX mentions are just audio plumbing | unconverted |
| `core/RadioConnection.h` | 2 | vendor(flex) — SmartSDR TCP text-protocol connection (commands/replies/status) — Flex wire protocol, per calibration anchor | unconverted |
| `core/RadioDiscovery.h` | 3 | mixed(flex) — Device-discovery list/events are core-profile; SmartSDR UDP:4992 parsing + Multi-Flex/license fields are flex | unconverted |
| `core/ReceivePresentationSync.h` | 1 | mixed(flex) — Cross-source RX latency sync (queues + GCC-PHAT); mechanism is core, but API hardcodes Flex/KiwiSdr pair | unconverted |
| `core/RttyDecoder.h` | 1 | universal — Radio-agnostic RTTY (Baudot) DSP decoder over generic 24 kHz PCM; engine-side digital-mode decode feature. | unconverted |
| `core/SerialPortController.h` | 2 | ui-support — USB-serial DTR/RTS PTT + CTS/DSR/DCD key/paddle input; local hardware I/O device, emits intent only | unconverted |
| `core/SettingsHelpers.h` | 2 | ui-support — QSlider live/persist debounce wiring for AppSettings saves; pure Qt client plumbing, no radio state. | unconverted |
| `core/ShortcutManager.h` | 4 | ui-support — Keyboard shortcut registry: QShortcut bindings, persistence, conflict checks — client input plumbing, no radio state. | unconverted |
| `core/SignalClassifier.h` | 1 | universal — ONNX CNN voice/carrier classifier over spectrogram patches; radio-agnostic engine DSP/analysis feature | unconverted |
| `core/SmartLinkClient.h` | 3 | vendor(flex) — SmartLink WAN client: FlexRadio Auth0 login + TLS to smartlink.flexradio.com, WAN radio list/hole-punch. | unconverted |
| `core/SpectrogramBuffer.h` | 1 | universal — Ring buffer of FFT frames per panadapter feeding CNN classifier patches; pure spectrum data, radio-agnostic. | unconverted |
| `core/SpotCollectorClient.h` | 2 | ui-support — UDP listener for DXLab SpotCollector desktop app; external integration feeding DxSpot, not radio state | unconverted |
| `core/SpotCommandPolicy.h` | 4 | ui-support — Settings-backed passive-spots toggle gating whether client emits spot-add cmds; pure AppSettings policy, no radio state | unconverted |
| `core/SpotModeResolver.h` | 4 | universal — Maps DX-cluster spot mode/comment/band-plan to canonical radio mode; pure spot-to-state logic, no vendor ties. | unconverted |
| `core/StreamStatus.h` | 1 | vendor(flex) — SmartSDR stream-status parsing: client_handle ownership, DAX RX/TX orphan-stream firmware quirks — pure Flex wire logic | unconverted |
| `core/SupportBundle.h` | 1 | ui-support — Diagnostics bundle: archives logs/sysinfo and opens email client; client-side support tooling, not radio state | unconverted |
| `core/TciServer.h` | 3 | mixed(flex) — TCI WebSocket server for WSJT-X et al: protocol surface is canonical radio state, but audio/IQ rides Flex DAX | unconverted |
| `core/TgxlConnection.h` | 2 | vendor(flex) — Direct TCP client for 4O3A Tuner Genius XL port-9010 protocol (relay/autotune); Flex-ecosystem accessory. | unconverted |
| `core/ThemeManager.h` | 119 | ui-support — Qt token-based theming singleton (colors/fonts/QSS, theme files, editor hooks) — pure client GUI plumbing, no radio state. | unconverted |
| `core/TxKeyingMarker.h` | 4 | ui-support — QWidget property marker guarding TX-keying controls from the automation bridge; GUI-shell plumbing, no radio state. | unconverted |
| `core/UlanziDialBackend.h` | 3 | ui-support — Platform alias for Ulanzi Dial HID knob backend (evdev/hidapi); physical input device for client, not radio state | unconverted |
| `core/UpdateChecker.h` | 3 | ui-support — App self-update checker polling GitHub releases API; pure client plumbing, no radio state — belongs in gui shell. | unconverted |
| `core/VersionNumber.h` | 3 | ui-support — Semver parse/compare utility used by update checker and What's New dialog; app plumbing, not radio state. | unconverted |
| `core/VirtualAudioBridge.h` | 3 | mixed(flex) — POSIX-shm virtual audio device bridge feeding digi-mode apps; routing is generic, semantics are Flex DAX | unconverted |
| `core/VoiceSignalDetector.h` | 1 | universal — Engine-side DSP: detects voice signals in FFT bins using band-plan segments; radio-agnostic spectrum analysis | unconverted |
| `core/WanConnection.h` | 2 | vendor(flex) — SmartLink TLS transport speaking SmartSDR V/H/R/S/M protocol with wan validate handshake + TOFU cert pinning | unconverted |
| `core/WaveformInstaller.h` | 1 | vendor(flex) — Uploads Docker waveform images via SmartSDR fw 4.2.18 'file upload' TCP protocol — Flex-only ecosystem. | unconverted |
| `core/WfmDemodulator.h` | 1 | mixed(flex) — WFM demod around WfmDsp: demod/Doppler-offset intent is core; IQ source is DAX IQ + SmartSDR cmds (flex) | unconverted |
| `core/WfmSettings.h` | 1 | ui-support — Client-side settings blob (AppSettings JSON) storing WFM audio output device id + legacy-key migration. | unconverted |
| `core/WsjtxClient.h` | 2 | ui-support — UDP listener for local WSJT-X app decodes/status; desktop-side integration feeding the universal spot surface | unconverted |
| `core/aprs/AprsBeacon.h` | 1 | universal — APRS position/status beacon composition; radio-agnostic packet operating feature. | unconverted |
| `core/aprs/AprsMessenger.h` | 2 | universal — APRS messaging (send/ack/retry) over canonical TX path; radio-agnostic. | unconverted |
| `core/aprs/AprsPacket.h` | 2 | universal — APRS/AX.25 packet parse+encode data types; radio-agnostic. | unconverted |
| `core/aprs/AprsSettings.h` | 2 | ui-support — APRS client settings holder (callsign/SSID/paths); client-side settings, not radio state. | unconverted |
| `core/aprs/AprsStationList.h` | 1 | universal — Heard-APRS-station model (calls/positions/last-heard); radio-agnostic spot-like data. | unconverted |
| `core/pms/PmsMailbox.h` | 1 | universal — Packet personal-message-system mailbox store/logic; radio-agnostic operating feature. | unconverted |
| `core/tnc/AetherAx25LibmodemShim.h` | 1 | universal — AX.25 modem shim bridging the client AFSK/libmodem demod to the TNC; radio-agnostic DSP glue. | unconverted |
| `core/tnc/Ax25.h` | 1 | universal — AX.25 frame data types/constants; radio-agnostic protocol layer. | unconverted |
| `core/tnc/Ax25FrameFormatter.h` | 1 | universal — AX.25 frame human-formatting; radio-agnostic. | unconverted |
| `core/tnc/HeardList.h` | 1 | universal — Heard-station list for the packet monitor; radio-agnostic. | unconverted |
| `core/tnc/KissTncServer.h` | 1 | ui-support — KISS-over-TCP server exposing the TNC to external apps; external integration, needs a home (cf CatPort/TciServer). | unconverted |
| `core/tnc/TncTerminal.h` | 1 | universal — Packet terminal session model (command/monitor); radio-agnostic operating feature. | unconverted |
| `models/AntennaGeniusModel.h` | 4 | vendor(flex) — 4O3A Antenna Genius switch client: UDP discovery + SmartSDR-like TCP protocol; Flex-ecosystem accessory | unconverted |
| `models/BandDefs.h` | 4 | universal — Static ARRL band plan table (edges, default freq/mode, GEN/WWV); canonical band-plan data, no vendor ties. | unconverted |
| `models/BandPlanManager.h` | 7 | universal — Band-plan overlay data (segments/spots/license classes, region merge) from JSON; radio-agnostic canon | unconverted |
| `models/BandSettings.h` | 4 | universal — Per-band save/restore of canonical state (freq/mode/filter/AGC/WNB/display range) — band memories, no vendor fields | unconverted |
| `models/CwxModel.h` | 1 | universal — CW keyer intent: WPM/delay/QSK, 12 macros, send/erase, sent-index progress. Generic despite Flex 'CWX' name. | unconverted |
| `models/DaxIqModel.h` | 1 | vendor(flex) — Flex DAX IQ streams: dax_iq stream create/rate cmds, 4-ch DAX model, pipes to SDR apps — DAX is Flex-only | unconverted |
| `models/DvkModel.h` | 1 | mixed(flex) — Voice keyer slots/commands are core-profile; status parsing + FlexLib SsdrErrors mapping are flex. | unconverted |
| `models/EqualizerModel.h` | 2 | universal — 8-band TX/RX audio EQ state (enable + band gains) — core capability; SmartSDR wire parsing/cmds move to flex adapter | unconverted |
| `models/FlexWaveformModel.h` | 1 | vendor(flex) — FlexLib waveform/WFP container management: parses SmartSDR waveform status, emits Flex protocol commands | unconverted |
| `models/MemoryEntry.h` | 1 | universal — POD memory-channel record (freq/mode/offset/tone/squelch/filter/digital offsets) — canonical memories surface | unconverted |
| `models/MeterModel.h` | 6 | mixed(flex) — Meter registry+values are core-profile; VITA-49 raw scaling, TGXL/PGXL amp routing, COMPPEAK quirks are Flex | unconverted |
| `models/NetEntry.h` | 2 | ui-support — Net scheduler entry: RRULE/timezone/reminder metadata, operator-scoped local JSON; radio state only via embedded MemoryEntry | unconverted |
| `models/PanadapterModel.h` | 2 | mixed(flex) — Per-pan display state is core-profile; DAX IQ ch, client_handle, VITA stream IDs, SmartSDR kv parsing are flex. | unconverted |
| `models/ProfileLoadCommand.h` | 1 | vendor(flex) — Parses SmartSDR 'profile global/tx/mic load' wire command + recall-hold timing/suppression sentinels; Flex-only | unconverted |
| `models/RadioModel.h` | 39 | mixed(flex) — Central radio aggregate: core slice/pan/TX/meter/memory state fused with Flex protocol, DAX, SmartLink, Multi-Flex | unconverted |
| `models/RadioSession.h` | 1 | universal — Per-radio session aggregate: owns RadioModel + id/label; session concept is core-profile, no vendor surface | unconverted |
| `models/RadioStatusOwnership.h` | 1 | vendor(flex) — SmartSDR status parsing helpers: Flex hex handles, client_handle ownership, remote_audio_rx, interlock gate | unconverted |
| `models/SliceModel.h` | 28 | mixed(flex) — Slice state (freq/mode/filter/DSP) is core-profile; DAX, index_letter, SmartSDR status KVs are flex ext | unconverted |
| `models/SpotModel.h` | 1 | universal — Panadapter spot store (callsign/freq/mode/lifetime/priority) on canonical state; kv ingest is trivially generic | unconverted |
| `models/TnfModel.h` | 1 | universal — Tracking notch filter state (freq/width/depth/permanent, global enable) — generic DSP notch surface; kv parse is transport detail | unconverted |
| `models/TransmitModel.h` | 14 | mixed(flex) — TX state model: power/MOX/VOX/CW/filter are core-profile; ATU, DAX, APD, profiles, interlock are Flex. | unconverted |
| `models/TunerModel.h` | 3 | vendor(flex) — 4o3a TGXL tuner state/commands via SmartSDR 'atu'/'tgxl' TCP verbs + direct port-9010 relay control | unconverted |
| `models/XvtrPolicy.h` | 5 | mixed(flex) — XVTR policy: transverter list/freq translation is core; waterfall-tile offset + FLEX model power clamps are flex | unconverted |

**Tag legend:** `universal` = core-profile surface every radio family has; `vendor` = FlexRadio/SmartSDR-specific, becomes a namespaced extension; `mixed` = header carries both (split candidates noted); `ui-support` = not radio state at all (settings, theming, app plumbing) — needs a home decision, not a protocol message.
