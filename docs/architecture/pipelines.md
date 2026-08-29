# AetherSDR — Thread Architecture & Data Pipelines

Extracted from CLAUDE.md for on-demand reference. Read this when debugging
thread safety, signal routing, or data flow issues.

For the authoritative audio ordering, sample formats, channel handling,
downmixing, metering taps, and VITA/Opus packetization details, see
[AetherSDR Audio Pipeline](audio-pipeline.md). This page is a thread and
high-level routing overview; it intentionally omits many audio-stage details.

### Data Pipelines

Multi-thread architecture — up to 12 threads depending on features enabled:
- **Main thread**: GUI rendering (paintEvent), RadioModel + all sub-models, user input
- **Connection thread**: RadioConnection (TCP 4992 I/O, kernel TCP_INFO RTT)
- **Audio thread**: AudioEngine (RX/TX audio; NR2/RN2/NR4/DFNR/BNR/MNR DSP, QAudioSink/Source)
- **Network thread**: PanadapterStream (VITA-49 UDP parsing, FFT/waterfall/meter demux)
- **ExtControllers thread**: FlexControl, MIDI, SerialPort (USB/serial I/O, RtMidi callbacks)
- **Spot thread**: DxCluster, RBN, WSJT-X, POTA, FreeDV spot clients
- **CwDecoder thread**: ggmorse decode loop (QThread::create, on-demand)
- **DAX IQ thread**: DaxIqModel worker (byte-swap + pipe I/O)
- **AX.25 TNC thread**: AetherAx25LibmodemShim worker (HDLC/AX.25 + AFSK decode, on-demand when the AetherModem/packet tab is active)
- **RADE thread**: RADEEngine neural encoder/decoder (on-demand, HAVE_RADE)
- **BNR**: NvidiaAfxFilter — in-process NVIDIA Maxine AFX GPU denoiser (runtime-loaded, HAVE_NVIDIA_AFX; runs inline on the audio thread, no dedicated thread)
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
    ▼                  ▼     │    ├─→ TunerApplet     ├─→ BNR (NvidiaAfxFilter)
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
      device-rate Int16 → canonical mono → 48 kHz float normalization
        → RN2 → test tone → client TX DSP chain → PC mic gain
        → Quindar → final limiter → 48-to-24 kHz SRC
        → linked TPDF + Int16 quantization → monitors/meters/scopes
              │
              ├─→ backend TX seam (HL2/Icom, 24 kHz Int16)
              │       └─→ backend-native conversion/modulation
              │
              └─→ Flex Opus PCC 0x8005 / uncompressed PCC 0x03E3
                              └─→ [queued to NETWORK]
                                  PanadapterStream.sendToRadio()
                                      └─→ Radio UDP 4991

The PSK Reporter WSPR beacon is a one-shot, operator-armed digital source. A
precise timer on the AudioEngine worker thread generates sample-accurate 4-FSK
independently of microphone callbacks. It uses a client-owned `dax_tx` stream
on Flex and the normalized transmit-audio seam on Icom/HL2, in DIGU. Its
dedicated non-voice PTT source suppresses Quindar, and the generator holds
silence through the unkey edge.

Four conventions in that generator are taken from WSJT-X's `Modulator.cpp`
rather than invented, because a WSPR frame is judged by decoders we do not own:

- **Tone 0 sits at the requested audio frequency**, with the constellation
  running upward from it (`m_frequency + itone[isym] * m_toneSpacing`). Centring
  the four tones on it instead shifts every resulting spot 2.2 Hz low.
- **Both ends of the frame are tapered** over 0.017 symbols (11.6 ms). WSJT-X
  fades only the tail; the head ramp is ours. Phase is continuous across symbol
  boundaries, so these are the only two discontinuities in 111.6 s, and an
  untapered one is a broadband click in a 200 Hz-wide shared sub-band.
- **The lead-in is referenced to the slot**, not to whenever the pump started,
  so scheduling latency is absorbed rather than reported as DT.
- **A missed boundary truncates the head** of the frame instead of sliding the
  whole transmission later.

The beacon also borrows station state that is not slice state and hands it back
on stop. Arming takes only the transmit passband. Everything else is taken at
the KEY, in `reassertBeaconChannel()`: the slice mode, both passbands again, the
speech processor, the compander, the TX equalizer and VOX. None of the audio
four is bypassed by selecting DIGU and all of them misshape a constant-envelope
tone; VOX is taken because it can unkey part-way through a 111.6 s frame.

The key, not arm, is where those belong for one reason: arming happens up to
120 s ahead of the key — up to ~6 minutes once the permitted deferrals are
spent — so anything pushed at arm is stale by the time it matters. A cross-band
`slice tune` triggers an asynchronous band-stack recall that can land after the
mode and filter sent behind it (§6.2 of the FlexLib oracle; #2824), and another
client, a band button or a profile load can move the slice in that window. The
same staleness argument applies to the speech chain, which is additionally why
a station that is merely *armed* keeps its own audio processing and VOX intact.

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
| **AX.25 TNC** | AetherAx25LibmodemShim worker | ~0% | moveToThread | HDLC/AX.25 + AFSK decode; on-demand when the packet tab is active |
| **DXCC** | DxccColorProvider ADIF parser | ~0% | moveToThread | One-shot at startup |
| **RADE** | RADEEngine neural encoder/decoder | ~0% | moveToThread | On-demand, HAVE_RADE |
| **BNR** | NvidiaAfxFilter in-process AFX denoiser | varies | audio thread | local NVIDIA GPU, HAVE_NVIDIA_AFX |

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
