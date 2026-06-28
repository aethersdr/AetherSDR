# AetherSDR Roadmap

Live tracking lives in [GitHub Issues](https://github.com/aethersdr/AetherSDR/issues)
and the per-cycle milestone view. This file is a human-readable snapshot
of what the project lead and core contributors are working on — updated
as direction changes.

For *what shipped*, see [`CHANGELOG.md`](CHANGELOG.md).

## Current cycle: post-v26.6.5

### In flight

- **Stream Deck plugin** — ship one Elgato-SDK plugin distributed via
  GitHub Releases (avoid Marketplace DRM); works on Windows/macOS plus
  Linux via OpenDeck.
- **AppSettings nested-JSON refactor** — ~460 flat call sites today;
  the new pattern is one nested-JSON value per feature (Principle V).
  Mechanical migration tooling is the prerequisite work.
- **TX DSP chain visual rebuild** — stage-per-applet chain with the
  visual `CHAIN` widget as the primary entry point.
- **Flathub submission** — the AppStream metainfo and manpage landed in
  v26.6.4; the actual Flathub PR + manifest is the remaining step.

### Queued (next cycle)

- **KiwiSDR follow-ups** — audio-quality / AGC polish on top of the
  v26.6.4 public-receiver browser; potential WebSDR / OpenWebRX support.
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

- **KiwiSDR receive sync** — GCC-PHAT Auto-Assist that time-aligns the Flex
  and a public KiwiSDR receiver in both audio and the spectrum/waterfall
  (v26.6.5).
- **SmartMTR TX meters** — selectable SWR, forward-power, and compression
  gauges with analog ballistics for the VFO flag, including VOX-keyed
  transmit (v26.6.5).
- **PROF profile-switcher applet** — live Global / TX / Mic profile
  selection from a sidebar applet (v26.6.5).
- **Agent automation bridge expansion** — radio connect/disconnect,
  display-stream leak detection (`streams`), custom context-menu inspection,
  and a panadapter/waterfall control surface (v26.6.5).
- **KiwiSDR public-receiver browser** — an API-policy-aware directory to
  find and connect to public KiwiSDR receivers worldwide, with diversity
  receive and receive-only TX inhibit (v26.6.4).
- **SmartMTR meter view** — a selectable, analog-ballistics meter for the
  VFO flag (extremes markers, value labels, TX mic level), opt-in with the
  S-meter pixel-identical by default (v26.6.4).
- **Agent automation / test bridge** — an in-app, agent-drivable bridge to
  drive and verify the GUI without pixels (`AETHER_AUTOMATION`).
- **GPU-composite slice flags + multi-GPU selector** — flags composited on
  the GPU instead of raster siblings, plus a render-GPU picker.
- **Accessibility pass** — `QAccessibleInterface` for custom-painted widgets,
  backed by a CI accessibility static-analysis check.
- **CAT / rigctld parity** — a large round of SmartSDR Flex / TS-2000 /
  rigctld behavior fixes (split VFO, VFO-B dialect capability, the `ZZTX;`
  uncommanded-TX fix).
- **Net Reminder Scheduler** — recurring net reminders with one-click tuning.
- Constitution **v2.0.0** — trims domain conventions (relocated to
  `AGENTS.md`) and adds governance principles; net 14 principles.
- Packaging — SHA256-pinned `third_party` downloads, AppStream metainfo +
  manpage (Flathub prep), and qtkeychain bundled for SmartLink on the
  AppImage and Windows builds.

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