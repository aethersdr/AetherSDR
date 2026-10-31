# AetherSDR Roadmap

Live tracking lives in [GitHub Issues](https://github.com/aethersdr/AetherSDR/issues)
and the per-cycle milestone view. This file is a human-readable snapshot
of what the project lead and core contributors are working on — updated
as direction changes.

For *what shipped*, see [`CHANGELOG.md`](CHANGELOG.md).

## Current cycle: post-v26.10.4

### In flight

- **aetherd — vendor-neutral radio backend** ([RFC #3849](https://github.com/aethersdr/AetherSDR/issues/3849), approved) — extracting an
  `IRadioBackend` seam (`RadioCapabilities` + typed status/command deltas)
  so radio-family logic lives behind a stable interface instead of being
  woven through `RadioModel`. FlexBackend owns the Flex wire objects
  and threads, and the Panadapter / Slice / Meter / Transmit / Amp / Tuner
  status+command paths decode behind the seam (RFC steps 2.1–2.4). The seam
  now carries **six** backends — `FlexBackend`, `HL2Backend`, `IcomCIV`,
  `AnanBackend`, `RtlSdrBackend`, and the synthetic `SimBackend` — which is what
  took it from a design to a proven interface. Bringing a third vendor up on it in v26.8.2 was also the seam's
  best audit to date: it surfaced a meter path that ignored its own unit,
  receive-DSP controls with no verb behind them, and a capability conflating
  "the host modulates" with "TX audio leaves through the seam". The versioned protocol (RFC step 3+) has
  since landed in increments: v26.9.3 added **Stage 3** capability-qualified
  local receive control (mode, filter, audio gain and mute, panadapter center
  and bandwidth) with bounded read-only telemetry, and opened **Stage 4** with
  an engine-local `TxCoordinator` for primary desktop transmit intent. The
  daemon stays observe-only unless `--allow-local-control` is passed. v26.9.4
  continued Stage 4: desktop TX producer ownership survives queued work, and
  **credential-bound TX grants** bind independent clients to actors on the
  `TxCoordinator` behind `--allow-local-tx` and a native-vault credential
  authority that fails closed, with a qualified software-PTT handoff for Flex
  radios on SmartSDR TCP API 1.4 over LAN. Startup remains disarmed. v26.9.5
  began the #5554 physical relocation: the Flex wire classes now live in
  `src/core/backends/flex/`, and `RadioConnection` no longer includes the
  simulator. The seam's probe table is generated from `IRadioBackend.h`, so a
  declared signal cannot go unprobed. v26.10.1 moved the slice receive
  controls behind the seam (#5262 M4): frequency, mode, filter, AGC, AF gain,
  mute and balance, NB, NR, ANF, manual notch, APF, squelch, RX antenna and
  slice lock travel as typed backend requests, receive contracts are seeded
  across the families, and with the GUI callers rerouted the raw Flex command
  count above the seam fell from 413 to 369. v26.10.2 kept the new per-radio
  differences in declared capability records rather than family checks — a
  line-out control record, client-persisted FFT FPS and dBm range — and carried
  WDSP's second noise blanker as a typed blanker kind on the receive route.
  v26.10.3 moved the remaining slice/transmit model controls, Radio Setup, TX
  Band, transverter and USB configuration, and profile/memory operations onto
  typed requests, with original-session and owner fencing. v26.10.4 moved
  spots, physical outputs, pan zoom and RX antenna, tuning step and net
  corrections (raw Flex commands above the seam 215 → 192), then slice
  lifecycle, split routing, TX-slice assignment and full duplex, then
  panadapter/waterfall lifecycle and every existing display control. These are
  scoped M4 increments, not completion of the daemon or the thin UI. Remaining:
  per-client propagation, transmit for SmartLink and the other families,
  transmit audio transport, and a replacement thin UI client — UI code still
  consumes models directly, and that remains correct until that client exists.
- **Icom networked radios — early; the IC-7300MK2 is supported** — `IcomCIV` speaks CI-V inside the RS-BA1
  UDP transport, brought up in v26.8.2 against a live **IC-705** (RX, scope,
  transmit, and FT8 both decoding and spotting on PSK Reporter) and an
  **IC-7300** (RX, scope and stability; transmit unverified). Only the IC-705
  and IC-7300MK2 are `verified` against their own CI-V guides ([RFC #5517](https://github.com/aethersdr/AetherSDR/issues/5517),
  approved, promotes the IC-7300MK2 to Supported); an unknown model
  gets no scope and no transmit rather than optimistic defaults. v26.8.3 gave
  the backend a **command plane**: every meter read, control write,
  reconciliation poll and PTT transition goes through one CI-V scheduler with
  explicit priorities, coalescing and stale-reply rejection — written because a
  delayed PTT-OFF reply arriving after a newer PTT-ON was cutting transmit
  audio. It also completed the **IC-7300MK2** control surface (18
  operator-visible defects), fixed the **RS-BA1 lease renewal** that froze the
  panadapter at the 255→256 sequence boundary, made **DATA mode** actually reach
  the radio for DIGU/DIGL and DFM, and replaced the hardcoded `0xA4` connect
  address with a broadcast `19 00` query. **WSPR** transmits (20 PSK Reporter
  reception reports on the air), **PC Audio** switches the model-specific DATA OFF
  modulation input, and the built-in CW decoder opens on normalized `CWU`.
  v26.9.5 implemented RFC #5517 in the app: an IC-7300MK2 identified by CI-V
  over built-in Ethernet/RS-BA1 connects as a **supported** radio, with no
  experimental badge or disclaimer, while every other model keeps the
  experimental treatment. v26.10.1 applied TUNE power live while TUNE is
  keyed, kept an RF power change made during TUNE, stopped VOX, the monitor,
  the speech processor, CW speed and break-in raising a false drop notice, and
  corrected the IC-9700, IC-7610 and IC-785x to advertise the one panadapter
  the backend implements. v26.10.2 fixed four IC-7300MK2 control defects that
  each had a passing backend test — COMP read back on the radio's 0–10,
  squelch on at threshold 0, the preamp/ATT interlock shown at once, and the SQL
  line drawn on a measured scale — and gave `radiocert` stages that catch each
  class on any radio publishing the control. v26.10.4 runs the IC-7300MK2,
  IC-705 and IC-9700 scope in SCROLL-F, so the panadapter scrolls and tunes in
  place as on a Flex, with Center and Fixed on an Icom-only Scope row; it also
  bounded the IC-705 preamp ladder above 74.8 MHz and reconnects when the radio
  releases the current media streams. Remaining: transmit confirmation on additional Icom
  models, the per-model SET-menu item numbers the MOD Input check needs, audio
  gain/mute/pan, an automation verb making the modulation
  sources assertable without parsing Radio Health text, and the once-a-second
  FT8 transmit dropout still under investigation.
- **ANAN-G2 — experimental, receive-only** ([RFC #4970](https://github.com/aethersdr/AetherSDR/issues/4970), approved) — openHPSDR Protocol 2 discovery
  with a single receive path, spectrum and audio, live tuning and zoom, arrived
  in v26.9.2. v26.9.3 removed the session rebuild behind a zoom change — `p2app`
  services DDC-Specific packets in a continuous loop and its "something changed"
  hook is empty, so a rate change is just a resend — and taught the wire layer
  multi-DDC encode with per-sender-port demux, because the DDC I&Q packet
  carries no index field. v26.9.4 shipped DDC0 droop-correction defaults
  derived from the Saturn gateware — the CIC and 1024-tap FIR are fully
  specified in the FPGA sources, so an unswept radio gets a corrected FFT on
  first connect, with the in-app calibration still available — cropped the
  panadapter's true edge instead of fading it, and gave the radio back its
  noise-floor auto-adjust. v26.9.5 computed the panadapter with WDSP's display
  analyzer, so no IQ is discarded between frames and FFT AVG drives it, at one
  point per screen pixel; published the S-meter; ran WDSP's noise blanker from
  the NB button; and drove the ADC step attenuator from RF Gain. v26.10.1
  applied the receiver's AF gain, mute and balance and sent receive audio to
  the G2's own speaker and headphone jack. v26.10.2 restored frequency, mode,
  tuning step, output level and dBm range per radio, widened AGC-T to
  −20…120 dB with a working AGC-off gain, published link telemetry and the
  supply voltage, and added WDSP's second noise blanker. v26.10.3 moved the seven
  front-end options to Radio Setup → Radio and added discovery per distinct
  attached IPv4 subnet. v26.10.4 delivered parts 2 and 3 of RFC #6209: the receive band-pass
  filters can follow the tuned frequency and the speaker amplifier mutes on its
  own, and discovery logs its bind and send failures. The codec is
  multi-DDC capable but `AnanBackend` still drives one; remaining is the
  `AnanRxDsp` fan-out, then transmit.
- **RTL-SDR — experimental, receive-only** — `librtlsdr` discovery with one
  panadapter and one host-demodulated slice (AM, FM, SSB, CW) on builds carrying
  the libraries, from v26.9.2. v26.9.3 added a bounded receiver lifecycle
  foundation and device-identity persistence, plus a `SharedCapturePolicy` that
  requires every receiver's complete guarded passband to fit the shared capture
  before a tune, filter, mode or rate change is admitted. That policy is written
  and tested but not yet wired to a live backend. v26.9.4 landed RFC #5468's
  A3 and A4 increments: rate-aware QSO recording and WAV playback, and TCI
  receive audio that preserves the producer's 24/48 kHz rate and stereo.
  v26.9.5 landed A5: CW, RTTY and AetherClock decode the selected slice's typed
  PCM. v26.10.1 landed M1: receive changes publish only after the dongle's
  readback and DSP adoption, capture browsing parks receivers outside the
  usable capture and resumes them, zoom uses a continuous 65,536-point FFT, and
  FM/FM-N gain symmetric filters and squelch. v26.10.2 added WFM stereo, live
  PPM adjustment and multi-receiver delivery — per-receiver squelch, relative
  meters, APRS on the selected receiver and receive DAX/TCI feeds — and every
  official package now bundles the backend. v26.10.3 adds SSB/CW and AM/SAM
  reception with accepted filter/DSP state and selected-receiver CW/Copy Assist
  audio. The normal launch still opens one
  receiver. Remaining: production multi-receiver capacity qualification and
  optional digital FM reception (RFC #6079).
- **Workspace canvas — experimental** — [RFC #4887](https://github.com/aethersdr/AetherSDR/issues/4887) landed complete in v26.8.3,
  all seven phases: pans and applets as freely placed, resizable, layered items
  on a canvas that can span several top-level windows, with named workspaces,
  full-recall switching and radio-profile bindings. It is **off by default**, and
  an install that never enables it never gains a settings key. Remaining before
  the experimental label can come off: live cross-window drag (deferred this
  cycle — a cross-top-level reparent is the #2495/#4617/#4319 crash lineage, so
  moves go through one deliberate menu path for now), and field time on real
  stations against the Classic shell.
- **Hermes-Lite 2 — from experimental to supported** — the backend arrived
  experimental in v26.7.4 and grew most of the way to parity in v26.8.1: four
  independent receivers, the SSB voice chain, CW/RTTY decoding and the QSO
  recorder, AX.25 packet with an on-air-proven mailbox, band switching with
  hardware filters and preamp, host-side memory channels, per-MAC operating-state
  restore with per-band drive/LNA memory, live connection health and a Radio
  Health dialog. v26.8.2 added **manual notch filters** and **manual frequency
  calibration**, DC-blocked the AM/SAM audio, and unfroze the first connect.
  v26.8.3 gave it a working **NB** button (WDSP's impulse blanker on the raw IQ,
  the only place it can run on this radio), a **real BFO** so a CW passband
  straddles the marker instead of sitting where a USB filter would, **AGC mode
  and threshold that survive a restart**, and a **TX ALC that no longer
  normalises away a TCI/DAX client's own level control**. Its meter surface is
  now certified against physical hardware. v26.9.4 was the largest step since
  v26.8.1: the **transmit chain moved to WDSP's TXA modulator** and it is the
  default, after ON8ST keyed it into a dummy load and onto an antenna; the ALC
  only ever reduces and the Mic Level slider is the transmit level; the modes
  the phasing modulator could not transmit in are declared, so AM no longer
  keys suppressed-carrier SSB; the **dBm reference is derived** (+3 dBm full
  scale at 0 dB LNA gain); the **S-meter reads WDSP's average** rather than a
  decaying peak-hold that read the noise floor 11–14 dB high; the wideband
  bandscope (endpoint 4) is decoded with an on-demand converter view; automatic
  RF gain drives on measured headroom (RFC #5535, shipped off until the LNA
  default reconciles with the arming baseline); and pan-bandwidth chains are
  built off the I/O thread, so a zoom no longer stalls EP2 and silences the
  radio. v26.9.5 ran the RX bandpass at **minimum phase outside CW**, cutting
  receive latency by 84 ms (44 ms back after an unmute instead of 128 ms);
  deferred the unmute past the T/R turnaround so the PA's own carrier no longer
  reaches the demodulator; let the operator declare the board variant (bare
  HL2, AK4951 companion, SquareSDR 2); routed the second receiver's S-meter to
  its slice; and made the client re-ask a silent radio to stream before declaring
  the link down. v26.10.1 filled most of the remaining control gaps: receive
  squelch per mode family referred to the LNA, the CW audio peaking filter, a
  working AGC-off level with DIGU/DIGL opening AGC-off, RIT and XIT per
  receiver, FFT AVG computed backend-side, Black Level and NB Blank on its
  absolute-dB rows, client-side spots, the CL1 external 10 MHz reference, and
  automatic RF gain armable across the native −12…+48 dB range on the
  bandscope law. Every control it cannot serve now refuses aloud. v26.10.2 made
  the client its memory — TUNE power per band, FFT AVG, receivers B–D with each
  receiver's setpoints, manual notches, FFT FPS and the dBm range survive a
  restart — brought receive audio out of WDSP at unity (12 dB quieter), sent CW
  at the TUNE carrier level, ramped the TX ALC gain inside each block and
  discovered the radio on every local interface. v26.10.3 enables AM, DSB and
  simplex FM/NFM through TXA, retains graphic EQ across restarts, makes Show TX
  in Waterfall client-owned on HL2, and observes hardware PTT without keying
  the host. v26.10.4 reads the N2ADR IO board's status registers over the
  I2C read path. Keyed rendering and hardware-PTT validation remain outstanding.
  **The experimental → supported call itself is still open**; what remains
  before making it is the rest of panadapter/waterfall parity with the Flex
  path, arming automatic RF gain by default, and field time on the TXA chain
  beyond one station. Two known costs are on the record rather than hidden: CW
  keeps linear phase, at 4096 taps (85 ms onset) until a notch or a narrow
  passband needs 8192, and the 0.6–1.1 s pan-bandwidth rebuild is off the audio
  path but still a wait.
- **AetherSDR style guide — the visual canon** ([RFC #6226](https://github.com/aethersdr/AetherSDR/issues/6226), approved;
  tracking [#6239](https://github.com/aethersdr/AetherSDR/issues/6239)) — the canon is written down in
  `docs/style/aethersdr-style-guide.md` with its own `color.canon.*` tokens,
  and v26.10.2 moved the first surfaces onto it: the About window, AetherRX,
  AetherTX, AetherModem, AetherMap, AetherGPS, Connect to Radio, AGC-T
  Calibration, the DVK panel and the panadapter header, through `CanonWindow`
  and the new in-window `CanonSurface` classes. v26.10.3 adds SpotHub, Radio
  Setup and AetherControl to the migrated workspaces, and v26.10.4 the multiFLEX
  Dashboard, TX Band Settings, Icom RC-28 Button Mapping, NR2's FFTW Wisdom
  window and the CTR2 window's message boxes. Remaining: the other surfaces,
  one PR at a time, then a single flip of the base tokens to canon values.
- **In-app updates** ([RFC #5304](https://github.com/aethersdr/AetherSDR/issues/5304)) — v26.10.4 shipped the
  update check at launch with its UPDATE title-bar badge and Skip This Version, and the
  Updates window (File → Update...) with the download built for this computer
  and the radio's firmware lanes and install. Behind it, every release
  publishes a signed manifest and per-file patch bundles, `UpdateStager`
  stages a verified update into a copy of the install, and the Windows
  `AetherSDRUpdater` swaps it in and rolls back on failure. Remaining: the
  Install and Restart to update wiring on Windows, then the other platforms.
- **AppSettings nested-JSON refactor** — ~460 flat call sites today;
  the new pattern is one nested-JSON value per feature (Principle V).
  The storage layer moved to SQLite and the scoped feature-document store,
  BandStack and memory-bank fold-ins, and the Settings Browser all shipped in
  v26.8.1 (RFC #4603, PRs 1–6). New radio-scoped configuration lands as
  versioned feature documents in `radio_settings`; the remaining work is
  migrating the legacy flat keys feature-by-feature.
- **Flathub submission** — the AppStream metainfo and manpage landed in
  v26.6.4; the actual Flathub PR + manifest is the remaining step.

### Queued (next cycle)

- **Unified client-side voice keyer** ([RFC #4214](https://github.com/aethersdr/AetherSDR/issues/4214), approved) — local
  per-client recordings, quick-access CQ/Call, external and controller control,
  and radio/local routing. Phase 1 shipped in v26.10.3: twelve local slots,
  recording, preview/import/export, F1–F12 playback and Local/Radio routing.
  Repeat/CQ automation, permanent CQ/Call chrome and external-control
  extensions remain; the umbrella is not declared complete.
- **KiwiSDR follow-ups** — WebSDR / OpenWebRX support on top of the shipped
  public-receiver browser (per-receiver passwords, idle-release, and
  waterfall polish landed in v26.7.2; warm audio through TX and the
  resume-after-TX-delay option in v26.8.1).
- **Extended region band plans** — DXCC entities outside IARU R1/R2/R3.
- **macOS VirtualAudioBridge audit** ([#2940](https://github.com/aethersdr/AetherSDR/issues/2940))
  — focused security review of the macOS shared-memory audio bridge.
  (The RigctlPty side is resolved — RigctlPty was removed in #3380.)

### Larger feature requests (community backlog)

Substantial features requested on the
[issue tracker](https://github.com/aethersdr/AetherSDR/issues?q=is%3Aopen+label%3A%22New+Feature%22)
— captured here for visibility, **not yet scheduled**. 👍 the issue to signal demand.

**Extensibility**

- **Plugin subsystem** — loadable decoder/DSP extensions, e.g. FT8/FT4/WSPR
  ([#3474](https://github.com/aethersdr/AetherSDR/issues/3474)).
- **TX-audio VST plugin host**
  ([#662](https://github.com/aethersdr/AetherSDR/issues/662)).

**Multi-radio & remote operation**

- **Single instance, two radios** — multi-radio operation; the `RadioSession`
  aggregate landed as the foundation
  ([#3445](https://github.com/aethersdr/AetherSDR/issues/3445)).
- **AetherLink** — integrated mobile remote server with low-bandwidth transport
  and an Android client
  ([#3128](https://github.com/aethersdr/AetherSDR/issues/3128)).

**Client-side DSP**

- **AM co-channel canceller** for MW/SW DX
  ([#578](https://github.com/aethersdr/AetherSDR/issues/578)).
- **Beat-cancel** — heterodyne/carrier interference canceller
  ([#529](https://github.com/aethersdr/AetherSDR/issues/529)).
- **CQUAM AM-stereo decoder**
  ([#176](https://github.com/aethersdr/AetherSDR/issues/176)).

**Operating modes & spotting**

- **Band-traffic / band-opening monitor**
  ([#3114](https://github.com/aethersdr/AetherSDR/issues/3114)).
- **Advanced spot colouring** — DXCC status, LoTW activity, per-callsign worked
  status ([#2809](https://github.com/aethersdr/AetherSDR/issues/2809)).
- **Contest-optimized high-contrast GUI**
  ([#2893](https://github.com/aethersdr/AetherSDR/issues/2893)).
- **Client-side digital voice keyer (DVK)** with local audio playback
  ([#957](https://github.com/aethersdr/AetherSDR/issues/957)).

**Packet / APRS / mapping** (building on the new map engine + AFSK demod)

- **Digipeater Phase 2**: wide-area WIDEn-N/SSn-N, N trapping, viscous/direct-only
  operation, and tiered beacons ([#3571](https://github.com/aethersdr/AetherSDR/issues/3571)).
  The current MVP covers 1200-baud WIDE1-1 fill-in only. APRS-IS is separate scope.
- **Live NEXRAD / weather-radar tile overlay** on the map
  ([#3574](https://github.com/aethersdr/AetherSDR/issues/3574)).
- **IQ-stream transmission over TCI** for CW/RTTY skimmers
  ([#999](https://github.com/aethersdr/AetherSDR/issues/999)).

**Amplifier & tuner integrations**

- **RF2K+** PA ([#1902](https://github.com/aethersdr/AetherSDR/issues/1902)),
  **Palstar HF-Auto** ([#97](https://github.com/aethersdr/AetherSDR/issues/97)),
  **LDG** USB-serial tuner ([#2092](https://github.com/aethersdr/AetherSDR/issues/2092)),
  and **Icom AH4** tuner protocol ([#542](https://github.com/aethersdr/AetherSDR/issues/542)).

### Open RFCs (awaiting decision)

Proposals written up under the [RFC process](GOVERNANCE.md#rfc-process),
waiting on a maintainer decision — **not scheduled, and not approved for
implementation**. An approved RFC moves up into the cycle above. Full list:
[`label:rfc`](https://github.com/aethersdr/AetherSDR/issues?q=is%3Aopen+label%3Arfc).

**Panadapter and display**

- [#6394](https://github.com/aethersdr/AetherSDR/issues/6394) — Raw-spectrum waterfall row ownership and interval integration

- [#5711](https://github.com/aethersdr/AetherSDR/issues/5711) — Per-panadapter spot marker visibility toggle
- [#5586](https://github.com/aethersdr/AetherSDR/issues/5586) — WSJT-X Rx/Tx frequency overlay on the panadapter/waterfall
- [#5350](https://github.com/aethersdr/AetherSDR/issues/5350) — Optional mini-waterfall for Mini-Pan
- [#5348](https://github.com/aethersdr/AetherSDR/issues/5348) — Radio-native VFO flag layout — carry a second receiver where a second panadapter cannot go
- [#5223](https://github.com/aethersdr/AetherSDR/issues/5223) — Decouple panadapter zoom from the radio's sample rate
- [#4925](https://github.com/aethersdr/AetherSDR/issues/4925) — Retain Band, Segment, or custom panadapter span across tuning and restart

**Audio, DSP and transmit**

- [#6417](https://github.com/aethersdr/AetherSDR/issues/6417) — Client-side instant replay audio and waterfall scrubbing
- [#5704](https://github.com/aethersdr/AetherSDR/issues/5704) — Operator control of AGC position relative to noise reduction
- [#5448](https://github.com/aethersdr/AetherSDR/issues/5448) — Audio preset workflow: quick selection, profile associations, and starting presets
- [#4836](https://github.com/aethersdr/AetherSDR/issues/4836) — TX dynamics: CFC and leveler, with multiband limiter follow-ups
- [#4769](https://github.com/aethersdr/AetherSDR/issues/4769) — TX Linearity Analyzer — numeric IMD/shoulder/ACPR measurement from the radio's own transmission
- [#4334](https://github.com/aethersdr/AetherSDR/issues/4334) — On-device text-to-speech (TTS) for the voice keyer — type a message, send it on-air
- [#5047](https://github.com/aethersdr/AetherSDR/issues/5047) — Add a slice-aware SELCAL32 decoder for aviation monitoring
- [#6346](https://github.com/aethersdr/AetherSDR/issues/6346) — AetherN³R (Aetherial Neural Net Noise Reduction): weak-SSB neural NR trained on HF receiver noise

**Radios, protocol and devices**

- [#6472](https://github.com/aethersdr/AetherSDR/issues/6472) — Client-side transverters for non-Flex radios with separate RX/TX LOs
- [#5688](https://github.com/aethersdr/AetherSDR/issues/5688) — SIP session border controller — phone patch and two-ended SIP/RTP relay over RF
- [#5468](https://github.com/aethersdr/AetherSDR/issues/5468) — Complete RTL-SDR receive DSP with multiple slices, independent zoom and stereo WFM
- [#4840](https://github.com/aethersdr/AetherSDR/issues/4840) — IC-9700 support for dual VFOs / slices for satellite use
- [#3894](https://github.com/aethersdr/AetherSDR/issues/3894) — Standalone receive sessions for KiwiSDR and future receive-only providers
- [#3869](https://github.com/aethersdr/AetherSDR/issues/3869) — HFChat: many-to-many text chat over HF RTTY (FDMA) with OTA + optional KiwiSDR reconciliation
- [#3613](https://github.com/aethersdr/AetherSDR/issues/3613) — Remote RF-quiet receive 'antennas' — WebSDR & KiwiSDR
- [#5342](https://github.com/aethersdr/AetherSDR/issues/5342) — Make radiocert a universal lifecycle and meter-to-UX certification framework
- [#5972](https://github.com/aethersdr/AetherSDR/issues/5972) — Native Xiegu G90 backend — CI-V control and external stereo raw-IQ input
- [#6106](https://github.com/aethersdr/AetherSDR/issues/6106) — Backend-owned hardware profiles for openHPSDR Protocol 2 radios
- [#6079](https://github.com/aethersdr/AetherSDR/issues/6079) — Optional Digital FM reception and program metadata
- [#6712](https://github.com/aethersdr/AetherSDR/issues/6712) — RF-KIT applet: optional TUNE button, ATU indicator, Error banner and one-click RESET
- [#6725](https://github.com/aethersdr/AetherSDR/issues/6725) — ANAN-G2: diversity reception on the synchronized DDC0/DDC1 pair

**Interface and workflow**

- [#6515](https://github.com/aethersdr/AetherSDR/issues/6515) — A View → Meter Type submenu for S-Meter and SmartMTR
- [#5616](https://github.com/aethersdr/AetherSDR/issues/5616) — Use Case Profile/Settings Manager — combine radio profiles and client presets under one switchable label
- [#5270](https://github.com/aethersdr/AetherSDR/issues/5270) — Pluggable applets and the Applet Exchange (ApX)
- [#5234](https://github.com/aethersdr/AetherSDR/issues/5234) — Add dedicated Band Applet and BAND top-bar toggle button
- [#5010](https://github.com/aethersdr/AetherSDR/issues/5010) — Status-bar clock display options
- [#4287](https://github.com/aethersdr/AetherSDR/issues/4287) — Dock AetherDSP Settings inline in the slice panel, with applet-style header and popout
- [#3689](https://github.com/aethersdr/AetherSDR/issues/3689) — Net Reminder Scheduler — post-merge follow-ups (#3684)
- [#1494](https://github.com/aethersdr/AetherSDR/issues/1494) — CW and RTTY tuning indicators: pitch centering and tone alignment
- [#6212](https://github.com/aethersdr/AetherSDR/issues/6212) — Status bar: caption the supply voltage when it is the only PA reading
- [#6677](https://github.com/aethersdr/AetherSDR/issues/6677) — Compact Memory drawer sizing and icon-revealed live search
- [#6690](https://github.com/aethersdr/AetherSDR/issues/6690) — AetherModem instances, fixed RX assignment, and a receive-only APRS iGate
- [#6742](https://github.com/aethersdr/AetherSDR/issues/6742) — Title bar radio tabs: the three most recent radios, every other radio in a drawer (amends RFC #4764)

**Project infrastructure**

- [#6435](https://github.com/aethersdr/AetherSDR/issues/6435) — Review routing, TX-path acknowledgement and red-main attribution
- [#4496](https://github.com/aethersdr/AetherSDR/issues/4496) — In-repo multilingual user documentation generator — bridge-driven, 5 languages
- [#2884](https://github.com/aethersdr/AetherSDR/issues/2884) — feat(smartlink): migrate Auth0 from ROPG to Authorization Code + PKCE on dedicated client
- [#6232](https://github.com/aethersdr/AetherSDR/issues/6232) — Official Windows ARM64 build and release support

### Recently shipped

Highlights from the current cycle (v26.10.x). Earlier releases and the
complete list are in [`CHANGELOG.md`](CHANGELOG.md):

- **The Updates window and an automatic update check** — a check at launch,
  at most once a day, that only lights an UPDATE badge, Skip This Version, File → Update... with the
  download for this computer, and radio firmware installs in the same window.
  RFC #5304 (v26.10.4).
- **SmartSDR 4.3 Noise Floor Range and CW profiles** — the radio-owned NF Range
  slider and the CW profile type, with APD state, FDVU/FDVL CAT modes and
  unsigned amplifier/tuner meter handles (v26.10.4).
- **The CTR2 relay over USB** — Lynovation's CTR2-Max firmware v1.02.00 and
  later relays over USB and starts automatically, with a wide-report link for
  devices that offer it and one-click Linux download-mode access. RFC #6091
  (v26.10.4).
- **RF-KIT RF2K-S, the Icom SCROLL-F panadapter and ANAN-G2 band-pass filters**
  — a REST-API amplifier applet, an Icom panadapter that scrolls instead of
  recentering, and the G2's filter bank and speaker mute (v26.10.4).
- **Touch gestures** — pinch-zoom, press-and-hold right-click and kinetic
  scrolling for touch screens. RFC #6775 (v26.10.4).
- **HTTPS back on upgraded Windows installs** — the installer clears the stale
  Qt TLS plugin that disabled SmartLink, update checks and downloads (v26.10.4).
- **SmartSDR 4.3 controls** — radio-owned Auto AGC-T, DSP parameter reset,
  PTT Mic Override and per-band RCA off/TUNE, gated by firmware, hardware and
  licence as applicable (v26.10.3).
- **DeepCW and DeepFist** — standard RX choices beside ggmorse where ONNX
  Runtime is available, with pinned upstream model downloads on first use
  (v26.10.3).
- **Auto squelch stays with its slice and pan** — twelve corrections for CW,
  FT8/digital and multi-receiver setups: mode gates and overlays, independent
  Auto intent/margins, own-pan noise floors and preserved manual thresholds.
  PR #6464 (v26.10.3).
- **Local voice recordings** — twelve 30-second slots, PC-microphone recording,
  preview/import/export and F1–F12 playback; the radio keyer remains selectable (v26.10.3).
- **Controller improvements** — exclusive CTR2 USB ownership, negotiated
  CW-centered spectrum/text and variable-speed Contour shuttle tuning
  (v26.10.3).
- **Elecraft station panels** — KPA500, KAT500 and KXPA100/KXAT100 over serial
  or raw TCP, with Full/Mini views and Advanced maintenance (v26.10.3).
- **HL2 AM/DSB/FM and RTL SSB/CW/AM/SAM** — TXA adds the HL2 transmit modes;
  RTL adds receive DSP and selected-decoder audio, retaining its normal
  one-receiver limit (v26.10.3).
- **More canon workspaces** — SpotHub, Radio Setup and AetherControl, plus a
  CW Pitch slider and keyer buttons that fit (v26.10.3).
- **One 52 px title bar with radio tabs** — the menu, brand, a tab per radio
  with its link state, the audio cluster and the window controls in one bar on
  every platform. RFC #4764 (v26.10.2).
- **docs.aethersdr.com** — the user guide as docs-as-code with Stable and Next
  versions, a signed PDF manual, real-radio screenshots and a browser-side Log
  Analyzer, linked from the Help menu (v26.10.2).
- **Remote operation over CGNAT** — a Tailscale node inside a FLEX-8000 or
  Aurora radio relays AetherSDR to tailnet clients, installed and configured
  from the Waveforms window, with the MTU capped to fit the tunnel. RFC #6271
  (v26.10.2).
- **The canon windows** — About, AetherRX, AetherTX, AetherModem, AetherMap,
  AetherGPS, Connect to Radio, AGC-T Calibration and the panadapter header on
  the AetherSDR style guide. RFC #6226 (v26.10.2).
- **DVK Import WAV** — any WAV converted to the radio's 24 kHz mono through the
  documented two-step upload, with slot names that keep their spaces (v26.10.2).
- **FFT Smooth and FFT De-comb** — client-side trace shaping across frequency
  on every radio family, off by default (v26.10.2).
- **RTL-SDR WFM stereo and multi-receiver delivery** — stereo broadcast FM,
  live PPM, per-receiver squelch, meters and audio feeds, and RTL in every
  official package. RFC #5468 (v26.10.2).
- **The Hermes-Lite 2 and ANAN-G2 remember where you left them** — receivers,
  setpoints, TUNE power, FFT settings, frequency, mode and step restored per
  radio (v26.10.2).
- **Signed Windows builds and shipped firewall rules** — Authenticode on every
  exe and DLL, inbound allow rules in the installer and a Windows Firewall page
  in Network Diagnostics (v26.10.2).
- **Every control works or says why not** — on a radio with no Flex command
  plane, each control either reaches the radio through a path it already has,
  is dimmed with its reason, or refuses aloud, and controls that work stop
  raising the "nothing was sent" notice (v26.10.1).
- **A CTR2 relay over Wi-Fi, with USB ready on the host side** — the CTR2 Proxy applet forwards a
  CTR2-Max controller's traffic to the connected radio unchanged, as an
  independent client that never touches AetherSDR's transmit paths. RFC #6091
  (v26.10.1).
- **Authenticated 4O3A accessories and a Peripherals page** — direct TGXL, PGXL
  and Antenna Genius connections send the device's access code from the system
  keychain, under a device list with Add, Remove and per-device auto-connect
  (v26.10.1).
- **Hermes-Lite 2 squelch, APF, RIT/XIT and FFT AVG** — receive squelch per mode
  family referred to the LNA, the CW peaking filter, a working AGC-off level,
  RIT and XIT per receiver, backend-side averaging and the CL1 10 MHz reference
  (v26.10.1).
- **ANAN-G2 receiver audio and the radio's own speaker** — AF gain, mute and
  balance apply, and receive audio plays through the G2's codec (v26.10.1).
- **RTL-SDR confirmed receive state** — changes publish only after the dongle
  confirms them, with capture browsing and a 65,536-point zoom FFT. RFC #5468 M1
  (v26.10.1).
- **Slice receive controls behind the seam** — typed backend requests and
  rerouted GUI callers replace 44 raw Flex commands above the seam
  (413 → 369). #5262 M4 (v26.10.1).
- **Qt 6.12 everywhere** — every binary and every source build, with a
  one-command Qt install; the macOS DMGs now require macOS 14.4 (v26.10.1).

## How to influence the roadmap

- **Open an issue** with the feature-request template if you want
  something specific. The AetherClaude orchestrator triages it within
  minutes.
- **Open a PR** if you've already built it — see
  [`CONTRIBUTING.md`](CONTRIBUTING.md). Most cleanup-class work
  AetherClaude can do autonomously; novel features benefit from a
  design discussion in the issue first.
- **Sponsor a feature** — email the project lead at
  `kk7gwy@aethersdr.com`. Sponsored work jumps the queue while
  remaining open-source.

This roadmap is intentionally short. Long roadmaps don't ship.
