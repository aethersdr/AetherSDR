# AetherSDR Roadmap

Live tracking lives in [GitHub Issues](https://github.com/aethersdr/AetherSDR/issues)
and the per-cycle milestone view. This file is a human-readable snapshot
of what the project lead and core contributors are working on — updated
as direction changes.

For *what shipped*, see [`CHANGELOG.md`](CHANGELOG.md).

## Current cycle: post-v26.6.3

### In flight

- **Stream Deck plugin** — ship one Elgato-SDK plugin distributed via
  GitHub Releases (avoid Marketplace DRM); works on Windows/macOS plus
  Linux via OpenDeck.
- **AppSettings nested-JSON refactor** — ~460 flat call sites today;
  the new pattern is one nested-JSON value per feature (Principle V).
  Mechanical migration tooling is the prerequisite work.
- **TX DSP chain visual rebuild** — stage-per-applet chain with the
  visual `CHAIN` widget as the primary entry point.
- **H1 Phase 2** ([#2951](https://github.com/aethersdr/AetherSDR/issues/2951))
  — WAN cert-mismatch dialog + Pinned Certificates settings UI.
  Phase 1 ships warn-only TOFU capture; Phase 2 makes it enforce.

### Queued (next cycle)

- **L1–L4 audit follow-ups** — four low-severity items from the
  2026-05-09 security pass, tracked as
  [#2954](https://github.com/aethersdr/AetherSDR/issues/2954)–[#2957](https://github.com/aethersdr/AetherSDR/issues/2957).
- **Extended region band plans** — DXCC entities outside IARU R1/R2/R3.
- **macOS shmem + RigctlPty audit** ([#2940](https://github.com/aethersdr/AetherSDR/issues/2940))
  — focused security review of VirtualAudioBridge (macOS) and
  RigctlPty (Linux + macOS), follow-up to the audit that found H2.

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

- **WFM software demodulator** — DAX IQ → NCO Doppler / resample / atan2 →
  virtual audio cable, for satellite data work.
- **AetherModem Phase 1 + APRS** — VHF 1200-baud AX.25 RX/TX with a
  Direwolf-derived AFSK demodulator, plus an APRS client (station map, GPS
  beacon, two-way messaging).
- **PSK Reporter reception map** on a new reusable Qt mapping engine
  (QGeoView); the APRS tab is the next planned consumer.
- **DAX-IQ fully operational** — end-to-end IQ delivery, dBFS meter, rate
  switching with persistence.
- **MainWindow decomposition (#3351)** — the ~19.5k-line monolith split into
  sibling TUs, with a new `RadioSession` aggregate.
- Constitution v1.1.0 — 14 numbered principles, multi-agent
  contribution model, signed-commit enforcement.
- Six security fixes shipped against the 2026-05 audit
  (H1, H2, M1, M2, M3+L5, M4 — all advisories will publish on the
  next release tag).
- Repo structure cleanup — root and `docs/` directories brought to
  portfolio-quality OSS shape across 9 sequential PRs
  ([#2933](https://github.com/aethersdr/AetherSDR/issues/2933)).
- TCI TX audio regression fix — restored full power to WSJT-X over TCI
  by reverting the device-identity change that triggered K2 scaling.
- Aetherial Tube Pre-Amp — RNNoise toggle on the TX mic pre-amp area.

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