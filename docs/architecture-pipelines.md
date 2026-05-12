# AetherSDR — Thread Architecture & Data Pipelines

Extracted from CLAUDE.md for on-demand reference. Read this when debugging
thread safety, signal routing, or data flow issues.

For the authoritative audio ordering, sample formats, channel handling,
downmixing, metering taps, and VITA/Opus packetization details, see
[AetherSDR Audio Pipeline](audio-pipeline.md). This page is a thread and
high-level routing overview; it intentionally omits many audio-stage details.

### Data Pipelines

Multi-thread architecture — up to 11 threads depending on features enabled:
- **Main thread**: GUI rendering (paintEvent), RadioModel + all sub-models, user input
- **Connection thread**: RadioConnection (TCP 4992 I/O, kernel TCP_INFO RTT)
- **Audio thread**: AudioEngine (RX/TX audio; NR2/RN2/NR4/DFNR/BNR/MNR DSP, QAudioSink/Source)
- **Network thread**: PanadapterStream (VITA-49 UDP parsing, FFT/waterfall/meter demux)
- **ExtControllers thread**: FlexControl, MIDI, SerialPort (USB/serial I/O, RtMidi callbacks)
- **Spot thread**: DxCluster, RBN, WSJT-X, POTA, FreeDV spot clients
- **CwDecoder thread**: ggmorse decode loop (QThread::create, on-demand)
- **DAX IQ thread**: DaxIqModel worker (byte-swap + pipe I/O)
- **RADE thread**: RADEEngine neural encoder/decoder (on-demand, HAVE_RADE)
- **BNR thread**: NvidiaBnrFilter gRPC async I/O (std::thread, HAVE_BNR)
- **DXCC parse thread**: DxccColorProvider ADIF log parser (one-shot at startup)

```
┌─────────────────────────────────────────────────────────────────────┐
│                      NETWORK LAYER                                  │
│                                                                     │
│  Radio UDP 4992 ──→ RadioDiscovery ──→ ConnectionPanel    [MAIN]    │
│  Radio TCP 4992 ──→ RadioConnection ──→ RadioModel        [CONN]→[MAIN] │
│  Radio UDP 4991 ──→ PanadapterStream (VITA-49 demux)      [NETWORK] │
│  TGXL  TCP 9010 ──→ TgxlConnection ──→ TunerModel        [MAIN]    │
│  WAN   TLS 4992 ──→ WanConnection ──→ RadioModel          [MAIN]    │
│  WAN   UDP 4993 ──→ PanadapterStream                      [NETWORK] │
└─────────────────────────────────────────────────────────────────────┘
                              │
                    ┌─────────┴─────────┐
                    │  PanadapterStream  │  ◄── NETWORK THREAD
                    │  (VITA-49 demux)   │      QMutex guards stream IDs
                    └──┬──┬──┬──┬──┬────┘
                       │  │  │  │  │
    ┌──────────────────┘  │  │  │  └──────────────────┐
    ▼ [queued]            ▼  │  ▼ [queued]            ▼ [queued]
┌────────┐         ┌────────┐│┌────────┐        ┌──────────┐
│PCC 8003│         │PCC 8004│││PCC 8002│        │PCC 03E3/ │
│FFT bins│         │WF tiles│││ meters │        │0123/8005 │
└───┬────┘         └───┬────┘│└───┬────┘        └────┬─────┘
    │                  │     │    │                   │
    ▼ MAIN             ▼     │    ▼ MAIN              ▼ AUDIO THREAD
SpectrumWidget   SpectrumWidget  MeterModel     AudioEngine
.updateSpectrum  .updateWfRow    .updateValues   .feedAudioData
    │                  │     │    │                   │
    │ (ring buffer)    │     │    ├─→ SMeterWidget    ├─→ NR2 (SpectralNR)
    │ + NB blanker     │     │    ├─→ TxApplet        ├─→ RN2 (RNNoiseFilter)
    ▼                  ▼     │    ├─→ TunerApplet     ├─→ BNR (NvidiaBnrFilter)
  paintEvent()    paintEvent │    └─→ StatusBar       ├─→ CwDecoder [MAIN]
  (~98% CPU)                 │                        ▼
                             │                   QAudioSink
                             │                   (speakers)
                             │
                      ┌──────┴──────┐
                      │ DAX streams │
                      └──────┬──────┘
                             ▼ MAIN
                     VirtualAudioBridge / PipeWireAudioBridge / TCI / RADE
                     (virtual devices, digital apps, modem paths)

TX AUDIO ROUTING SUMMARY:                  ◄── AUDIO THREAD
  QAudioSource (PC mic) ──→ AudioEngine.onTxAudioReady()
                              │
              ┌───────────────┼────────────────┐
              ▼               ▼                ▼
      PC mic voice TX     DAX TX mode       RADE mode
              │               │                │
              │               │                ├─→ txRawPcmReady()
              │               │                └─→ RADEEngine.feedTxAudio()
              │               │                       │
              │               │                       ▼
              │               │                AudioEngine.sendModemTxAudio()
              │               │                VITA PCC 0x03E3
              │               │
              │               └─ PC mic handler returns;
              │                  DAX/TCI enters AudioEngine.feedDaxTxAudio()
              │                  and bypasses client voice DSP
              │                       │
              │                       ├─→ low-latency VITA PCC 0x03E3
              │                       └─→ radio-native VITA PCC 0x0123
              │
              ▼
      test tone → client TX DSP chain → post-DSP monitor
        → PC mic gain → Quindar → final limiter → meters/scopes
              │
              ├─→ Opus remote_audio_tx VITA PCC 0x8005
              └─→ uncompressed VITA fallback PCC 0x03E3
                              │
                              ▼ [queued to NETWORK]
                      PanadapterStream.sendToRadio()
                              │
                              ▼
                         Radio UDP 4991

TCP COMMAND PIPELINE (bidirectional):
  GUI widget ──→ SliceModel.setXxx() ──→ emit commandReady("slice ...")  [MAIN]
                 TransmitModel          ──→ emit commandReady("xmit ...")
                 TunerModel             ──→ emit commandReady("tgxl ...")
                 EqualizerModel         ──→ emit commandReady("eq ...")
                 TnfModel               ──→ emit commandReady("tnf ...")
                        │
                        ▼
                 RadioModel.sendCmd(cmd)                                  [MAIN]
                   │ stores callback in m_pendingCallbacks
                   │ allocates sequence number
                   ▼ [QMetaObject::invokeMethod → queued to CONN thread]
              ┌─────────────────┐
              │ RadioConnection  │  ◄── CONNECTION THREAD
              │ writeCommand()   │      heap-allocated, moveToThread
              │ QTcpSocket       │      init() creates socket on thread
              │ ping RTT timer   │      measures RTT at socket read time
              └────────┬────────┘
                       │
              ┌────────┴────────┐
              ▼                 ▼
         QTcpSocket       WanConnection    ◄── WAN stays on MAIN (TLS)
              │                 │
              └────────┬────────┘
                       ▼
                     Radio

  Radio ──→ "S<handle>|<object> key=val ..."
              │
              ▼ CONNECTION THREAD
        RadioConnection.processLine()
              │ emits statusReceived, commandResponse
              ▼ [auto-queued signal to MAIN]
        RadioModel.onStatusReceived()                                    [MAIN]
              │
              ├─→ SliceModel.applyStatus()      ──→ GUI signals
              ├─→ PanadapterModel.applyStatus()  ──→ SpectrumWidget
              ├─→ TransmitModel.applyStatus()    ──→ TxApplet
              ├─→ TunerModel.applyStatus()       ──→ TunerApplet
              ├─→ EqualizerModel.applyStatus()   ──→ EqApplet
              ├─→ MeterModel.registerMeter()     ──→ meter definitions
              └─→ Multi-Flex client tracking     ──→ TitleBar badge

        RadioModel.onCommandResponse()                                   [MAIN]
              │ looks up callback by sequence number
              └─→ invokes callback on main thread (safe model access)

EXTERNAL CONTROL PIPELINES:                 ◄── EXTCONTROLLERS THREAD
  FlexControl (USB serial) ──→ FlexControlManager ──┐
  MIDI (RtMidi)            ──→ MidiControlManager  ──┼─→ [auto-queued signals]
  SerialPort (PTT/CW)     ──→ SerialPortController ──┘       │
                                                              ▼ MAIN THREAD
                                                     MainWindow dispatches:
                                                       ├─→ RadioModel (tune, TX, CW)
                                                       ├─→ SliceModel (freq, gain, DSP)
                                                       └─→ AudioEngine (mute, mic gain)
  TGXL (TCP 9010)          ──→ TgxlConnection      ──→ TunerModel relay adjust [MAIN]

SPOT PIPELINES:                             ◄── SPOT WORKER THREAD
  DX Cluster (telnet)  ─┐
  RBN (telnet)         ─┤
  WSJT-X (UDP mcast)  ─┼──→ SpotModel (batched 1/sec) ──→ SpectrumWidget spots
  POTA (HTTP polling)  ─┤     + DxccColorProvider (ADIF lookup)
  FreeDV (WebSocket)   ─┘
```

**Thread summary:**

| Thread | Components | CPU | Creation | Notes |
|--------|-----------|-----|----------|-------|
| **Main** | QRhi render(), RadioModel, all sub-models, all GUI widgets | ~28% | Qt default | GPU waterfall + FFT; QPainter overlay cached |
| **Connection** | RadioConnection, QTcpSocket, kernel TCP_INFO RTT | ~0% | moveToThread | Heap-allocated, init() slot pattern |
| **Audio** | AudioEngine, NR2/RN2 DSP, QAudioSink/Source, TX encoding | ~1.5% | moveToThread | std::atomic flags, recursive_mutex for DSP lifecycle |
| **Network** | PanadapterStream, QUdpSocket, VITA-49 parsing, per-stream stats | ~0.3% | moveToThread | QMutex guards stream ID sets |
| **ExtControllers** | FlexControlManager, MidiControlManager, SerialPortController | ~0% | moveToThread | USB serial I/O, RtMidi, poll timers |
| **Spot** | DxCluster, RBN, WSJT-X, POTA, FreeDV clients | ~0% | moveToThread | Batched 1/sec forwarding |
| **CwDecoder** | ggmorse decode loop | ~0% | QThread::create | On-demand start/stop per CW mode |
| **DAX IQ** | DaxIqModel worker | ~0% | moveToThread | Byte-swap + pipe I/O |
| **DXCC** | DxccColorProvider ADIF parser | ~0% | moveToThread | One-shot at startup |
| **RADE** | RADEEngine neural encoder/decoder | ~0% | moveToThread | On-demand, HAVE_RADE |
| **BNR** | NvidiaBnrFilter gRPC async I/O | ~0% | std::thread | GPU container, HAVE_BNR |

**Cross-thread signals (auto-queued):**
- Connection → Main: statusReceived, messageReceived, commandResponse, pingRttMeasured
- Main → Connection: writeCommand (via QMetaObject::invokeMethod), connectToRadio, disconnectFromRadio
- Network → Main: spectrumReady, waterfallRowReady, meterDataReady, daxAudioReady
- Network → Audio: audioDataReady
- Audio → Network: txPacketReady (→ sendToRadio)
- Audio → Main: levelChanged, pcMicLevelChanged, nr2/rn2/bnrEnabledChanged
- Audio → CwDecoder: feedAudio (lock-free ring buffer)
- Main → Audio: setNr2/Rn2/BnrEnabled (via QMetaObject::invokeMethod)
- Main → Audio: startRxStream/stopRxStream (via helper methods)
- Main → Network: registerPanStream, setDbmRange (QMutex-protected setters)
- ExtControllers → Main: tuneSteps, buttonPressed, externalPttChanged, cwKeyChanged, paramAction
- Main → ExtControllers: setTransmitting, loadSettings, open/close (via QMetaObject::invokeMethod)
- CwDecoder → Main: textDecoded, statsUpdated (auto-queued)

**Design principle:** Everything except GUI rendering and model dispatch runs
on a dedicated worker thread. RadioModel owns all sub-models as value members
on the main thread — GUI accesses models directly with no pointer indirection.
Each worker thread has a single responsibility and communicates exclusively via
auto-queued signals. The main thread handles only paintEvent + model updates.

**GPU-accelerated rendering (#391):** When `AETHER_GPU_SPECTRUM=ON` (default),
`SpectrumWidget` inherits `QRhiWidget` instead of `QWidget`. The waterfall is
a GPU texture with incremental row uploads (~6KB/frame via ring buffer offset
in fragment shader). The FFT spectrum is a vertex buffer with per-vertex heat
map colors (blue→cyan→green→yellow→red). Overlays (grid, band plan, scales,
markers) are painted by QPainter into a cached QImage, uploaded as a texture
only when state changes. Main thread CPU reduced from ~97% to ~28%.

**Key QRhi lesson:** `fract()` must be in the fragment shader, not vertex
shader. Per-vertex `fract()` when UV spans 0→1 produces identical values
at both vertices (`fract(0+offset) == fract(1+offset)`), resulting in
constant UV across the quad.
