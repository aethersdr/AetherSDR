# AetherSDR Roadmap

Live tracking lives in [GitHub Issues](https://github.com/aethersdr/AetherSDR/issues)
and the per-cycle milestone view. This file is a human-readable snapshot
of what the project lead and core contributors are working on — updated
as direction changes.

For *what shipped*, see [`CHANGELOG.md`](CHANGELOG.md).

## Current cycle: post-v26.8.1

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
- **Hermes-Lite 2 — from experimental to supported** — the backend arrived
  experimental in v26.7.4 and grew most of the way to parity in v26.8.1: four
  independent receivers, the SSB voice chain, CW/RTTY decoding and the QSO
  recorder, AX.25 packet with an on-air-proven mailbox, band switching with
  hardware filters and preamp, host-side memory channels, per-MAC operating-state
  restore with per-band drive/LNA memory, live connection health and a Radio
  Health dialog. **The experimental → supported call itself is still open**;
  what remains before making it is wider mode coverage, panadapter/waterfall
  parity with the Flex path, and hardening the raw-IQ DSP chain (HL2 ships raw
  IQ, so the client does all the tune/decimate/demodulate work a Flex does
  on-radio).
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