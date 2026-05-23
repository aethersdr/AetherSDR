# AetherSDR Roadmap

Live tracking lives in [GitHub Issues](https://github.com/aethersdr/AetherSDR/issues)
and the per-cycle milestone view. This file is a human-readable snapshot
of what the project lead and core contributors are working on — updated
as direction changes.

For *what shipped*, see [`CHANGELOG.md`](CHANGELOG.md).

## Current cycle: post-v26.5.2.1

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

- **AetherModem Phase 1** — 1200 baud VHF AX.25 packet TX (data-mode
  transmit pipeline using the existing DAX TX path).
- **L1–L4 audit follow-ups** — four low-severity items from the
  2026-05-09 security pass, tracked as
  [#2954](https://github.com/aethersdr/AetherSDR/issues/2954)–[#2957](https://github.com/aethersdr/AetherSDR/issues/2957).
- **Extended region band plans** — DXCC entities outside IARU R1/R2/R3.
- **macOS shmem + RigctlPty audit** ([#2940](https://github.com/aethersdr/AetherSDR/issues/2940))
  — focused security review of VirtualAudioBridge (macOS) and
  RigctlPty (Linux + macOS), follow-up to the audit that found H2.

### Recently shipped

Highlights from the last 30 days — full list in
[`CHANGELOG.md`](CHANGELOG.md):

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