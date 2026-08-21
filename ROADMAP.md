# AetherSDR Roadmap

Live tracking lives in [GitHub Issues](https://github.com/aethersdr/AetherSDR/issues)
and the per-cycle milestone view. This file is a human-readable snapshot
of what the project lead and core contributors are working on — updated
as direction changes.

For *what shipped*, see [`CHANGELOG.md`](CHANGELOG.md).

## Current cycle: post-v26.8.3

### In flight

- **aetherd — vendor-neutral radio backend** — extracting an
  `IRadioBackend` seam (`RadioCapabilities` + typed status/command deltas)
  so radio-family logic lives behind a stable interface instead of being
  woven through `RadioModel`. FlexBackend owns the Flex wire objects
  and threads, and the Panadapter / Slice / Meter / Transmit / Amp / Tuner
  status+command paths decode behind the seam (RFC steps 2.1–2.4). The seam
  now carries **four** backends — `FlexBackend`, `HL2Backend`, `IcomCIV`, and
  the synthetic `SimBackend` — which is what took it from a design to a proven
  interface. Bringing a third vendor up on it in v26.8.2 was also the seam's
  best audit to date: it surfaced a meter path that ignored its own unit,
  receive-DSP controls with no verb behind them, and a capability conflating
  "the host modulates" with "TX audio leaves through the seam". Remaining: the
  versioned protocol (RFC step 3+) that lets a headless `aetherd` and thin UI
  clients split apart; UI code still consumes models directly, and that remains
  correct until it lands.
- **Icom networked radios — early** — `IcomCIV` speaks CI-V inside the RS-BA1
  UDP transport, brought up in v26.8.2 against a live **IC-705** (RX, scope,
  transmit, and FT8 both decoding and spotting on PSK Reporter) and an
  **IC-7300** (RX, scope and stability; transmit unverified). Only the IC-705
  and IC-7300MK2 are `verified` against their own CI-V guides; an unknown model
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
- **Workspace canvas — experimental** — RFC #4887 landed complete in v26.8.3,
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

- **APRS digipeater** tab (MVP: WIDE1-1 fill-in)
  ([#3571](https://github.com/aethersdr/AetherSDR/issues/3571)).
- **Live NEXRAD / weather-radar tile overlay** on the map
  ([#3574](https://github.com/aethersdr/AetherSDR/issues/3574)).
- **IQ-stream transmission over TCI** for CW/RTTY skimmers
  ([#999](https://github.com/aethersdr/AetherSDR/issues/999)).

**Amplifier & tuner integrations**

- **RF2K+ / RF2K-S** PA ([#1902](https://github.com/aethersdr/AetherSDR/issues/1902)),
  **Palstar HF-Auto** ([#97](https://github.com/aethersdr/AetherSDR/issues/97)),
  **LDG** USB-serial tuner ([#2092](https://github.com/aethersdr/AetherSDR/issues/2092)),
  and **Icom AH4** tuner protocol ([#542](https://github.com/aethersdr/AetherSDR/issues/542)).

### Recently shipped

Highlights from the last 30 days — full list in
[`CHANGELOG.md`](CHANGELOG.md):

- **The workspace canvas** — pans and applets become freely placed, resizable,
  layered items on a canvas that can span several top-level windows, with named
  workspaces, full-recall switching, radio-profile bindings and an Edit Layout
  posture so operating the station never brushes the arranging machinery.
  Experimental and off by default (v26.8.3).
- **A CI-V command scheduler for Icom** — one ordered command plane above the
  transport, replacing independent sends from meter, control, startup,
  reconciliation and PTT producers, with priority, coalescing and stale-reply
  rejection. Written because a delayed PTT-OFF reply arriving after a newer
  PTT-ON was cutting transmit audio (v26.8.3).
- **IC-7300MK2 controls, meters and certification** — 18 operator-visible
  defects closed against live hardware, from an ATU button that could not be
  clicked back into bypass to a missing RF Power and SWR meter (v26.8.3).
- **WSPR from an Icom** — the readiness check gated the seam-audio path on
  `hostModulates`, which Icom deliberately reports false, so PSK Reporter never
  advanced to PTT. Confirmed on the air with 20 reception reports from the
  continental US and Alaska (v26.8.3).
- **Icom RS-BA1 lease renewal** — the frozen panadapter was a media-lease
  failure, not a renderer failure: the inner renewal sequence was byte-identical
  to the correct encoding only through sequence 255 (v26.8.3).
- **Hermes-Lite 2 — a host-side noise blanker** — WDSP's impulse blanker on the
  raw IQ ahead of the demodulator, which is the only place it can run on a radio
  that ships raw IQ and runs no firmware DSP (v26.8.3).
- **Hermes-Lite 2 — a real BFO for CW** — the panadapter passband now straddles
  the marker instead of sitting where a USB filter would; the gateware generates
  the shaped CW carrier at the TX NCO, so the receiver had been listening 600 Hz
  from where the radio transmits (v26.8.3).
- **VK3AMP amplifiers** — 600 W / 1000 W / 2000 W units over TCP control and
  UDP telemetry, with calibrated power/reflected/SWR gauges, a bypass and
  voltage-rail interlock, and a variant selector that rescales the forward-power
  gauge (v26.8.3).
- **The TX voice chain moves to 48 kHz float** — one high-quality rate
  conversion to transport rate with TPDF dither before the final quantization,
  replacing several conversions and truncations that compounded aliasing and
  quantization noise (v26.8.3).
- **8 DAX RX audio channels on a FLEX-6700** — slices assigned to DAX 5–8 were
  silently carrying silence-fill; the selectors are now driven from the radio's
  actual slice capacity so smaller models present no dead entries (v26.8.3).
- **CAT band changes bring the panadapter with them** — a band change from
  WSJT-X, FLDigi, rigctld or SmartCAT no longer leaves the view behind on the
  old band until a manual GUI action (v26.8.3).
- **A multi-hour Windows transmit crash** — a TCI session left the local
  microphone device undrained, and the Windows capture backlog eventually
  crossed the signed 2 GiB boundary inside the channel normalizer (v26.8.3).
- **Networked Icom radios** — `IcomCIV`, a fourth backend on the aetherd seam,
  speaking CI-V inside the RS-BA1 UDP transport. Brought up on a live IC-705
  (RX, 30 sweeps/s scope, RX audio, TCI RX, transmit, FT8 decoding and spotting)
  and an IC-9700. Receive handedness certified against WWV (v26.8.2).
- **Hermes-Lite 2 — manual notch filters** — right-click a signal on the
  panadapter to add, move or resize a notch, driving a WDSP notch database that
  was vendored but never declared. 33.4 dB measured off-air on WWV; costs a
  constant +64 ms of RX latency, stated rather than implied (v26.8.2).
- **Hermes-Lite 2 — manual frequency calibration** — a Calibration page, a
  `freqcal` bridge verb, and one per-radio ppb correction that covers every band
  and both oscillators, stored per MAC so two HL2s never share a number
  (v26.8.2).
- **Hermes-Lite 2 — the first connect no longer freezes the app** — 21–82 s of
  frozen UI became a live one, and the FFTW wisdom that cost is spent on now
  actually persists, so it is paid once per machine instead of once per launch
  (v26.8.2).
- **SPE Expert amplifiers** — 1.3K-FA / 1.5K-FA / 2K-FA over serial or ser2net
  TCP (raw or telnet), with gauges, telemetry, the front-panel keystrokes, and
  an RFC 2217 power-ON pulse that works over the network (v26.8.2).
- **Three new spot and schedule overlays** — N1MM+/DXLog contest bandmap spots
  with dupe/mult/CQ/bust status over the SmartSDR-compatible UDP feed, the EiBi
  shortwave broadcast schedule with full code resolution, and the KiwiSDR DX
  Community database as an opt-in click-to-tune layer (v26.8.2).
- **US 60m follows the FCC Report & Order** effective 13 Feb 2026 — the
  5351.5–5366.5 kHz segment added at 9.15 W ERP with its own colour, the retired
  5358.5 kHz channel removed, and the band edges corrected so the top of the
  5405 kHz channel resolves as 60m at all (v26.8.2).
- **The local iambic keyer keys to spec** — absolute-grid element scheduling and
  sample-accurate sidetone edges take a 30 WPM setting from 26.26 to 29.998 WPM
  on real hardware, and Mode B stops dropping the trailing element on a clean
  simultaneous release (v26.8.2).
- **MNR reaches the attenuation it advertised** — 4.54 dB → 24.4 dB on
  stationary noise, after fixing the minimum-statistics estimator, the
  decision-directed recurrence and a strength blend that fed back into the
  adaptive state (v26.8.2).
- **K4-style mini-pan** — a detachable ±5/±10 kHz scope on the active VFO that
  floats over other applications and keeps working in Minimal Mode (v26.8.2).
- **Copy Assist survives an unusable GPU** — a Vulkan stack that enumerates
  devices but cannot create one took the app down on the *second* attempt; a
  per-session failure latch and a one-shot CPU retry replace the crash, and the
  UI now says a fallback happened instead of naming a GPU running nothing
  (v26.8.2).
- **Hermes-Lite 2 — four independent receivers** — up to four DDCs behind the
  single ADC, each with its own NCO, slice, WDSP channel, audio, S-meter and
  panadapter, added and closed at runtime. Sample rate, LNA gain, band and
  antenna are shared because the hardware shares them; four receivers are
  available through 192 kHz and three at 384 kHz on one 100BASE-T link
  (v26.8.1).
- **Hermes-Lite 2 — the SSB voice chain, decoders and packet** — the EQ applet,
  PROC, TX cut filters, eSSB and the ALC/compression meters are wired to the
  host modulator; CW and RTTY decoding and the QSO recorder work; and AX.25
  packet (APRS, KISS TNC, terminal, mailbox) transmits, proven on the air with
  two complete BBS sessions on 21.100 MHz (v26.8.1).
- **Hermes-Lite 2 — band switching, memory and operating-state restore** —
  band buttons, hardware LPF/BPF filters and the hardware preamp reach the
  radio; host-side memory channels work on any radio with no slots of its own;
  and frequency, mode, passband and span are restored per MAC, with TX drive
  and LNA gain remembered per band (v26.8.1).
- **Client settings on SQLite** — transactional saves, startup integrity checks,
  verified backups with quarantine and restore, credentials moved to the OS
  keychain, a `--config` command line for repairing a store that blocks startup,
  per-radio versioned feature documents, and a Settings Browser for reading and
  editing the whole store (RFC #4603, v26.8.1).
- **Capability-gated UI** — every Flex-only surface hides itself on a backend
  that has no such thing, declared by concept rather than by radio family:
  profiles, DAX, the ATU chain, SmartLink, GPS presence, PA supply voltage, and
  the DVK button's SmartSDR+ entitlement (v26.8.1).
- **Qt 6.8.3 LTS everywhere** — the source floor, the CI image, both AppImage
  architectures, the Windows installer and both macOS legs are now the same
  pinned Qt, so one version covers every check and every artifact. The Apple
  Silicon DMG stopped taking whatever Homebrew was publishing, the ARM AppImage
  stopped silently falling back to CPU spectrum drawing, and the Linux AppImage
  runs natively on Wayland (v26.8.1).
- **The Intel Mac DMG reaches older hardware** — it declares and honours a
  macOS 12.0 floor, down from 13.0. The speech-to-text runtime is published at a
  macOS 15.5 floor and one library's floor becomes the whole bundle's, so
  speech-to-text is dropped from the Intel artifact to get there; Apple Silicon
  keeps it (v26.8.1).
- **TCI PTT keys the slice the client asked for** — the fault two operators
  reported across v26.7.3 and v26.7.4 was four separate defects in one path;
  receiver numbers are also now stable across a slice recreate, and the routing
  decision is logged (v26.8.1).
- **TCI rig control hotfix** — a `vfo:` SET confirmed the *pre-tune* frequency,
  so WSJT-X concluded the radio had never moved and failed every band change,
  and relative tuning from a control surface oscillated instead of walking.
  Transmissions could go out of band. Same-day hotfix on top of v26.7.4
  (v26.7.4.1).
- **Hermes-Lite 2 — experimental** — receive, transmit, and TCI signaling for
  WSJT-X on the aetherd `IRadioBackend` seam, with an operator-controllable
  panadapter span (6 Mb low-bandwidth mode) and per-radio nicknames keyed by
  MAC. Early and experimental — **not** a supported radio family; FlexRadio
  remains the supported target (v26.7.4).
- **Built-in demo mode** — a synthetic `SimBackend` that generates its own RX
  audio and matching panadapter render, plus a fault-injection harness, so the
  app can be demonstrated, developed against, and regression-tested with no
  hardware attached. It cannot key (v26.7.4).
- **Copy Assist — on-device speech-to-text** — whisper.cpp transcription with a
  transcription-language selector, running locally (v26.7.4).
- **AetherClock** — a NIST time-signal decode engine plus an applet and
  alignment display (v26.7.4).
- **GPS & station-location dashboard** — position and timing surfaced in one
  place (v26.7.4).
- **3D FFT polish pass** — surface-mapped slice shadows, cached elevation
  shadows on slice flags, preserved history across smooth-scroll boundaries,
  and motion smoothing for both Flex and KiwiSDR sources (v26.7.4).
- **ACOM S-series amplifiers** — serial / ser2net support (v26.7.4).
- **Cross-needle PWR / SWR applet** — an analog cross-needle forward-power,
  reflected-power, and SWR meter face, joined by configurable analog S-meter
  themes (v26.7.3).
- **RTX 50-series / Blackwell BNR** — the in-process NVIDIA AFX denoiser now
  covers consumer Blackwell (RTX 50xx) on Windows and Linux; the app
  auto-detects the GPU and downloads the matching per-arch model pack
  (v26.7.2).
- **MCP server for agent control** — a Model Context Protocol server exposes
  the automation bridge as typed tools, gated behind a Radio Setup toggle with
  token auth (v26.7.2).
- **Searchable Radio Setup & Network Diagnostics** — both reworked into
  searchable settings / troubleshooting browsers (v26.7.2).
- **WAVE showcase visualizations** — GPU-rendered 3D Ridge, Tunnel, and Horizon
  scope modes, plus an incremental-reduction QRhi scope path (v26.7.2).
- **Adaptive RX filter (ESSB auto-fit)** — the SSB receive passband auto-fits
  to the signal, opt-in with edge-heterodyne handling (v26.7.2).
- **QRZ callsign lookup** — a CW-decoder contact card and lookup dialog backed
  by a 7-day cache (v26.7.2).
- **CHIRP-next CSV import** — bring CHIRP memory exports straight into memory
  channels (v26.7.2).
- **Microwave weak-signal bands** — 13cm / 9cm / 5cm / 3cm, plus
  radio-declared band capability from the discovery/status stream (v26.7.2).
- **3D stacked-trace spectrum** — a perspective stacked-trace panadapter render
  mode (rolling FFT history, floor-anchored ridges, 3D Floor depth) with the
  right-edge dBm scale carried into 3D (v26.7.1).
- **NVIDIA BNR — in-process AI noise removal** — the Maxine AFX denoiser running
  in-process on a local NVIDIA GPU, download-on-demand, no container; the
  NIM/gRPC microservice backend was removed (v26.7.1).
- **60 fps GPU panadapters** — a per-pixel GPU FFT trace (no per-frame CPU vertex
  bake) plus present coalescing lift the FFT ceiling from 30 to 60 fps at flat
  CPU cost (v26.7.1).
- **TX meter mouse-over readouts** — exact numeric badges on the SWR / power /
  ALC / mic-level / compression meters (v26.7.1).
- **FlexLib-sourced model capabilities** — extended-DSP, diversity, and slice/pan
  counts now come from the FlexLib `ModelInfo` platform table, fixing the AU-510
  and ML/CL/S-variant gaps (v26.7.1).

For older highlights (KiwiSDR receive sync and the public-receiver browser,
SmartMTR TX meters, the PROF profile-switcher, the agent automation bridge,
the accessibility pass, CAT/rigctld parity, and packaging work) see
[`CHANGELOG.md`](CHANGELOG.md).

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
