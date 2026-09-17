# AetherSDR Roadmap

Live tracking lives in [GitHub Issues](https://github.com/aethersdr/AetherSDR/issues)
and the per-cycle milestone view. This file is a human-readable snapshot
of what the project lead and core contributors are working on — updated
as direction changes.

For *what shipped*, see [`CHANGELOG.md`](CHANGELOG.md).

## Current cycle: post-v26.9.3

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
  daemon stays observe-only unless `--allow-local-control` is passed. Remaining:
  the multi-client TX arbiter and daemon transmit grants, per-client
  propagation, and a replacement thin UI client — UI code still consumes models
  directly, and that remains correct until that client exists.
- **Icom networked radios — early** — `IcomCIV` speaks CI-V inside the RS-BA1
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
  Remaining: transmit confirmation beyond the 705, the per-model SET-menu item
  numbers the MOD Input check needs, audio gain/mute/pan, VOX and CW break-in, an
  automation verb making the modulation sources assertable without parsing Radio
  Health text, and the once-a-second FT8 transmit dropout still under
  investigation.
- **ANAN-G2 — experimental, receive-only** ([RFC #4970](https://github.com/aethersdr/AetherSDR/issues/4970), approved) — openHPSDR Protocol 2 discovery
  with a single receive path, spectrum and audio, live tuning and zoom, arrived
  in v26.9.2. v26.9.3 removed the session rebuild behind a zoom change — `p2app`
  services DDC-Specific packets in a continuous loop and its "something changed"
  hook is empty, so a rate change is just a resend — and taught the wire layer
  multi-DDC encode with per-sender-port demux, because the DDC I&Q packet
  carries no index field. DDC0 edge droop is compensated from an in-app
  calibration. The codec is multi-DDC capable but `AnanBackend` still drives
  one; remaining is the `AnanRxDsp` fan-out, then transmit.
- **RTL-SDR — experimental, receive-only** — `librtlsdr` discovery with one
  panadapter and one host-demodulated slice (AM, FM, SSB, CW) on builds carrying
  the libraries, from v26.9.2. v26.9.3 added a bounded receiver lifecycle
  foundation and device-identity persistence, plus a `SharedCapturePolicy` that
  requires every receiver's complete guarded passband to fit the shared capture
  before a tune, filter, mode or rate change is admitted. That policy is written
  and tested but not yet wired to a live backend. Remaining: selectable sharp
  passband filtering, and the USB/DSP/audio/viewport integration that turns the
  policy into real multi-receiver capture.
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
  now certified against physical hardware.
  **The experimental → supported call itself is still open**; what remains
  before making it is wider mode coverage, panadapter/waterfall parity with the
  Flex path, and hardening the raw-IQ DSP chain (HL2 ships raw IQ, so the client
  does all the tune/decimate/demodulate work a Flex does on-radio). Two known
  costs are on the record rather than hidden: the **+64 ms of RX latency** the
  8192-tap notch filter buys unconditionally, and the 0.6–1.1 s UI stall when a
  pan-bandwidth change crosses a sample-rate boundary and rebuilds every
  receiver.
- **AppSettings nested-JSON refactor** — ~460 flat call sites today;
  the new pattern is one nested-JSON value per feature (Principle V).
  The storage layer moved to SQLite and the scoped feature-document store,
  BandStack and memory-bank fold-ins, and the Settings Browser all shipped in
  v26.8.1 (RFC #4603, PRs 1–6). New radio-scoped configuration lands as
  versioned feature documents in `radio_settings`; the remaining work is
  migrating the legacy flat keys feature-by-feature.
- **TX DSP chain visual rebuild** — stage-per-applet chain with the
  visual `CHAIN` widget as the primary entry point.
- **Flathub submission** — the AppStream metainfo and manpage landed in
  v26.6.4; the actual Flathub PR + manifest is the remaining step.

### Queued (next cycle)

- **KiwiSDR follow-ups** — WebSDR / OpenWebRX support on top of the shipped
  public-receiver browser (per-receiver passwords, idle-release, and
  waterfall polish landed in v26.7.2; warm audio through TX and the
  resume-after-TX-delay option in v26.8.1).
- **WDSP 2.10 refresh and Neural Noise Reduction** ([RFC #5684](https://github.com/aethersdr/AetherSDR/issues/5684),
  approved) — update the vendored WDSP and add NNR as a seventh ADSP method.
- **Global precipitation with regional radar backups** ([RFC #5630](https://github.com/aethersdr/AetherSDR/issues/5630),
  approved) — worldwide precipitation on the map with regional radar where it
  exists, and optional coverage shading, building on the shipped NOAA layer.
- **Glacier waterfall palette** ([RFC #5670](https://github.com/aethersdr/AetherSDR/issues/5670), approved) — an
  additional waterfall colour scheme. Good first issue.
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

Highlights from the current cycle (v26.9.x). Earlier releases and the
complete list are in [`CHANGELOG.md`](CHANGELOG.md):

- **A Tools-first menu bar** — the top level becomes
  `File · Settings · Profiles · Tools · View · Help`, collecting the operating
  tools that were scattered across File, Settings, View and Help under one
  **Tools** menu placed ahead of **View**, because operators reach for them more
  often than for display settings. Existing actions and handlers are reused, so
  shortcuts and lifecycle behavior are unchanged and only discoverability moves.
  RFC #5570 (v26.9.3).
- **Clock-aligned waterfall time markers** — thin UTC-labelled lines at 15 s to
  15 minute intervals, snapped to clock boundaries and pinned to the signal rows
  they were captured with, so they stay correct through scrolling, pause, resize
  and a waterfall rate change. Off by default, persisted per panadapter slot
  (v26.9.3).
- **An APRS WIDE1-1 fill-in digipeater** — an AetherModem tab that substitutes
  MYCALL with the H bit on the first matching unused hop. It requires the shared
  1200-baud profile, a valid callsign and explicit per-session arming, and
  deliberately never restores arming from settings; wide-area WIDEn-N
  decrementing is still Phase 2 (v26.9.3).
- **Weather radar and night lights on PSK Reporter** — NOAA radar with playback
  and the NASA city-lights basemap, both behind retry handling so a transient
  tile failure no longer leaves the layer blank. A dark map style and brightness
  controls join them, and the controls move into a left sidebar (v26.9.3).
- **Web-888 as its own receiver family** — the KiwiSDR receive path now
  distinguishes Web-888, replaying its waterfall setup once the bare `wf_setup`
  marker is fully processed and invalidating the cached view so an unchanged
  zoom is actually resent. Saved receivers keep their family; legacy entries
  default to KiwiSDR (v26.9.3).
- **The Runtime Monitor Overview** — CPU Total, Max Thread, resident memory and
  GUI Tick Lag as cards and charts over 1 min to 1 h. CPU is whole-process
  cumulative time normalized by core count, so work from threads that start and
  exit between samples is not lost (v26.9.3).
- **aetherd gains control, not just observation** — Stage 3 adds
  capability-qualified local receive methods (mode, filter, audio gain and mute,
  panadapter center and bandwidth) with bounded read-only telemetry, and Stage 4
  adds an engine-local `TxCoordinator` for primary desktop transmit intent. The
  daemon stays observe-only unless `--allow-local-control` is passed, and no
  transmit grants exist yet (v26.9.3).
- **The `IRadioBackend` threading and lifetime contract is pinned by tests** —
  and writing those tests exposed a rule-5 gap that the same change closes.
  Backends now announce capability revisions, family verbs are gated on the
  declared namespace, and the capability-bool population is frozen in CI so the
  seam cannot quietly widen (v26.9.3).
- **ANAN-G2 stops rebuilding the session to change zoom** — `p2app` shows the
  radio services DDC-Specific packets in a continuous loop and its "something
  changed" hook is empty, so a rate change is just a resend. One scroll-wheel
  notch cost a mute, stop, 2000 ms settle and a 6000 ms connect window before
  this. The wire layer also gained multi-DDC encode and per-port demux (v26.9.3).
- **Concurrent TCI DAX IQ skimming** — four independent DAX IQ subscriptions let
  compatible Flex setups feed several CW skimmers through one TCI server.
  Receivers on one panadapter share its IQ stream, so four independent band
  spectra still need four panadapters (v26.9.2).
- **Experimental ANAN-G2 and RTL-SDR reception** — openHPSDR Protocol 2
  discovery with a single receive path for the G2, and a single slice and
  panadapter with host demodulation for RTL-SDR dongles. Both are receive-only
  and neither is a supported family (v26.9.2).
- **The SPE floating front panel** — a live amplifier LCD mirror with guarded
  front-panel keys, alongside TelePost LP-100A wattmeter readings over local
  serial or a serial-to-network proxy (v26.9.2).
- **Icom identifies itself over the wire** — model identification now comes from
  CI-V rather than an editable network nickname, with optional wake on connect
  behind model-specific profiles for the IC-705, IC-7300MK2 and IC-9700. Wake
  defaults off (v26.9.2).
- **A broad IC-7300MK2 control cleanup** — TX bandwidth encoding, the 6–48 WPM
  CW range, 5 Hz CW pitch steps, squelch that stays usable in CW and data modes,
  and refusal rather than silent substitution when a control is unsupported.
  Model-specific mappings stay separate so no MK2 behavior is imposed on other
  Icom radios (v26.9.2).
- **The KiwiSDR directory moves to a CDN mirror** — the public-receiver browser
  reads from the AetherSDR mirror, and stale directory data is advisory so
  operators keep browsing receivers that are still reachable (v26.9.2).
- **Green Heron Everyware antenna control** — a native applet for compatible
  antenna switches and rotators (v26.9.1).
- **A globe projection for PSK Reporter** — operators can switch from the flat
  map to a global view of received reports (v26.9.1).
- **System Info Threads and Logs tabs** — runtime diagnosis moves inside the
  app instead of requiring external tooling (v26.9.1).
- **Radio-authoritative Icom memories** — read-only IC-9700 radio memory,
  model-gated controls and telemetry, and safer reconnect and TX lifecycle
  behavior, with DTCS and multi-radio NAT support (v26.9.1).

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
