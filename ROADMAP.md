# AetherSDR Roadmap

Live tracking lives in [GitHub Issues](https://github.com/aethersdr/AetherSDR/issues)
and the per-cycle milestone view. This file is a human-readable snapshot
of what the project lead and core contributors are working on — updated
as direction changes.

For *what shipped*, see [`CHANGELOG.md`](CHANGELOG.md).

## Current cycle: post-v26.10.1

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
  count above the seam fell from 413 to 369. Remaining:
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
  the backend implements. Remaining: transmit confirmation on additional Icom
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
  the G2's own speaker and headphone jack. The codec is
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
  FM/FM-N gain symmetric filters and squelch. Production still admits one
  receiver. Remaining: WFM stereo, optional digital decoding, live PPM
  interaction and multi-receiver delivery.
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
  bandscope law. Every control it cannot serve now refuses aloud.
  **The experimental → supported call itself is still open**; what remains
  before making it is the rest of panadapter/waterfall parity with the Flex
  path, arming automatic RF gain by default, and field time on the TXA chain
  beyond one station. Two known costs are on the record rather than hidden: CW
  keeps linear phase, at 4096 taps (85 ms onset) until a notch or a narrow
  passband needs 8192, and the 0.6–1.1 s pan-bandwidth rebuild is off the audio
  path but still a wait.
- **CTR2 controller relay** ([RFC #6091](https://github.com/aethersdr/AetherSDR/issues/6091), approved) — the
  CTR2 Proxy applet shipped in v26.10.1: an operator-enabled, byte-for-byte
  relay that connects a CTR2-Max controller to the radio AetherSDR is connected
  to, as an independent client outside AetherSDR's transmit paths. Wi-Fi mode
  works with today's CTR2 firmware. USB mode's host side is complete in link
  format v0, with AetherSDR starting the handshake, and the firmware author's
  development firmware has received the radio's status stream over it. It
  waits on a CTR2 USB firmware release.
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

- **RF2K+ / RF2K-S** PA ([#1902](https://github.com/aethersdr/AetherSDR/issues/1902)),
  **Palstar HF-Auto** ([#97](https://github.com/aethersdr/AetherSDR/issues/97)),
  **LDG** USB-serial tuner ([#2092](https://github.com/aethersdr/AetherSDR/issues/2092)),
  and **Icom AH4** tuner protocol ([#542](https://github.com/aethersdr/AetherSDR/issues/542)).

### Open RFCs (awaiting decision)

Proposals written up under the [RFC process](GOVERNANCE.md#rfc-process),
waiting on a maintainer decision — **not scheduled, and not approved for
implementation**. An approved RFC moves up into the cycle above. Full list:
[`label:rfc`](https://github.com/aethersdr/AetherSDR/issues?q=is%3Aopen+label%3Arfc).

**Panadapter and display**

- [#5711](https://github.com/aethersdr/AetherSDR/issues/5711) — Per-panadapter spot marker visibility toggle
- [#5586](https://github.com/aethersdr/AetherSDR/issues/5586) — WSJT-X Rx/Tx frequency overlay on the panadapter/waterfall
- [#5350](https://github.com/aethersdr/AetherSDR/issues/5350) — Optional mini-waterfall for Mini-Pan
- [#5348](https://github.com/aethersdr/AetherSDR/issues/5348) — Radio-native VFO flag layout — carry a second receiver where a second panadapter cannot go
- [#5223](https://github.com/aethersdr/AetherSDR/issues/5223) — Decouple panadapter zoom from the radio's sample rate
- [#4925](https://github.com/aethersdr/AetherSDR/issues/4925) — Retain Band, Segment, or custom panadapter span across tuning and restart
- [#4764](https://github.com/aethersdr/AetherSDR/issues/4764) — Frameless window retrofit: one 52 px unified title bar with radio tabs, on all three platforms

**Audio, DSP and transmit**

- [#5704](https://github.com/aethersdr/AetherSDR/issues/5704) — Operator control of AGC position relative to noise reduction
- [#5682](https://github.com/aethersdr/AetherSDR/issues/5682) — Isolate TCI RX/TX audio from UI scheduling across Flex, HL2 and Icom
- [#5448](https://github.com/aethersdr/AetherSDR/issues/5448) — Audio preset workflow: quick selection, profile associations, and starting presets
- [#4861](https://github.com/aethersdr/AetherSDR/issues/4861) — Raw (pre-noise-reduction) audio for Copy Assist ASR
- [#4836](https://github.com/aethersdr/AetherSDR/issues/4836) — TX dynamics: CFC and leveler, with multiband limiter follow-ups
- [#4769](https://github.com/aethersdr/AetherSDR/issues/4769) — TX Linearity Analyzer — numeric IMD/shoulder/ACPR measurement from the radio's own transmission
- [#4334](https://github.com/aethersdr/AetherSDR/issues/4334) — On-device text-to-speech (TTS) for the voice keyer — type a message, send it on-air
- [#4214](https://github.com/aethersdr/AetherSDR/issues/4214) — Unified client-side voice keyer: local per-client recordings, quick-access CQ/Call, external/controller control, radio/local routing
- [#5047](https://github.com/aethersdr/AetherSDR/issues/5047) — Add a slice-aware SELCAL32 decoder for aviation monitoring

**Radios, protocol and devices**

- [#5688](https://github.com/aethersdr/AetherSDR/issues/5688) — SIP session border controller — phone patch and two-ended SIP/RTP relay over RF
- [#5468](https://github.com/aethersdr/AetherSDR/issues/5468) — Complete RTL-SDR receive DSP with multiple slices, independent zoom and stereo WFM
- [#4840](https://github.com/aethersdr/AetherSDR/issues/4840) — IC-9700 support for dual VFOs / slices for satellite use
- [#4667](https://github.com/aethersdr/AetherSDR/issues/4667) — Band plans conflate preferred mode with usage — six segments mislabelled digi-only, and the schema that caused it
- [#3894](https://github.com/aethersdr/AetherSDR/issues/3894) — Standalone receive sessions for KiwiSDR and future receive-only providers
- [#3869](https://github.com/aethersdr/AetherSDR/issues/3869) — HFChat: many-to-many text chat over HF RTTY (FDMA) with OTA + optional KiwiSDR reconciliation
- [#3613](https://github.com/aethersdr/AetherSDR/issues/3613) — Remote RF-quiet receive 'antennas' — WebSDR & KiwiSDR
- [#5342](https://github.com/aethersdr/AetherSDR/issues/5342) — Make radiocert a universal lifecycle and meter-to-UX certification framework
- [#5972](https://github.com/aethersdr/AetherSDR/issues/5972) — Native Xiegu G90 backend — CI-V control and external stereo raw-IQ input
- [#6106](https://github.com/aethersdr/AetherSDR/issues/6106) — Backend-owned hardware profiles for openHPSDR Protocol 2 radios
- [#6079](https://github.com/aethersdr/AetherSDR/issues/6079) — Optional Digital FM reception and program metadata

**Interface and workflow**

- [#5616](https://github.com/aethersdr/AetherSDR/issues/5616) — Use Case Profile/Settings Manager — combine radio profiles and client presets under one switchable label
- [#5304](https://github.com/aethersdr/AetherSDR/issues/5304) — AetherSDR In-App Update Design
- [#5270](https://github.com/aethersdr/AetherSDR/issues/5270) — Pluggable applets and the Applet Exchange (ApX)
- [#5234](https://github.com/aethersdr/AetherSDR/issues/5234) — Add dedicated Band Applet and BAND top-bar toggle button
- [#5010](https://github.com/aethersdr/AetherSDR/issues/5010) — Status-bar clock display options
- [#4287](https://github.com/aethersdr/AetherSDR/issues/4287) — Dock AetherDSP Settings inline in the slice panel, with applet-style header and popout
- [#3689](https://github.com/aethersdr/AetherSDR/issues/3689) — Net Reminder Scheduler — post-merge follow-ups (#3684)
- [#3184](https://github.com/aethersdr/AetherSDR/issues/3184) — meta(theme): theming system status + polish roadmap — where to help land this
- [#1494](https://github.com/aethersdr/AetherSDR/issues/1494) — CW and RTTY tuning indicators: pitch centering and tone alignment

**Project infrastructure**

- [#4496](https://github.com/aethersdr/AetherSDR/issues/4496) — In-repo multilingual user documentation generator — bridge-driven, 5 languages
- [#4031](https://github.com/aethersdr/AetherSDR/issues/4031) — chore(warnings): eliminate all compiler warnings — GCC -Wall/-Wextra/-Wpedantic + MSVC /W3, 4 phases
- [#2884](https://github.com/aethersdr/AetherSDR/issues/2884) — feat(smartlink): migrate Auth0 from ROPG to Authorization Code + PKCE on dedicated client

### Recently shipped

Highlights from the current cycle (v26.10.x). Earlier releases and the
complete list are in [`CHANGELOG.md`](CHANGELOG.md):

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
