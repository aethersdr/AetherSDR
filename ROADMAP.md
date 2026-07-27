# AetherSDR Roadmap

Live tracking lives in [GitHub Issues](https://github.com/aethersdr/AetherSDR/issues)
and the per-cycle milestone view. This file is a human-readable snapshot
of what the project lead and core contributors are working on — updated
as direction changes.

For *what shipped*, see [`CHANGELOG.md`](CHANGELOG.md).

## Current cycle: post-v26.7.4.1

### In flight

- **aetherd — vendor-neutral radio backend** — extracting an
  `IRadioBackend` seam (`RadioCapabilities` + typed status/command deltas)
  so radio-family logic lives behind a stable interface instead of being
  woven through `RadioModel`. FlexBackend owns the Flex wire objects
  and threads, and the Panadapter / Slice / Meter / Transmit / Amp / Tuner
  status+command paths decode behind the seam (RFC steps 2.1–2.4). The seam
  now carries **three** backends — `FlexBackend`, `HL2Backend`, and the
  synthetic `SimBackend` — which is what took it from a design to a proven
  interface. Remaining: the versioned protocol (RFC step 3+) that lets a
  headless `aetherd` and thin UI clients split apart; UI code still consumes
  models directly, and that remains correct until it lands.
- **AppSettings nested-JSON refactor** — ~460 flat call sites today;
  the new pattern is one nested-JSON value per feature (Principle V).
  Mechanical migration tooling is the prerequisite work.
- **TX DSP chain visual rebuild** — stage-per-applet chain with the
  visual `CHAIN` widget as the primary entry point.
- **Flathub submission** — the AppStream metainfo and manpage landed in
  v26.6.4; the actual Flathub PR + manifest is the remaining step.

### Queued (next cycle)

- **Hermes-Lite 2 — from experimental to supported** — an experimental backend
  landed in v26.7.4 (receive, transmit, TCI/WSJT-X, operator-controllable span,
  per-radio nicknames), but HL2 is not a supported radio family yet. Getting
  there means wider mode coverage, panadapter/waterfall parity with the Flex
  path, and hardening the raw-IQ DSP chain (HL2 ships raw IQ, so the client does
  all the tune/decimate/demodulate work a Flex does on-radio).
- **KiwiSDR follow-ups** — WebSDR / OpenWebRX support on top of the shipped
  public-receiver browser (per-receiver passwords, idle-release, and
  waterfall polish already landed in v26.7.2).
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