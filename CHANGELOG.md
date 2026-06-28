# Changelog

All notable changes to AetherSDR are documented in this file.
Format follows [Keep a Changelog](https://keepachangelog.com/).

> **Versioning:** Starting with **v26.5.1**, AetherSDR moves to **CalVer**
> (`YY.M.patch`). Earlier tags used semver through v0.9.8.

## [Unreleased]

## [v26.6.5] — 2026-06-28

### KiwiSDR receive sync + SmartMTR TX meters + Profile Switcher applet + agent automation bridge expansion

50 commits since v26.6.4. Headlined by **KiwiSDR receive sync** (GCC-PHAT
audio + visual alignment between the Flex and a public Kiwi), **SmartMTR TX
meters** (SWR / forward-power / compression gauges for the VFO flag), a new
**PROF profile-switcher applet** for live Global/TX/Mic profile selection, and a
large **agent automation/test bridge** expansion (radio connect/disconnect,
display-stream leak detection, custom context-menu inspection, and a
panadapter/waterfall control surface) — plus a broad wave of spectrum, VFO-flag,
and KiwiSDR stability fixes.

### Added

- **KiwiSDR receive sync** — GCC-PHAT Auto-Assist that time-aligns the Flex and a
  public KiwiSDR receiver in both audio and the spectrum/waterfall. (#3872)
- **SmartMTR TX meters** — selectable SWR, forward-power, and compression gauges
  with analog ballistics for the VFO flag meter. (#3776)
- **PROF (Profile Switcher) applet** — live Global / TX / Mic profile selection
  from a sidebar applet. (#3829)
- **Resizable CW decode panel** with an adjustable, persistent font size. (#3824)
- **APRS message-services picker** on the Recipient field. (#3828)
- **RC-28 rotation-sensitivity divider** with auto-snap to 1 kHz. (#3875)
- **SWR sweep CSV export.** (#3882)
- **Master Volume Up/Down configurable keyboard shortcuts.** (#3820)
- **S-meter config context menu** plus a needle pivot cover with a themed
  backlight glow (both tokenized for the theme subsystem). (#3859)
- **Agent automation bridge — major expansion:** radio connect/disconnect
  commands (#3851); radio-side display-stream inventory + leak detection
  (`streams` verb) that surfaces leaked/duplicate panadapter & waterfall streams
  `get pans` can't show (#3856); the `contextMenu` verb to trigger/inspect custom
  right-click menus (#3883); observability fields plus close/drag/showMenu/pan
  verbs, grab pan, and a token-anchored TX guard (#3842); a Panadapter/Waterfall
  control surface — antennas, CWX, floors, pan management (#3832); and bridge
  fidelity for slice TX, key/PTT, menu reachability, lineedit submit, resize, and
  multi-instance station identity (#3819). RX/observe verbs are observe-only; see
  `docs/automation-bridge.md`.

### Changed

- **WFM toggle relocated to the DAX pan menu** so the frequency digits no longer
  clip. (#3853)
- **KiwiSDR audio mutes during non-FDX transmit.** (#3865)
- **AetherDSP Settings now toggle from the DSP-tab ADSP button.** (#3881)
- **PTT (Hold) honors its reassigned key** instead of a hardcoded Space. (#3884)
- **ADSP launcher accents** when a client NR module is active. (#3822)
- **`pan close` verb** drives the production `RadioModel::removePanadapter`
  teardown path instead of duplicating the commands. (#3843)
- **GPU flag-sprite machinery excised** — slice flags are now always live. (#3806)
- CI: SHA-pinned actions drop version comments to stop Dependabot drift (#3867);
  `actions/cache` → 6.1.0 (#3864); `actions/setup-python` → 6.3.0 (#3863).

### Fixed

- **Closing a panafall-created panadapter now frees its waterfall stream on the
  radio** — the teardown now sends the FlexLib-correct Panadapter.Close +
  Waterfall.Close pair through `RadioModel::removePanadapter`. (#3843, #3855)
- **KiwiSDR proxy redirect transport** (proxy → proxy2). (#3850)
- **Kiwi waterfall pan/zoom stalls** — moved off the GUI thread. (#3825)
- **VOX-keyed transmit** now engages the SmartMTR TX meter and audio gate. (#3862)
- **Pan Lock** stands down during a slice drag and recenters on release. (#3786)
- **VFO flag-side pan following** and **diversity-pair flag placement**. (#3866, #3880)
- **Zoomed FFT spectrum rendering.** (#3836)
- **Smooth FFT trace + clamped display scale**, with EMA reset on TX→RX so the
  floor doesn't linger. (#3847, #3831)
- **DAX RX stream create/remove storm** that dropped connection quality to red. (#3796)
- **Keyboard/space PTT** routed through the Quindar coordinator. (#3798)
- **AetherControl window** clamped to the screen height. (#3795)
- **Panadapter split-pair** shown for an externally-initiated split. (#3794)
- **SpotHub clear buttons** persist; counter-desync and console-clear fixes. (#3823)
- **Floating window fill** for width-capped applets. (#3827)
- Automation: widget click/toggle deferred out of the socket read callback (#3826);
  tightened mark→tail log correlation (#3793).
- CWX drift-test teardown heap-use-after-free. (#3799)

### Performance

- **VITA-49 RX buffer** default raised to 4 MiB with an operator-adjustable
  slider — eliminates burst drops and auto-throttle fps caps. (#3811)
- Reuse GPU FFT-trace vertex scratch buffers + guard against a 1-bin spectrum. (#3782)
- Skip overlay re-bake on cursor-move when no readout is shown. (#3780)

### Removed

- Dead `m_overlayDynamic` overlay image (#59a19681), leftover `RxApplet.h.bak`
  (#3818), and a reverted half-baked frequency shrink-to-fit (#3846, #3839).

## [v26.6.4] — 2026-06-23

### KiwiSDR public-receiver browser + SmartMTR meter view + agent automation bridge + GPU-composite flags + accessibility

80 commits since v26.6.3. Headlined by a **KiwiSDR public-receiver
integration**, a selectable **SmartMTR meter view** for the VFO flag, an in-app
**agent automation/test bridge** for the GUI, **GPU-composited slice flags** with
a multi-GPU render selector, a first **accessibility** pass for custom-painted
widgets, and a large round of **CAT/rigctld parity** fixes. Governance moves to
**Constitution v2.0.0**.

### New features

- **KiwiSDR public-receiver browser** — an API-policy-aware directory browser to
  find and connect to public KiwiSDR receivers worldwide, independent of the
  FlexRadio path. Honest, policy-aware receiver picker with the public-directory
  **Limits** marker; diversity receive interlock with receive-only TX inhibit on
  Kiwi panadapters; source-attributed terminal denial messages (badp codes + MSG
  keys); clearer connection-failure messages; dedicated Support-menu logging
  categories. See `docs/kiwisdr-public-directory.md`. (#3668, #3679, #3676, #3678,
  #3699, #3706, #3707, #3716, #3749, #3759)
- **SmartMTR selectable meter view** for the VFO flag — an opt-in alternative to
  the S-meter (which stays pixel-identical when not selected). Analog d'Arsonval
  bar ballistics, sliding-window min/max "extremes" markers (1s/3s/5s), optional
  numeric value labels, and a TX mic-level meter. Renders correctly in the GPU
  flag-sprite path. (#3723, #3750, #3751, #3752, #3753, #3760, #3771)
- **Agent automation / test bridge** — an in-app, agent-drivable bridge for the
  GUI (`dumpTree`/`grab`/`invoke`/`get`, slice/VFO verbs), with TX automation
  hardening (meter freshness, ATU, two-tone, safety rails), input validation,
  redaction of masked fields, and a log-observability suite. Off in production,
  gated behind `AETHER_AUTOMATION`. See `docs/automation-bridge.md`. (#3646,
  #3710, #3722, #3727, #3728, #3738, #3717, #3747)
- **GPU-composite slice flags** instead of translucent raster siblings — removes
  the main-thread re-blend over the QRhi waterfall each frame, plus a multi-GPU
  **render-GPU selector** (Display menu) for multi-adapter systems and a
  flag re-raster pause when a flag isn't presented. (#3617, #3695, #3656, #3746,
  #3713)
- **Accessibility** — `QAccessibleInterface` implementations for custom-painted
  widgets (a Grouping summary for the VFO flag; the ESC level bar), backed by a
  new CI accessibility static-analysis check. See `docs/a11y.md`. (#3754, #3758)
- **Net Reminder Scheduler** — recurring net reminders with one-click tuning.
  (#3684)
- **PSK Reporter map** gains band-condition shading, a lookback window, and a
  hover card, with cross-platform spot-delivery fixes. (#3635)
- **FreeDV Reporter**: 6m+ band coverage and multi-select. (#3591)
- Maidenhead **grid-locator** field in the APRS position row. (#3672)
- **Memory** editor: NUL sanitisation, field dropdowns, smoother editing. (#3657)
- Continuous **edge auto-pan** while dragging a slice. (#3581)
- Status-bar **date** follows the system locale. (#3690)
- **Support bundles** are deflate-compressed. (#3691)
- **Waveform** install over the Docker file-upload protocol. (#3585)

### CAT / rigctld parity

- Resolve SmartSDR Flex/TS-2000 split-behavior gaps + block TS-2000 satellite-mode
  TX. (#3739)
- Resolve rigctld behavior gaps and fix on-demand split VFO. (#3724)
- Match missing Hamlib rigctld functionality — VFO mode, mode mapping, CTCSS/FM
  tones, levels & funcs. (#3619)
- **Behaviour:** VFO B is a dialect capability — disabled for rigctld, preserved
  across dialect switches and smaller-radio reconnects. (#3694/#3698)
- Don't cap running CAT ports by receiver count. (#3693/#3697)
- **Fix (safety):** a bare `ZZTX;` is a read, not a key — no more uncommanded TX
  in the Flex dialect. (#3625/#3629)
- Make the CAT per-port enable toggle visible. (#3618)

### Fixes & hardening

- **TCI DAX RX audio** routed by a cached channel→TRX map, resilient to transient
  `dax=0`; cache cleared on disconnect teardown. (#3669/#3759, #3766/#3767)
- **Crash:** guard the slice DAX-recall single-shot against a dangling slice
  (use-after-free). (#3733)
- **Live state (Principle II):** `setDax`, `setCwSidetone`, `setSbMonitor`, and
  `setAmCarrierLevel` now update their cached state optimistically. (#3737, #3736,
  #3734, #3712)
- Reproject only the active waterfall stream while panning. (#3700/#3701)
- Draw the panadapter grid below the FFT trace in the GPU path. (#3606/#3713)
- Use the radio's auto-black level for an evenly-levelled waterfall floor. (#3586)
- Independently themable **LIVE** chip (red live / grey history). (#3761)
- Pace TX waterfall rows to `line_duration` to match the RX scroll rate. (#3686)
- Profile FFT settling on load. (#3604, #3573)
- Prune dead followers in `AudioOutputRouter`. (#3660/#3661)
- Capture TX audio in client-side QSO recordings. (#3556/#3632)
- Run the CWX local sidetone keyer on a `steady_clock` worker thread. (#3644)
- Stream Deck Tune Toggle parsed the wrong TCI status field. (#3647)
- Migrate hardcoded highlight/disabled-state colours to ThemeManager tokens
  (Principle IX); persist Theme Editor overrides under pre-seeded applet scopes.
  (#3645, #3688)
- Ulanzi dial on Linux: detect an inaccessible evdev device and offer a one-click
  udev grant. (#3677)
- RX BW indicator shows bandwidth, not the hi-cut. (#3659/#3696)
- Idle MOX button gets a distinct accent so it reads as the transmit button.
  (#3763)
- Fan-mode button label clarity; Fahrenheit toggle for amplifier temperature.
  (#3651, #3652)

### Internal — audio sink factory (Phase 6, #3306)

- New `AudioOutputRouter` registry for output-following sinks; Pudu/QSO/CW and
  Quindar sinks finished onto the factory with format negotiated through it.
  (#3630, #3631, #3655)
- Architecture note: WebSDR-sourced VFO/slice design. (#3621)

### Governance

- **Constitution v2.0.0** — trims the domain conventions (relocated to
  `AGENTS.md`) and adds governance principles; net 14 principles. (#3602)

### Packaging / CI

- Build and bundle **qtkeychain** for SmartLink credential persistence on the
  Linux **AppImage** (#3640) and **Windows** (#3634).
- **Pin + verify SHA256** for all `third_party` setup-script downloads. (#3665/#3692)
- **AppStream metainfo** for Linux + Flathub-submission polish; a simple
  **manpage**. (#3673, #3709, #3674)
- Dependabot: `actions/checkout` 6.0.3→7.0.0, `microsoft-store-apppublisher`
  1.1→1.3, `action-gh-release` 3.0.0→3.0.1. (#3682, #3681, #3680)

## [v26.6.3] — 2026-06-14

### Satellite WFM + APRS/PSK-Reporter mapping + packet AFSK + MainWindow decomposition

58 commits since v26.6.2. Headlined by a WFM software demodulator for
satellite data, an APRS client and a PSK Reporter reception map built on a new
reusable Qt mapping engine, a Direwolf-derived VHF packet demodulator, and the
internal #3351 decomposition of the MainWindow monolith.

### New features

- **WFM software demodulator** for satellite data work (G3RUH 9600 bd):
  DAX IQ → SkyRoof-parity DSP chain (phase-continuous NCO Doppler correction,
  exact 48 kHz resampling, flat atan2 discriminator) → virtual audio cable for
  HS-SoundModem. Per-slice WFM toggle on FM modes. (#3407, #3522, #3562)
- **APRS client** — the AetherModem AX.25 tab becomes a lightweight APRS
  client: live station table with wireframe symbol icons and weather decode, a
  timed GPS position beacon (grid-locator fallback), and two-way messaging with
  retries, auto-ack, and digipeat de-duplication — all on the shared one-at-a-
  time TX keying queue. (#3530)
- **PSK Reporter reception map** (View ▸ PSK Reporter) showing who is hearing
  your callsign on an OpenStreetMap basemap — mode-coloured markers, great-circle
  paths, live MQTT feed plus HTTP polling, persistent spot cache. Built on a new
  **reusable Qt mapping engine** (vendored QGeoView, LGPL-3.0) intended for reuse
  by the APRS tab. (#3565)
- **Direwolf-derived AFSK demodulator** for VHF 1200-baud AX.25, replacing
  libmodem's AFSK on the VHF path (HF 300-baud unchanged). A 1,875-packet
  overnight comparator beat both Direwolf and Graywolf on copy rate. (#3527)
- **DAX-IQ fully usable** — end-to-end IQ sample delivery, a dBFS level meter,
  24/48/96/192 kHz rate switching with persistence, and startup state restore.
  (#2529, #3521, #3522)

### Packet radio

- `HdlcCodec`: pure-C++ HDLC framer replacing libmodem's `bitstream_state`. (#3475)
- AX.25 shim isolated on a dedicated `QThread`. (#3473)
- MQTT: per-topic control, CW keyer, radio state, AX.25, debug logging. (#3460)

### Fixes & hardening

- **Behaviour change (CAT):** rigctl/TCI `set_freq` retunes that stay within the
  panadapter span no longer recenter the pan (`autopan=0`); cross-band tunes
  still recenter, preserving the #536 behaviour. Satellite trackers issuing
  Doppler steps every few seconds previously yanked the pan on every step.
- Profile-load recovery hardening for Multi-Flex — suppress/defer profile-owned
  slice/pan/waterfall writes during global profile restore. (#3563)
- DEXP controls now use the `compander`/`compander_level` protocol keys SmartSDR
  actually uses (were silently rejected). (#3568)
- Multi-Flex ping-watchdog grace during a second client's join, so AetherSDR no
  longer false-disconnects mid-join. (#3570)
- TCI: dB on the wire for the VOLUME command; `ready;` sent after the full
  settings dump, not mid-burst. (#3498, #3502)
- ARRL US bandplan license-class accuracy. (#3518)
- Narrow-passband drag hit-testing. (#3523)
- GPU spectrum builds without `Qt6GuiPrivate` (Windows/macOS aqtinstall). (#3561)
- Keep inactive-slice bandwidth visible via a neutral colour rather than
  near-invisible dimming. (#3484/#2389)
- Per-row waterfall-history frequency frames (no per-pan reproject). (#3578)
- TMate2 enhanced display — TX bargraph, text overlays, encoder labels. (#3542)
- RC-28 velocity-proportional tuning. (#3467)
- SmartSDR-parity **"Purple"** waterfall colour scheme — additive and opt-in,
  joining the existing presets. (#3583)

### Internal — MainWindow decomposition (#3351)

- `MainWindow.cpp` reduced from ~19,500 to ~8,100 lines, split across sibling
  translation units (`MainWindow_Controllers/Menus/Shortcuts/Wiring/DigitalModes/
  SwrSweep/Spots/Session/DspApplets.cpp`, `MainWindowHelpers`,
  `MainWindowShortcutState.h`) — pure code motion, no behaviour change. Mapped in
  `docs/architecture/mainwindow-decomposition.md`.
- New **`RadioSession`** aggregate owns `RadioModel` + `TciServer` + `CatPorts`
  per radio, making teardown order structural (fixes the #2385 crash-on-quit
  class). (#3544, #3545)

### Packaging / CI

- Microsoft Store: a `v*` release tag now auto-stages a **draft** submission via
  the `msstore` CLI — safe-by-construction (SHA-pinned action, secrets via env,
  tag + opt-in + fork gates, `--noCommit`), dormant until credentials are set.
  (#3567)

## [v26.6.2] — 2026-06-07

### Theming + HID controllers + packet-radio suite + Windows Store + 206-commit consolidation

206 commits across 18 contributors since v26.5.3. This is a consolidated
**v26.6.2** that rolls up the 26.6.1 feature wave, the v26.6.1.1 hotfixes,
and a large block of new work landed since — headlined by the runtime
theming system, three new HID controller classes, a full packet-radio
suite (KISS TNC, connected-mode BBS terminal, and a personal mailbox),
and the Microsoft Store (MSIX) packaging path for one-click install and
automatic updates on Windows.

### Theming foundation (early beta)

A runtime theming system, opt-in via **Settings → Theme Editor**; Default
Dark remains the shipped default. Token names, the `.aethertheme` format,
and the editor UX are expected to change before stabilising — file issues
with the `theme` label if you try it.

- `ThemeManager` foundation + 51-token design taxonomy, gradient + alpha support
- Theme Editor dialog with live colour editing, gradient editor, font + sizing pickers
- New **Default Light** theme; `.aethertheme` import/export with drag-and-drop
- Embedded DSEG Modern 7- and 14-segment fonts (SIL OFL 1.1)

### HID input devices

Three new physical-device classes, all opt-in so the macOS Input
Monitoring prompt never surfaces unless an operator enables one:

- **Elgato StreamDeck+** — encoders, LCD buttons, touchscreen with live labels (#3236)
- **Ulanzi Dial** — cross-platform on Linux evdev, Windows, and macOS (#3238, #3239)
- **Native Icom RC-28** encoder support (#3293, #3298)

### Packet radio — KISS TNC, BBS terminal & mailbox

AetherModem grows from a decoder into a complete packet station, all over
its built-in 1200-baud VHF (Bell 202) AFSK modem — no external TNC
hardware required:

- **KISS-over-TCP TNC server (#3279)** — turns AetherSDR into a software TNC
  for any host packet/APRS app (Xastir, YAAC, APRSdroid, UISS, Dire
  Wolf-style clients, terminal programs). Cross-platform `QTcpServer` with
  multiple simultaneous clients, resync-safe KISS framing (FEND/FESC), and
  proper lifecycle handling — TCP keepalive, slow-consumer write-backlog
  cap, and an idle sweep so dead clients are reaped, not leaked. **Enable
  TNC**, **Start on Startup**, and **TCP port** (default 8001) controls,
  all persisted.
- **Standalone packet terminal (#3381)** — a built-in connected-mode AX.25
  client in a new Terminal tab: call a 1200-baud VHF packet BBS, read and
  send messages, and disconnect, with reliable error correction (T1
  retries) over the half-duplex link. Verified on the air against a live
  BBS (SJVBBS-1).
- **Personal Mailbox System (PMS)** — the answering-side counterpart: your
  own connected-mode AX.25 mailbox other stations can connect to, sharing
  the same `Ax25Connection` data-link state machine as the terminal.
- Built on the **AX.25 1200-baud VHF (Bell 202) RX + TX** profiles from
  earlier in the cycle (#3253, #3256).

### Windows Store — one-click install & automatic updates

AetherSDR is heading to the Microsoft Store as an MSIX package, so Windows
operators get one-click installation and automatic background updates
instead of chasing installers:

- **MSIX packaging groundwork + Store app identity** (#3178, #3225)
- **Lean Store payload** — DFNR neural-denoiser weights embedded for the
  Store build, with the model archive excluded from the MSIX package to
  keep it small (#3225, #3205)
- **CI builds and attaches the MSIX / `.msixupload`** to every release,
  with the Store-submission automation plan documented (#3281)

### Other new work since v26.6.1

- **Accessibility, Phase 2 (#3303)** — accessible widget names, live
  screen-reader announcements, VFO tab navigation, and full keyboard
  navigation.
- **Ulanzi Studio sibling plugin (#3227)** — `ulanzi-aethersdr` alongside
  the in-app mapper.
- **Filter passband shading fix (#3294)** — premultiplied-alpha
  double-multiply on the GPU path corrected.

### Hotfixes folded in (originally v26.6.1.1)

- **macOS DMG release build unblocked (#3349)** — a removed background image
  plus a masked `|| true` broke create-dmg; Apple Silicon DMG now builds,
  signs, and notarizes cleanly.
- **Tuning-step toast no longer fires on radio syncs (#3337)**.
- **Radio Setup tall tabs stay reachable (#3347)** — tab pages wrapped in a
  scroll area.

### Windows hardening

- PerMonitorV2 DPI awareness (#3208), discrete-GPU preference on hybrid laptops (#3299)
- Windows Snap restoration for the frameless title bar (#3069), WASAPI CW sidetone routing (#3241)

### New protocol surfaces

- **SmartCAT TCP server** — TS-2000 + FlexCAT dialects (#3131)
- **Unified RADE TX pipeline** — EOO frame transmission + callsign encoding (#3221)
- **SSDR-parity PWR/SWR metering** on PGXL/TGXL amplifier applets (#3277)
- TCI `clicked_on_spot` event for Log4OM interoperability (#3145)

### Reliability sweep

- **Faster connect times** — ~1.3 s cut off the connect handshake by
  skipping the peek when multiFLEX is on and pipelining the serial
  subscriptions (#3391)
- **Steadier discovery** — the re-bind loop now pauses while connected and
  fully quiesces on routed/WAN links, so reconnects stop fighting the
  discovery socket (#3420, #3422)
- **7-year-old NR2 Gamma crackling fix** — SpectralNR Bessel function variants corrected (#3275)
- **Multi-monitor main-window restore under Minimal Mode** (#3174)
- **Multi-pan TCI spot freeze** — spot-marker rebuilds coalesced (#3310)
- **FreeDV / Quindar interactions** (#3317, #3320) + RADE filter passband alignment (#3301)

### Build, CI, and packaging

- **`check-windows` + `check-macos` always run** on every PR (#3244)
- **Sanitizer step pinned to bash** so `pipefail` works (#3155); **SOURCE_DATE_EPOCH** reproducible builds (#3165)
- **System-library opt-in flags** + lowercase binary name + Linux 256×256 icon + `.desktop` description (#3135, #3138, #3143, #3074 — @dawkagaming)
- **Debian multiarch tuple for Qt6GuiPrivate probe** (#3159 — @K5PTB)

### Contributors

Big thanks to **@ten9876** (maintainer, 45 commits — theming system
end-to-end, HID device backends, CI gating), **@aethersdr-agent** (the
AetherClaude orchestrator, 39 commits — automated fixes across slices,
controllers, and applet paths), **@jensenpat** (38 commits — the entire
packet-radio suite: KISS TNC, BBS terminal, PMS mailbox, AX.25 1200-baud,
plus UI and slice work), **@M7HNF-Ian** (15 commits — meters, WASAPI
sidetone, spectrum tooltips, CI paths), **@NF0T** (13 commits — RADE TX
pipeline, NR2 Gamma fix, Quindar interactions), **@K5PTB** (9 commits —
SmartCAT server, MQTT publish topics, Debian multiarch), **@nigelfenton**
(9 commits — TCI mic/AF handlers, theme namespaces, Ulanzi sibling
plugin), **@chibondking** (6 commits — PWR/SWR metering, adaptive
throttle), **@rfoust** (4 commits — panadapter context slice, waterfall
rate), **@mvanhorn** (2 commits — theme migration tool cleanup), and
**@Ozy311** (2 commits — meters + slices). Dependabot contributed 2
dependency and security bumps.

We are excited to welcome our first-time contributors this cycle:
**@dawkagaming**, **@w5jwp**, **@motoham88**, **@w9fyi**, **@ea5wa**,
**@svabi79**, and **@VU3ESV**.

73, Pat KI6BCJ & Claude (AI dev partner)

## [v26.6.1.1] — 2026-06-02

### Hotfix: macOS DMG release build + two GUI fixes

Three fixes on top of v26.6.1, no other changes:

- **fix(ci): unblock macOS DMG build broken by missing background image
  (#3349)** — the `create-dmg` invocation referenced
  `docs/assets/logo-invert.png`, which had been removed; the trailing
  `|| true` masked the failure and Sign DMG then failed on a DMG that was
  never produced. Drops the missing `--background`, adds an explicit
  `test -f` guard.
- **fix(gui): only toast "Step: …" on deliberate step changes, not radio
  syncs (#3337)** — the tuning-step toast no longer fires on radio-driven
  status syncs, only on deliberate user step changes.
- **fix(radio-setup): wrap tab pages in QScrollArea so tall tabs stay
  reachable (#3347)** — tall Radio Setup tabs are scrollable instead of
  clipping controls off-screen.

## [v26.6.1] — 2026-06-01

### HID input devices + Windows hardening + new protocol surfaces + 143-commit reliability sweep

143 commits across 14 contributors landed in this cycle. **HID input
device support** is the most user-visible new work, landing three new
device classes: the **Elgato StreamDeck+** (encoders + LCD buttons +
touchscreen labels), the **Ulanzi Dial** (cross-platform on Linux
evdev / Windows / macOS), and **native Icom RC-28** encoder support —
all opt-in so the macOS Input Monitoring prompt never surfaces unless
the operator explicitly enables one. **Windows hardening** sweeps
PerMonitorV2 DPI awareness, discrete-GPU preference on hybrid laptops,
MSIX packaging groundwork with embedded DFNR weights, Windows Snap
restoration for the frameless title bar, and WASAPI sidetone routing.
New protocol surfaces include the **SmartCAT TCP server** (TS-2000 +
FlexCAT dialects), a **unified RADE TX pipeline** with EOO frame
transmission and callsign encoding, **1200-baud VHF AX.25** RX + TX
via the in-process modem, and **SSDR-parity PWR/SWR metering** on the
PGXL/TGXL amplifier applets. A long-tail reliability sweep clears a
**7-year-old NR2 Gamma crackling bug** in the SpectralNR path (#1507),
fixes the **multi-monitor main-window restore** under Minimal Mode
(#2483), and resolves the **multi-pan TCI spot freeze** (#2481).

This release also lands the **groundwork for a runtime theming
system** — `ThemeManager` foundation, a 51-token design taxonomy, a
Theme Editor dialog, and a Default Light theme alongside the existing
dark. The theming work is **early beta**, opt-in via Settings → Theme
Editor; the Default Dark theme remains the shipped default. Token
names, the `.aethertheme` file format, and the editor UX are all
expected to change before stabilising. See the dedicated section below
for the phase-by-phase detail.

Big thanks to **@jensenpat** (22 commits — TCI/CAT, multi-monitor
restore, network diagnostics), **@aethersdr-agent** (the AetherClaude
orchestrator, 28 commits — landing across spectrum, audio, spot and
applet paths), **@NF0T** (10 commits — Windows packaging + DPI + DFNR
embedding + RADE TX pipeline), **@nigelfenton** (8 commits — TCI
fixes), **@M7HNF-Ian** (7 commits — XVTR, slice spawning, NR2 Gamma
fix), **@chibondking** (6 commits — bandplan corrections, panadapter
context slice spawning), **@K5PTB** (5 commits — MQTT publish topics,
CMake Debian multiarch fix), **@dawkagaming** (4 community PRs —
system-library opt-in flags, lowercase binary name, Linux icon size,
`.desktop` description), **@rfoust** (4 commits), and first-time
contributors **@w5jwp** (Icom RC-28 encoder support), **@motoham88**
(StreamDeck+ support), **@mvanhorn** (theme migration tool cleanup),
and **@VU3ESV** (macOS `phys_footprint` memory reporting).

### Headline features

**End-to-end theming system (#3076, Phases 1–6)**

Six-phase migration from scattered hex-literal paint calls to a
canonical token-based theming system:

- **Phase 1 — `ThemeManager` foundation** (#3077).
- **Phase 2 — 51-token design taxonomy** (#3080), gradient token support
  (#3084), widget→tokens reverse map with live re-theme (#3085),
  ComboStyle pilot migration to `applyStyleSheet` + tokens (#3087),
  app-wide stylesheet migration with `applyAppTheme()` (#3090), and the
  mass migration across 59 files / ~1000 hex literals (#3102). Audit
  tool `tools/audit_colours.py` shipped to drive the inventory (#3078).
- **Phase 3 — paint-code migration** in tranches: `MeterSlider`
  (#3106), the migration tool itself + `SpectrumWidget` (#3113),
  file-scope `const QColor` in comp/gate/curve widgets (#3116), and
  the spectrum + chain + waveform paint-code closeout (#3117).
- **Phase 4 — Default Light theme** ships alongside the existing dark
  default (#3129).
- **Phase 5 — Theme Editor dialog**: live colour editing + Save As
  (#3130), inspector mode + 505-site reverse-map sweep (#3144), alpha
  pipeline + `themeRegions` + glass-mode backdrop (#3148), gradient
  editor + meter-bar gradient + flat↔gradient conversion (#3158), font
  + sizing pickers + per-token reset + theme rename/delete (#3160).
- **Phase 6 — `.aethertheme` import/export with drag-and-drop** (#3164).
- **DSEG Modern fonts embedded** (SIL OFL 1.1) for the 7-segment and
  14-segment displays (#3163).

Plus the inline Theme Editor with auto-persist + built-in theme
protection (#3176), per-applet/per-tribe scoping for toggle-button
namespaces (#3198), per-applet overrides for sliders + knobs (#3188),
waterfall colormap runtime theming for all five presets (#3122), slice
indicator runtime theming (#3121), and the Theme Editor UX polish round
(#3199). Cleanup PRs along the way tokenized leftover slider call sites
(#3204), swapped `kGreenToggle` for `color.background.success` (#3195),
and rescued misattributed migration-tool comments (#3125).

**Elgato StreamDeck+ support (#3236, follow-up #3250)**

First-class StreamDeck+ integration — encoders, LCD buttons, and the
touchscreen with labels. Three robustness gaps surfaced in initial
review were addressed in the follow-up.

**Ulanzi Dial cross-platform support (#3238, #3239)**

Linux evdev backend with a visual configurator (#3238), then Windows
and macOS backends with a cross-platform mapper (#3239). Both
StreamDeck+ and the Ulanzi Dial are gated behind opt-in toggles in
Settings so the macOS Input Monitoring prompt only appears when the
operator explicitly enables either device (#3257).

**Icom RC-28 USB encoder support (#3293, #3171)**

Initial Icom RC-28 native support (#3293); also recognises a
community-built `aether-pad` RC-28 emulator via Arduino VID/PID alias
(#3171).

**SmartCAT TCP server (#3131)**

New TCP CAT server supporting both the Kenwood TS-2000 and FlexCAT
dialects. Joins the existing TCI, Hamlib NET rigctl, and per-slice
rigctld surfaces for CAT interop without external bridges.

**Unified RADE TX pipeline (#3221)**

RADE TX path consolidated, with EOO frame transmission and callsign
encoding embedded in the protocol — eliminates the previous
codec-specific tail handling and brings callsign metadata onto the
wire alongside the audio frames.

**HLTH antenna health applet (#3153)**

New applet for antenna health monitoring, built on the customizable
button-bar refactor (#3150).

**Customizable button bar (#3150)**

Favourites + push-down drawer pattern for the main button bar — operators
can pin frequently-used controls and let the rest live in the drawer.

**SSDR-parity PWR/SWR metering on PGXL/TGXL applets (#3277)**

The PGXL and TGXL amplifier applets gain SmartSDR-aligned PWR/SWR
meter rendering.

**Adaptive throttle UX (#3175, #3203)**

Network-congestion-adaptive panadapter throttle gets an explicit
toggle in the Connect panel for all modes (was previously WAN-only),
plus visibility through the status-bar heartbeat colour and the
Network Diagnostics dialog graphs.

**Bell 202 AX.25 1200-baud VHF (#3253, #3256)**

Receive (#3253) then transmit (#3256) profiles for 1200-baud VHF AX.25
packet radio via the in-process modem.

**MQTT publishes CW decoded text + publish-button topics surface (#3216, #3251)**

CW decoder output now publishes to `aethersdr/cw/decode` (#3216);
internal publish topics are visible in the Publish Buttons settings
tab (#3251).

**Reference target curve on Aetherial Parametric EQ (#3259)**

Visual reference overlay for matching a target frequency response.

**AetherControl double-click to latch (#3103)**

Replaces the click+ESC asymmetry with a single double-click gesture
for the latch/unlatch action.

**TCI clicked_on_spot event for Log4OM (#3145)**

TCI spot-click notification surface added for Log4OM interoperability.

**LAN peripheral auto-reconnect**

Networked peripherals auto-reconnect when the LAN comes back, mirroring
the existing radio-side reconnect logic (Principle V.).

**ESC click-to-adjust polar display (#3134)**

Polar display for phase and gain now adjusts directly on click.

### Long-tail bugs landing in this cycle

**NR2 Gamma crackling — 7-year-old SpectralNR bug (#1507 → #3275)**

Replaces unscaled Bessel `I0`/`I1` calls in the SpectralNR path with
the exponentially-scaled variants, eliminating the long-reported
crackling on NR2 Gamma mode. Originally reported in 2019.

**Multi-monitor main window restore in Minimal Mode (#2483 → #3174)**

Window geometry restores to the saved screen even when AetherSDR
launches in Minimal Mode (Principle XIV.).

**Spectrum tooltip strobing + child tooltips (#2355 → #3209, #3233)**

Spot tooltips no longer strobe on overlay rebuilds; `SpectrumWidget`
no longer kills child-widget tooltips.

**Spectrum trace below background image (#3124)**

Z-order regression on the GPU path — FFT trace was being painted below
the background image. Restored to above.

**Waterfall rate slider behaviour (#3104) + paced fallback restore (#3182)**

Slider behaviour fixed; paced RX waterfall fallback path restored.

**Multi-pan TCI spot freeze (#2481 → #3310)**

Spot-marker rebuilds coalesced so multi-pan TCI sessions no longer
freeze when a burst of spots arrives.

**FlexControl Aux LED restoration (#2908 → #3269)**

Aux LED state restored when the device sends an F-reset (Principle VIII.).

**FlexControl encoder accumulator (#3260 → #3267)**

Encoder accumulator now snaps to the step grid and resets on absolute
tune (Principle XI.).

**Tuner BYPASS → STANDBY transition (#3140 → #3147)**

Clear bypass cleanly on the transition (Principle III.).

**S-Meter pulsing during PGXL OPERATE (#2927 → #3172)**

Analog S-Meter no longer pulses when the PGXL amplifier is in OPERATE
mode (Principle II.).

**S-Meter MIC vs MICPEAK in Level TX (#3187 → #3191)**

Level TX mode now reads the MIC meter, not MICPEAK.

**FCC §97.301(d) 15m Extra phone edge (#3136 → #3137)**

Bandplan edge corrected to 21.200 MHz (Principle IV.).

**License-class filter invariant (#3060 → #3089)**

`contiguousRegionsForBand` walker pins the filter-before-merge
invariant when license classes are active (Principle IV.).

**ISED license_classes added to RAC Canada plan (#3061 → #3088)**

Canadian band-plan data now carries the ISED license-class structure
matching the US ARRL plan format (Principle IV.).

**Squelch save pairing + slice detach reset (#3263 → #3268)**

Squelch state pairs with action and resets cleanly on slice detach
(Principle I.).

**FlexLib-canonical EQ command case (#3185 → #3265)**

TX/RX EQ commands emitted in the case FlexLib expects (Principle I.).

**Spot smart filter exempts memory bookmarks (#2894 → #3190)**

Memory bookmarks no longer caught by the spot smart filter; stale
S-history cache cleared.

**EQ slider inverted-fill paint (#3280)**

Inverted blue fill on EQ band sliders corrected.

**RF/Tune power slider unit labelling (#3284 → #3285)**

Sliders relabelled as percent (not watts) — matches what the radio
actually accepts (Principle I.).

**FreeDV mode filter polarity (#3092 → #3101)**

FDV modes excluded from the SSB filter polarity flip — wasn't applicable
to FreeDV's symmetric passband (Principle I.).

**Quindar interactions (#3317, #3320)**

Quindar outro timer routed through `dispatchMoxOff()` (#3317); FreeDV
modes excluded from the Quindar tone allowlist (#3320).

**RADE filter passband alignment (#3301)**

Filter passband aligned with the FreeDV waveform convention rather than
the legacy AetherSDR-internal definition.

**DVK WAV transfer crash on teardown (#2501 → #3309)**

WAV transfer teardown made idempotent — fixes a crash on rapid
disconnect/reconnect cycles.

**Slice spawn on empty-pan click (#3086 → #3123)**

Clicking on an empty panadapter now spawns a new slice instead of
hijacking the active slice (Principle I.).

**Panadapter context slice creation (#3095)**

Right-click menu surface for explicit panadapter-context slice spawn.

**TCI master AF volume seeding (#3245)**

Connect init burst seeds master AF volume so clients see the correct
value on first connect.

**TCI DAX RX re-arm (#3270 → #3282)**

DAX RX re-arms when the radio or slice arrives after `audio_start`
(Principle I.).

**TCI mic_level handler (#3234)**

Bridges existing `setMicLevel` to the TCI surface.

**TCI spot-click notification polish (#3152 → #3170)** — Principle I.

**WASAPI Float32 fallback (#3231)**

`preferredFormat()` fallback for output devices that don't accept the
default Float32 format (Principle VIII.).

**RX mute lift on all startPlayback bail paths (#3230)**

Closes a code path where a `startPlayback` bail-out left RX muted.

**WASAPI CW sidetone host API (#3193 → #3241)**

CW sidetone forced to WASAPI on Windows for low-latency keying.

**NR2 FFTW wisdom cancellation (#3100)**

Status logging cleaned up; wisdom cancellation no longer aborts the NR
chain mid-flight.

**NetworkDiag adaptive-throttle slot cleanup (#3235)**

Removed `Qt::UniqueConnection` from adaptive-throttle lambda slots —
unique connections don't work with capturing lambdas and were silently
duplicating connections.

**Adaptive-throttle cap gating (#3201)**

`ensureOwnedPanadapter` cap path now guarded by `AdaptiveThrottleEnabled`
check.

**XVTR LO Error units (#3272) + Max Power clamp (#3273)**

Display fix; pre-send clamp.

**RPRT terminator on bare-mode rigctl getters (#3120 → #3127)**

All bare-mode rigctl getters now terminate with `RPRT` for parser
compatibility (Principle I.).

**WSJT-X startup CAT lock-mode timeout (#3115)**

Fixed the lock-mode handshake timeout that was breaking WSJT-X startup.

**SSB filter presets aligned to 100 Hz low cut + label-width passband (#3292 → #3295)** — Principle I.

**Transmit micStateChanged emission (#2180 → #3240)**

`setMonGainSb` now emits `micStateChanged()` so monitor-gain edits
surface to the wider transmit-state graph.

**Spectrum tooltip strobe suppression (#3233)** — Principle VIII.

**aethercontrol click+ESC vs double-click (#3103)** — see headline features.

**Discrete GPU preference on Windows hybrid laptops (#3299)** —
Principle XI.

**Channel Strip TX/RX BYPASS suppresses RN2 + RN2 in presets (#3054 → #3066)** — Principle VI.

**aether-pad Arduino emulator recognised (#3171)** — see Icom RC-28
section above.

**HID hotplugCheck full scan + HAVE_HIDAPI guard (#3298)**

Hotplug scan now does a full pass instead of stopping at the first
matching device; `HAVE_HIDAPI` guard fixes for the no-HID build path.

**StreamDeck+ macOS shutdown deadlock (#3255)**

Stop() and HID cleanup ordering corrected — UlanziDialMacOS stop()
now releases its mutex before the runloop teardown.

**HaliKey Serial DCD as third input pin (#3126)**

Adds DCD alongside CTS/DSR as a configurable serial input.

**macOS bitmap cursor crash avoidance (#3099)**

Workaround for a Qt + macOS interaction at certain cursor sizes.

**macOS Ctrl+M minimal-mode shortcut on Windows (#3098)**

Was Mac-only by accident; now binds on Windows too.

**Show Qt menu separators on Windows (#3094)**

Restored separator rendering after a Windows-specific style regression.

**Windows Snap restoration for the custom title bar (#3069)**

`WS_THICKFRAME` + hit-testing dance restored so the frameless title
bar can use Windows Snap.

**Local CWX sidetone keyer drift correction (#3202, #3271)**

Drift correction implemented (#3202); regression test added (#3271).

**CWX macro fire log + ESC abort strikeout (#3146 → #3149)**

Macro fires logged in history; strikeout rendering on ESC-aborted
fires.

**Slice troubleshooter diagnostics (#3132)**

Extra signals for the slice-triage flow.

**About dialog: active pan renderer (#3097)**

Surfaces whether the GPU or QPainter spectrum path is active.

**About dialog memory accounting on macOS (#3197 → #3207)**

Status-bar memory line uses the macOS `phys_footprint` rather than the
VM-size proxy.

**TGXL/PGXL socket release on shutdown (#3079 → #3083)** — Principle I.

**Slice indicator + waterfall colormap runtime theming (#3121, #3122)**

See theming section.

**POTA API polling default → 60s (#3168 → #3169)** — Principle XIII.

**Background image renders BELOW spectrum trace (#3124)** — see
spectrum section.

**Theme Editor filter persistence + reset semantics + factory-loader v2 (#3199)**

UX polish round on the Theme Editor dialog.

### Refactor & internals

- **Primary-slider style consolidation (#3118)** — 8 divergent inline
  styles consolidated into one `Theme.h` helper.
- **`connectSliderSetting` helper (#3032 → #3108)** — slider →
  `AppSettings` binding extracted; `WaveApplet` and `PanadapterApplet`
  migrated.
- **License-class filter-before-merge invariant (#3060 → #3089)** —
  band-plan walker invariant pinned.
- **ContainerWidget drag-MIME documentation (#3056 → #3096)** —
  TXDSP-style alias documented at the drag MIME-set call site
  (Principle III.).

### Diagnostics & support bundles

- **macOS `phys_footprint` in status-bar memory (#3197 → #3207)** — uses
  the macOS-native memory accounting rather than the VM-size proxy.
- **About dialog active pan renderer (#3097)** — surfaces GPU vs
  QPainter spectrum path.
- **Slice troubleshooter diagnostics (#3132)** — extra signals for the
  triage flow.
- **Network Diagnostics adaptive-throttle visibility (#3203)** —
  heartbeat colour + dialog graphs.

### Build, CI, and packaging

- **`check-macos` build gate for hot platform-guarded files (#3223)** —
  mirrors the `check-windows` pattern; covers AudioEngine/MainWindow.
- **AudioEngine added to `check-windows` filter (#3052 → #3210)** —
  catches WASAPI-related regressions before they hit main.
- **Always-on `check-windows` + `check-macos` on every PR (#3244)** —
  retires the path-filter allow-list. Cost: ~15–20 min runner time per
  PR; benefit: closes a recurring regression class. See PR body for
  the receipts.
- **Sanitizer step pinned to bash (#3155)** — `pipefail` + `PIPESTATUS`
  bashisms work; the previous Dash default was silently swallowing
  ctest exit codes and reporting clean runs.
- **CMake `/MANIFESTINPUT` + `/MANIFEST:EMBED` pairing (#3237)** —
  manifest embedding on MSVC.
- **`SOURCE_DATE_EPOCH` for reproducible builds (#3165)** — distro
  packagers can pin build timestamps.
- **Debian multiarch tuple for Qt6GuiPrivate probe (#3159)** — fixes
  the build on Debian-derived distros where Qt private headers live
  under the multiarch tuple path.
- **liquid-dsp AltiVec SIMD gate (#3072)** — only probe on PowerPC
  hosts.
- **liquid-dsp WIN32 link-flag guard (#3220)** — `-lc/-lm` only on
  POSIX.
- **System-library opt-in flags (#3135 — @dawkagaming)** —
  `USE_SYSTEM_ZLIB`, `USE_SYSTEM_MSPACK`, `USE_SYSTEM_LIBMOSQUITTO`,
  `USE_SYSTEM_RTMIDI`. Off by default so existing build hosts continue
  to use vendored snapshots; distro packagers turn them on to match
  dynamic-linking policy.
- **Lowercase binary name option (#3138 — @dawkagaming)** — convenience
  for distro packagers.
- **Linux 256×256 icon (#3143 — @dawkagaming)** — correct icon size for
  the Linux `.desktop` location.
- **Improved `.desktop` description (#3074 — @dawkagaming)** — Linux
  desktop integration polish.
- **Windows MSIX packaging groundwork (#3178)** + **embed DFNR model
  for Store (#3225)** + **exclude DFNR model archive from MSIX
  package (#3205)** — preparation for Microsoft Store distribution.
- **Native stored ZIPs for support bundles and profile backups (#3206)**
- **Theme-manager test fixture realignment (#3161 → #3166)** — fixture
  realigned with default-dark v1.3 token table; nullptr-deref guarded
  (Principle VIII.).
- **PerMonitorV2 DPI awareness on Windows (#3208)**
- **macOS bitmap cursor crash workaround (#3099)**

### Documentation

- **`docs/assets/`** — AetherSDR light + dark theme screenshots added,
  unused `logo-invert.png` removed (#824d58e6, #4670d1d6).
- **CODEOWNERS Tier 3 expansion (#3177)** — @chibondking added to the
  reviewer roster.
- **`ContainerWidget` drag-MIME cross-reference (#3096)** — code
  comment cross-referencing the TXDSP-style alias fallback (Principle
  III.).
- **CHANGELOG attribution corrections for v26.5.3 (#3067)** —
  @nigelfenton (Nigel G0JKN) and @G6PWY-Chris contributor handles fixed
  retroactively after they reported the misattributions directly.

---

## [v26.5.3] — 2026-05-24

### Aetherial Audio TX completion + security hardening + 100-commit reliability sweep

139 commits across 14 contributors landed in this cycle. The headline
work is the **TX path of the Aetherial Audio Channel Strip** reaching
feature-complete with the new PAPR processor and split-band de-esser,
**two published security advisories** landing their enforcement phases
(SmartLink TLS cert pinning + CAT PTY symlink hardening), the **MQTT
configuration moves into a dedicated Settings dialog** (separating
config from the live applet), and a substantial reliability sweep
across audio (WASAPI / CoreAudio / macOS mic), spectrum (TX waterfall
scroll rate, panadapter dBm prime), and CAT (rigctld, TCI, MIDI).
Native Hamlib NET rigctl ships as a first-class CAT path alongside TCI.

Big thanks to **@jensenpat** (16 commits — macOS audio, Windows COM
handle, TCI architecture), **@NF0T** (Ryan B, 12 commits — Windows
installer + RADE + various), **@rfoust** (Robbie Foust, 9 commits —
FlexControl wheel UX overhaul), **@Ozy311** (5 commits — BYPASS state
persistence, applet float/dock title-bar sync, ProfileManager auto-save
signal, DX Spots slider cap, BYPASS tooltip), **@nigelfenton** (Nigel
Fenton, G0JKN, 5 commits — TCI Network dialog tab, tx_gain + ALC, use-
after-free fix, vfo emit on band-change, mic-capture suppress during
TCI feed), **@chibondking** (CJ Johnson, 5 commits — panadapter dBm
prime fix), **@M8WLO** (Andy, 4 commits), **@s53zo** (2 commits —
substantial MQTT settings dialog refactor with self-found subscription-
state fix), **@pepefrog1234** (2 commits — the macOS silent-SSB-TX fix
+ set_split_vfo ping-pong fix), **@K5PTB** (2 commits), **@G6PWY-Chris**
(Chris G6PWY, 1 commit), and **@aethersdr-agent** (the AetherClaude orchestrator,
22 commits — automated fixes on `aetherclaude-eligible` issues).
Dependabot contributed 6 security and version bumps.

### Security advisories landed in this release

**GHSA-wfx7-w6p8-4jr2 — SmartLink TLS cert pin enforcement (Phase 2, #3026)**

Phase 1 (warn-only) shipped in v26.5.2; Phase 2 promotes the cert-
fingerprint mismatch to a **hard handshake-pause**. On a mismatch the
client emits a modal dialog (Accept new cert / Reject and disconnect)
before sending `wan validate`, so a MITM-side attacker never sees an
authenticated session. New SmartLink tab in Radio Setup lists pinned
certificates with per-row Forget and Forget All. Pin cache schema
migrates Phase 1 string entries to Phase 2 `{fp, pinnedAt}` objects
on first save.

**GHSA-qxhr-cwrc-pvrm — RigctlPty symlink out of /tmp (#3027)**

The convenience symlink for CAT software auto-discovery now lives in
`$XDG_RUNTIME_DIR/aethersdr/cat-A` (Linux) or
`~/Library/Caches/AetherSDR/cat-A` (macOS) instead of the cross-user
`/tmp` location. Atomic symlink replacement via `symlink(.tmp)` +
`rename(.tmp, final)` closes the TOCTOU window the old
unlink-then-symlink had on non-sticky filesystems.

Additional defense-in-depth fixes: PII log-redactor regex tightening
(#2954), read-buffer caps on radio/WAN/DxCluster sockets (#2955),
callsign sanitisation in WAV recording paths to handle `/M`/`/P`/`/MM`
(#2956), atomic-rename log symlink (#2957).

### Headline features

**Aetherial Audio Channel Strip TX path completion (#3024)**

- **PAPR processor** — new all-pass biquad cascade (4 stages at
  300/700/1500/2500 Hz) for peak-to-average-power-ratio reduction.
  Drive (0–18 dB) + Phase knobs in the channel strip; auto-makeup gain
  linked to Drive so RMS lifts alongside peaks instead of disappearing
  into the compressor's gain reduction.
- **Split-band de-esser** — replaces the old broadband-attenuation
  pattern (`l *= gainLin; r *= gainLin;`) with
  `output = full + bandpass × (gain − 1)`. Fixes the broadband-
  attenuation bug that was costing operators ~30W of forward power on
  voice content (reported externally as "DESS reduces TX power to ~70W
  of 100W expected"). User-selectable cascade stages (12/24/36/48 dB/oct)
  via a left-aligned toggle in the DESS panel.
- **Peak-hold toggle** on all meters; CRST + RMS + THRESH styling
  unified via the canonical `MeterSmoother` (30 ms attack / 180 ms
  release at 120 Hz).
- **10 Hz throttle** on text readouts; live response at 125 Hz —
  meters track the audio without spamming the GUI thread.

**Native Hamlib NET rigctl implementation (#2975)**

Full 149-test rigctld implementation for WSJT-X / JTDX / fldigi /
Winlink VARA / N1MM+ / DXLog interoperability without a standalone
rigctld bridge. Slices A–H each expose a TTY (per-user runtime path
on Linux + macOS, see #3027) and a TCP port (4532–4539).

**TX waterfall scroll rate matches RX (#3031)**

After #3019 stabilised TX waterfall rendering across slices, the FFT-
derived row path scrolled at `line_duration` cadence (~10 rows/sec)
while RX native tiles arrived at the FFT rate (~30 rows/sec) — TX
appeared 3× slower than RX. Diagnosed via inline probe; corrected by
dropping the FFT row throttle and emitting one row per FFT frame,
matching native-tile behaviour. The original assumption that native
tiles arrive at `line_duration` was wrong; they arrive at FFT rate.

**WASAPI mono-only USB PnP mic silent-open recovery (#2929)**

Some USB PnP mics report mono-only capture natively; Qt accepts an
unsupported stereo open and returns a non-null `QIODevice` that
delivers zero bytes. Three-layer fix:

1. Pre-emptive channel-count clamp from `dev.maximumChannelCount()`
   before format selection (catches the obvious mono-only mics).
2. Fallback-ladder fix to skip only the exact failed `(rate, ch)`
   combo instead of the whole rate (catches mono-only mics whose
   fallback was suppressed).
3. 1.5 s silent-open watchdog that tears down and retries as mono if
   no bytes arrive — covers mics that lie about `maximumChannelCount`.

**Panadapter dBm range prime on reconnect (#3034 — @chibondking)**

Secondary panadapters (Slices B–H) could go completely flat on
reconnect with the dBm scale stuck at the default `[-50, +50]` while
the radio's saved range was different. Cause traced end-to-end: noise-
floor auto-adjust animated from the wrong baseline, fired
`dbmRangeChangeRequested` with bogus values, and the `pendingDbm`
guard locked them in. Fix: prime the spectrum widget with the pan's
current dBm range immediately on wire so the auto-adjust starts from
the right baseline. Solid community contribution.

**FlexControl wheel tuning overhaul (#3029 — @rfoust)**

Two-axis quality-of-life improvement to the virtual FlexControl wheel:

- New **Mouse Sensitivity** slider alongside the existing Wheel
  Tightness, with midpoint (50) returning `1.0` scale — old behaviour
  preserved as default.
- **De-jitter**: clamps single-event pointer deltas to 15° (`π/12`),
  prevents huge wheel kicks from fast mouse jumps.
- **Lazy re-anchoring**: when the pointer crosses through the centre
  dead-zone, the anchor is dropped; next movement out of the dead-zone
  re-anchors without computing a delta. Stops the "wild swing on
  centre crossing" pattern.
- **Coast / pointer clock separation**: coast only starts when both
  instant and blended velocities exceed 4.0 rad/s; pointer input within
  32 ms of a tick suppresses coast ticks. Slow mouse drags no longer
  trigger free coasting.
- UI: centred slider title labels, Tight/Loose + Less/More endpoints,
  two-column layout.

**ATU pre-tune Auto-mode safety nets (#3050)**

Four-layer guardrail bundle for the unattended ATU sweep:

- **License-class filter** (data-driven from active bandplan's optional
  `license_classes` block — ARRL US ships with `{T, G, E}` populated;
  combobox hidden when active plan has no class structure). Prevents a
  Tech-class operator from keying into General/Extra-only sub-bands
  during the unattended sweep.
- **Max-points soft cap** at 100 with mm:ss TX-duration warning.
- **High-power warning** (warn, not refuse, per Principle XIII) when
  Auto-mode tune power exceeds 20 W.
- **Audible cue** on per-point timeout + 3-fail bypass abort.

**ATU pre-tune per-band early exit (#3063)**

Three consecutive fail-bypass on a band now skips past that band's
remaining points and continues on the next band instead of aborting
the whole sweep. Useful when only one antenna is bad (broken feedline
on 80m, fine on 40m/20m/15m).

**Windows hybrid-GPU support (#1921)**

`NvOptimusEnablement = 1` and `AmdPowerXpressRequestHighPerformance = 1`
exports so the Windows binary launches on the discrete GPU on hybrid
laptops. Sidesteps the Intel iGPU D3D11 driver bug that crashed
AetherSDR during `QRhiWidget` reparenting (panadapter pop-out).

**Status-bar slice-locked toast (#2984)**

LOCKED feedback for blocked slice tuning surfaces as a status-bar
toast for 2 seconds. Centralised in `SliceModel` — previously each
tune-feedback site managed its own timer.

**TCI panadapter spectrum forwarding (#2841) + tx_gain + ALC (#2950)**

TCI server forwards panadapter FFT rows to subscribed clients (third-
party tools can render AetherSDR spectrum). New `tx_gain` command +
ALC reading exposed via `tx_sensors`.

**TCI TX overflow-mode picker (#3065)**

Right-click the TCI TX slider to choose how out-of-range (>1.0)
samples from digital-mode clients are handled before the radio sees
them: **Clip** (saturating ±1.0, legacy default), **NaNGuard** (pass
through bit-exact; only zero NaN/Inf), or **Measure** (true bypass —
count overshoots for telemetry, never mutate samples). WSJT-X / FT8
operators chasing bit-exact tone fidelity can drop the float-domain
limiter entirely. Default stays Clip so existing users see no
behavior change.

**AetherControl virtual FlexControl controller (#2888)**

Software-only FlexControl panel mirrors the physical USB knob layout.
Settings → FlexControl.

**Adaptive frame-rate throttle + graceful reconnect (#2829)**

Network-congestion-adaptive panadapter FPS throttle; reconnect
semantics that don't drop slice state on transient TLS drops.

**Mute-all-slices + reconnect mute fix (#2833)**

Master mute button in the RX panel; fixes stale mute display when
reconnecting to a radio.

**Spectrum Ctrl+wheel zoom anchored on cursor (#1518 / #2869)**

Ctrl+wheel zoom now centres on the cursor frequency rather than the
visible centre, matching common spectrum-analyser UX.

**multiFLEX dashboard disconnect controls (#2981)**

Per-client disconnect buttons in the multiFLEX dashboard.

**MQTT settings dialog refactor (#3051 — @s53zo)**

Substantial community PR that moves MQTT configuration out of the live
applet into a dedicated **Settings → MQTT…** dialog. The applet stays
as the live surface (connect/disconnect, status, message log, publish
buttons); the dialog owns broker credentials, TLS/CA config,
subscription rows, display-on-panadapter flags, and publish button
definitions. Persists the On/Off state as `MqttEnabled` for auto-
reconnect on startup. Also self-found and fixed a subscription-state
bug where removed topics could survive across reconnects.

Maintainer follow-up commits on top of the PR (subscribe-diff
optimisation; consolidation of the 8 MQTT flat AppSettings keys into a
single nested `MqttSettings` JSON block per Principle V, with one-shot
transparent migration from legacy keys).

**Loop antenna controls (#2863)**

Antenna A/B loop selection exposed via the RX panel.

**MQTT antenna display names (#2881)**

MQTT-driven antenna name overrides; pairs with the existing antenna-
alias store for radio-side identification.

**Cycle TX slice shortcut (#2836)**

New `cycle_tx_slice` action assignable to FlexControl, MIDI, or
keyboard.

**Drag value popups on sliders (#2944)**

All sliders show a value popup during drag, dismissing on release.

**Audio device prompt suppression (#2926)**

Hotplug-prompt suppression controls when the current selection still
works.

**FlexControl new wheel mode WheelAgcT (#2907)**

New wheel-mode action driving the AGC threshold slider, joining the
existing volume/AF/RIT/XIT modes.

**FlexControl master volume routing (#2925)**

FlexControl volume action now routes to master volume instead of the
active slice volume — matches operator expectation across most setups.

**ATU SWR sweep respects active band plan (#2800)**

The SWR sweep now restricts its scan to band-plan segments instead of
sweeping the full radio frequency range.

### Bug fixes — audio

- **Silent SSB/voice TX on macOS** (#2930) — mic native rate vs Qt
  request mismatch caused capture stream to deliver no frames. Fixed
  by honouring the mic's native rate and explicitly starting capture
  for `remote_audio_tx` mode.
- **CW sidetone routes to selected output** (#2899) — was routed to
  the default audio output regardless of the user's selection. Fixed
  via the same `AudioEngine` sink path as RX audio.
- **Windows COM port handle release** (#3010) — FlexControl USB
  serial port stayed open across application restart, blocking
  subsequent connections. Synchronous close on shutdown releases the
  handle.
- **TX final limiter default-off** (#3004) — client TX final limiter
  was on by default, dropping AetherSDR's drive level below SmartSDR's.
  Now off-by-default to match the parity expectation.
- **RN2 (RNNoise) on Tube Pre-Amp TX** (#2813) — Channel strip Tube
  Pre-Amp section gains an RN2 toggle for client-side TX noise
  reduction in addition to the existing RX RN2.
- **BYPASS persistence** (#2892) — Channel strip BYPASS state now
  persisted across application restart via the section-active flag.
- **mute_local_audio_when_remote restored** (#1110) — behaviour
  inadvertently removed in a v0.9.x refactor.
- **Hotplug dialog suppression** (#2864) — Linux audio hotplug dialog
  spammed users when an unrelated device appeared. Suppress when the
  currently-selected device is still available.
- **RADE state restoration on engine failure** (#2898, #2861) — slice
  state fully restores when the RADE engine fails to start.
- **FreeDV mode-string coverage** (#2820) — extended `FDVU`/`FDVL`
  checks to all RADE-related code paths.
- **Default audio summary logging** (#2973) — startup logs the
  active sinks / sources / resampler so support bundles surface the
  audio path without operator instrumentation.
- **RX audio stream diagnostics** (#2889) — per-stream RX diagnostics
  exposed in the support bundle.

### Bug fixes — spectrum + waterfall

- **TX waterfall stability across slices** (#3019) — TX waterfall on
  multi-pan setups had inconsistent row cadence and dBm range jitter.
  Fixed by computing TX waterfall mask from active TX filter +
  carrier, applying mask to all overlapping pans, and freezing dBm
  range during TX.
- **Panadapter center clamp** (#783 → #2867) — 7-year-old bug:
  panadapter centre could be set below 0 Hz when zooming aggressively.
  Clamped to non-negative.
- **Off-screen slice indicator comfort margin** (#2941) — single click
  on the chevron now reveals the slice 18% inside the visible band
  rather than landing on the edge.
- **Right-side applet clipping** (#3023) — applet content was clipped
  on the right when the applet panel scrollbar was visible. Adjusted
  viewport width.
- **FDX indicator ignores radio rejection** (#3016) — FDX showed
  enabled when the radio actually rejected the command. UI now
  downstream of authoritative radio state.
- **FFT line slider on QPainter path** (#2722) — FFT line height
  slider was honoured on the GPU path but ignored on the software-
  render path.
- **Native 4m/2m band selection** (#2831) — 4m + 2m bands route to
  the radio's native band selector via `ModelCapabilities` instead of
  the legacy `model.contains()` checks.
- **TXDSP drag silent-fail** (#1836 → #3044, #3058) — composite tile
  didn't reorder via drag because its internal container id (`tx_dsp`)
  differed from its `AppletEntry.id` (`TXDSP`). First fix landed an
  aliasing fallback in `dropEvent` (#3044); cleanup PR introduced
  `ContainerWidget::setDragId()` so the composite explicitly sets the
  drag identity, eliminating the fallback (#3058).
- **XVTR validity guard for waterfall remapping** (#2853) —
  regression fix from earlier waterfall work.

### Bug fixes — CAT, protocol, MIDI

- **TCI use-after-free in queued lambdas** (#2814) — `TciProtocol`
  could be destroyed while queued lambdas held a raw pointer.
  Captured by `QPointer`.
- **RigctlProtocol use-after-free in queued lambdas** (#2995) — same
  pattern, different file.
- **set_split_vfo TX slice ping-pong** (#2931) — rigctld
  `set_split_vfo` poll could ping-pong TX between slices.
- **rigctld cwx routing through CwxModel** (#2909) — `cwx send`/clear
  now produce CAT sidetone consistent with manual keyer input.
- **MIDI VFO encoder direction + step size** (#2993) — binary VFO
  encoder direction was inverted; step size now reads from the
  encoder's actual config, not the slice's default.
- **MIDI tx.mox uses isTransmitting** (#2859) — toggle now reads
  `isTransmitting()` not `isMox()`, avoiding feedback loops on QSK or
  hardware-keyed PTT.
- **TCI emit vfo after band-change slice recreate** (#2828) — TCI
  clients weren't notified of the new VFO frequency after a band-
  change forced slice recreation.
- **HID IcomRC28Parser byte offsets** (#1896) — 7-year-old bug:
  parser had wrong byte offsets and report-size assumption.
- **NRS level re-push after profile global recall** (#2849) — slice
  state cache wasn't replayed for radio-default-on settings.
- **Slice state cached and replayed for profile recall** (#2917) —
  re-pushes cached NRS / NB / etc. after a profile global recall.

### Bug fixes — UI & UX

- **Single-click discrimination interval setting** (#3009) — exposed
  the single-vs-double-click discrimination interval as a user
  setting; old hardcoded 230 ms was too short for slower-tap operators.
- **Slice-mute UI consolidation** (#3006) — single-click mutes this
  slice; double-click mutes all slices.
- **WheelMasterAf consolidated into WheelVolume** (#2986) — two
  near-identical FlexControl wheel modes folded.
- **Radio Setup microphone control alignment** (#3008) — visual
  cleanup of the mic config row.
- **GuardedSlider type tightening + EQ formatter extraction** (#2987)
- **About dialog: hoist AETHER_GIT_SHA fallback + CMake clarity**
  (#2991, #2988)
- **What's New dialog refactor** (#2979) + WhatsNew generator cleanup
  (#2992)
- **Background image dialog parented to top-level** (#2865)
- **VFO `QStackedWidget` cross-tab height inflation** (#2821)
- **Suppress routine connect notices during startup** (#2852)
- **Suppress interlock error dialogs** (#2851) — these were noise
  during normal TX handovers.
- **WaveApplet drawer collapsed state persistence** (#2827)
- **Applet panel float / dock title-bar icon sync** (#2896)
- **Antenna SWR sweep respects active band plan** (#2800)
- **Drag value popups during slider drag** (#2944)
- **Clipboard copy controls in Radio Setup details** (#2976)
- **Native 4m/2m band selection routing** (#2831)
- **Sync RX antenna status to panadapter** (#2862) + **sync WNB
  status from radio** (#2860)

### Bug fixes — security

- **PII log redactor regex tightening** (#2954)
- **Read-buffer caps on radio/WAN/DxCluster sockets** (#2955)
- **Callsign sanitisation in WAV recording paths** (#2956) —
  callsigns containing `/` (e.g. `/M`, `/P`, `/MM`) could escape the
  WAV recording directory.
- **Atomic-rename log symlink** (#2957) — was remove + symlink, brief
  window where the symlink didn't exist.

### Refactor & internals

- **LOCKED tune-feedback timer centralization** (#2983 → #3017) —
  per-site timers consolidated into `SliceModel`.
- **Band-plan contiguous-regions walker** (#2822 → #3018) — single
  canonical walker for ATU pre-tune + SWR sweep.
- **Canonical Biquad utility** (#3042 → #3045) — new
  `src/core/Biquad.{h,cpp}` + `StereoBiquad` with 9 filter types
  (LowPass / HighPass / BandPassConstQ / BandPassPeak / Notch /
  AllPass / PeakingEq / LowShelf / HighShelf), DF-II Transposed
  topology, double-coeffs + float-state precision split.
  Migration of the 5+ scattered biquad implementations
  (DESS / Pudu / PhaseRotator / Eq) to follow in subsequent PRs.
- **`ContainerWidget::dragId`** (#3057 → #3058) — optional drag-
  identity property separates "drag MIME identity" from "container
  persistence identity". Composite tiles (TXDSP) can set `dragId`
  without renaming their persisted `m_id`.
- **`RadioModel::m_pendingPanStatuses` TTL** (#2228 → #3037) — 30 s
  TTL sweep on insert bounds the deferred-pan-status map for
  never-claimed pans.
- **AetherSweep decouple from `m_overlayMenu`** (#2205 → #3035) —
  `leftOccludedRect()` helper abstracts overlay-menu geometry queries.
- **Comfort margin ternary cleanup** (#3033 → #3039) — collapsed
  redundant branches after #3030 made the `revealOffscreen` and
  default arms return identical values.
- **Dead FFT accumulator state cleanup** (#3038 → #3040) — removed
  five unused members after #3031's debt-gate removal.
- **CODEOWNERS framework + teams** (#3053, #3055) — three-tier
  CODEOWNERS with team mentions (`@aethersdr/maintainers`,
  `@aethersdr/infrastructure`, `@aethersdr/reviewers`) replacing
  per-user ownership. Tier 1 = governance/security/bot policy,
  Tier 2 = infrastructure, Tier 3 = source code.
- **liquid-dsp vendored** (#3043 → #3046) — comprehensive DSP toolkit
  (modems / FEC / adaptive filters / AGC / NCO) vendored at
  `third_party/liquid-dsp/`. Built static, linked into AetherSDR on
  Linux + macOS. MSVC build skipped due to C99 `_Complex` syntax
  incompatibility (tracked separately as #3049).

### Diagnostics & support bundles

- **TX route diagnostics in support bundle** (#2885 → #2919) —
  support-bundle `tx_route` section surfaces TX slice id / mode /
  DAX channel.
- **About dialog git short SHA** (#2988) — surfaces git short SHA
  under the version line.
- **What's New dialog refactor** (#2979) — UX overhaul of the
  release-notes browser.

### Build & CI

- **`check-windows` path filter for MainWindow.{cpp,h}** (#2671 →
  #3028) — file is densely guarded with `#ifdef Q_OS_WIN` and was
  previously the source of most MSVC-only regressions. Triggers
  Windows CI on changes to it now.
- **Weekly ASan / UBSan / TSan workflow** (#2866) — weekly sanitizer
  build with sticky-issue tracking.
- **Pin all third-party action references by commit SHA** (#2920)
- **Auto-pin docker image digest** (#2918) — ci.yml and codeql.yml.
- **Least-privilege `GITHUB_TOKEN` scopes** (#2916) — ci.yml and
  codeql.yml drop default token scopes; jobs that need more opt in
  explicitly.
- **CODEOWNERS @jensenpat scope expansion** (#2915) — landed
  pre-#3053 framework refactor.
- Multiple dependabot bumps (#2996–#3000, #2922) — docker actions,
  ccache-action, sccache-action, github-script, ws npm dep in the
  Stream Deck plugin.

### Documentation

- **Restructure `docs/`** (#2963) — split into `architecture/`,
  `style/`, `qa/` subdirectories + `tests/README`. No content lost,
  just structure.
- **Community-response style guide** (#2966) — added to
  `.specify/memory/` for AetherClaude orchestrator + maintainer use.

---

## [v26.5.2.1] — 2026-05-17

### Hotfix — TCI TX audio level, Windows process-hang, SSA Sweden band plan, AppImage build

Three bug fixes plus one community contribution shipped within a day of
v26.5.2.  Recommended upgrade for any operator using TCI digital modes
(WSJT-X / JTDX), Windows users with MQTT or floating-panel windows, and
Linux operators building from source on Ubuntu 22.04.

### Bug fixes

**TCI TX audio level regression vs v26.5.1 (#2806)**
- v26.5.2's TCI server identity (`device:SunSDR2DX` + `protocol:ExpertSDR2,1.9`,
  added in #2597 for RF2K-S amplifier whitelist compatibility) hit a
  specific code path in WSJT-X's `TCITransceiver`
  (`TCITransceiver.cpp:823` + `.hpp:188-200`) that halves outgoing TX
  sample amplitude (K2 = 0.499 / 0x7FFF vs K1 = 0.999 / 0x7FFF, ~-6 dB
  at the source) when **both** of these match:
  - `device:` is `"SunSDR2DX"` or `"SunSDR2PRO"`
  - `protocol:` is not `"ExpertSDR3"`
- Operators measured ~70 W of 100 W expected on a 100 W slice.
- Fix changes identity to `device:AetherSDR` + `protocol:ExpertSDR3,1.5`,
  which clears **both** triggers (belt-and-suspenders) and selects
  WSJT-X command formats that AetherSDR has supported correctly since
  v26.5.1.  Side benefit: the literal `device:AetherSDR` avoids the
  leading-space parser trip the older `<name> <model>` form caused
  when a radio nickname was empty.
- Everything else from #2597 (strict init-burst order, `vfo_limits`,
  `if_limits`, `channels_count`, `split_enable`, post-READY audio
  stream configuration) is preserved — that's the bulk of what the
  RF2K-S TCI parser actually keys on to engage.  Configurable /
  per-client adaptive identity to restore the SunSDR2DX whitelist
  half is tracked in #2806.

**Windows process lingering in Task Manager after close (#2802, chibondking)**
- Three independent root causes ganged up to keep `AetherSDR.exe`
  visible in Task Manager indefinitely after the main window closed.
  Each ships a minimal one-line fix matching an existing precedent:
  - `m_aetherialStrip` carries `Qt::Window` for an independent taskbar
    entry; Qt 6 treats that as top-level for `quitOnLastWindowClosed`,
    and the default `WA_QuitOnClose = true` blocked the quit signal.
    Now sets `WA_QuitOnClose = false` immediately after the window-flag
    is applied.
  - `m_appletPanelFloatWindow` is constructed `parent = nullptr` (a
    real top-level) and was the only top-level secondary window in the
    codebase missing `WA_QuitOnClose = false`.  `FloatingContainerWindow`
    has set the same attribute since the frameless popout work.
  - `MqttClient::~MqttClient` called `mosquitto_destroy()` on Windows
    without ever stopping the loop thread (the existing `#ifndef Q_OS_WIN`
    guard in `disconnect()` skipped `loop_stop` to avoid a deadlock).
    libmosquitto's contract requires `loop_stop` before `destroy`; the
    destructor now calls `mosquitto_loop_stop(m_mosq, true)` (`force=true`,
    can't deadlock) — the same pattern `connectToBroker()` already used.
- Result: process exits within ~1 s of main-window close, every time.

**Linux AppImage build failure on gcc 11 strict mode (#2799)**
- `third_party/libmodem_core/bitstream.h:192` used unqualified `container`
  inside the `noexcept(noexcept(...))` clause of a class template member.
  gcc 11.4 (Ubuntu 22.04 native runner used by the AppImage workflow)
  requires `this->container` for two-phase name lookup of class-template
  member access from inside a function-signature noexcept clause.  Newer
  gcc (Docker CI image) was lenient; AppImage CI bombed on the v26.5.2
  tag push.
- Fix qualifies both references with `this->` — pure portability,
  zero user-visible change.  Restored the v26.5.2 AppImage x86_64
  artifact via `workflow_dispatch` after the fix landed.

### New region data

**SSA (Sweden) band plan (#2805, NF0T)**
- Adds `resources/bandplans/ssa-sweden.json` — Swedish national overlay
  on IARU Region 1 with the correct 6 m band edge (50–52 MHz, not the
  IARU R1 file's 50–54 MHz), embedded power-limit suffixes on bands
  where PTSFS 2025:1 imposes substantially lower caps (60 m 15 W e.i.r.p.,
  30 m 150 W p.e.p.), Swedish segment labels (ALLA, FYRAR, SMAL DIGI),
  and finer 6 m segmentation matching SSA's published v3.0 plan
  (separate beacon-only 50.400–50.500 segment, FM/DV repeater input
  51.210–51.390, output 51.810–51.990).  89 Swedish-language activity-
  center / digital / beacon / emergency spots.
- Auto-loaded by `BandPlanManager::loadPlans()`, surfaced in
  View → Band Plan submenu.  Principle IV — no hardcoded Swedish edges
  in C++.

### Contributors

Thanks to **@chibondking** for the Windows process-hang root-cause
analysis and triple-fix (#2802); **@NF0T** for the SSA Sweden band plan
on top of an already-busy week of FreeDV/RADE work (#2805); and
**@jensenpat** for the third_party libmodem_core code that landed in
#2753 (and whose portability tweak is in this hotfix).

## [v26.5.2] — 2026-05-17

### Six days of community momentum — AetherModem, FreeDV ergonomics, FLEX-6700 4m/2m, dialog architecture sweep

The first natural-cadence release after the 1.0-equivalent. **123 commits in
six days**, driven by an unusually active week from community contributors and
the AetherClaude orchestrator. Highlights:

- **AetherModem** — Phase-0 bring-up of a native AX.25 packet decoder
  (`jensenpat`), with 21-lane phase-bank HDLC framing, FCS validation, and a
  new Packet Decode dialog. RX is active on 300 baud HF and 1200 baud VHF;
  TX is live on 300 baud HF with timing refinements queued for Phase 1.
- **FreeDV/RADE ergonomics** — sync/SNR indicator in the VFO widget for FDVU/FDVL
  slices, far-end callsign and frequency-offset row, end-of-over (EOO) callsign
  decode + FreeDV Reporter posting (`NF0T`).
- **FLEX-6500 / FLEX-6700 4m/2m bands** — surfaced via `ModelCapabilities`
  using FlexLib `ModelInfo.cs` as the authority; works with the radio's
  internal XVTR or any external transverter.
- **PersistentDialog architecture sweep** — new base class + showOrRaisePersistent
  pattern; 10+ dialogs migrated (Profile Manager, Profile Import/Export, Memory,
  Network Diagnostics, PropDashboard, TxBand, Waveforms, RadioSetup, DxCluster,
  AetherDsp, AX.25 Packet Decoder, MIDI, MQTT, SpotHub). Frameless toggle now
  propagates correctly; geometry persists; no duplicate-window classes of bug.
- **Spectrum & panadapter stabilization** (`rfoust`) — panadapter restore-on-
  reconnect across multi-pan and floating-pan setups, macOS GPU lifecycle
  cleanup, dBm-range echo smoothing (no snap-back after release), Ctrl-drag
  dBm zoom, Ctrl-drag waterfall rate.
- **Protocol hardening** — `M`-message severity parsing (no more Info notices
  popping modals), hex meter num field handling, `tx_slice_mode` from transmit
  status, FlexWaveformModel for firmware v4.2.18 WFP status.

Huge thanks to **@jensenpat** (AetherModem Phase-0, AX.25 timing, AX.25 receive
improvements, FFT floor lock edge cases, XVTR waterfall offset, RF Gain / AGC-T
shortcuts, spot auto-mode routing), **@NF0T / Ryan** (FreeDV sync indicator,
Waveforms dialog, FlexWaveformModel protocol, hex meter parser, CWX context
menu + CharGap, RADE auto-deactivate, tx_slice_mode, RX-applet radeActivated
restore), **@rfoust / Robbie** (panadapter persistence + macOS GPU lifecycle,
dBm Ctrl-zoom, ctrl-drag waterfall rate, TNF status tooltip, panadapter
restore-window extension, spectrum dBm echo smoothing, FFT floor edge cases),
**@M7HNF-Ian** (spot tooltips near waterfall edge, SpotHub auto-reconnect),
**@algojogacor** (spot label gap), and **@aethersdr-agent** (AetherClaude — 30+
mechanical fixes including the entire PersistentDialog migration sweep, persistence
fixes, log rotation, naming polish, AsyncLogWriter + PerfTelemetry test harnesses,
NoiseFloor diagnostics).

### New features

**AetherModem — AX.25 packet decoder (Phase 0, #2753, jensenpat)**
- Native HDLC/AX.25 packet decoder integrated into the slice signal path.
  21-lane phase-bank receiver with FCS validation. New `Packet Decoder`
  dialog accessible from the slice context menu; tagged as Experimental
  via banner (#2764) until 1200 baud VHF reaches feature parity with HF.
- Receive timing improved in follow-on PR #2788; em-dash render fix on
  the experimental banner (#2791).

**AetherModem Phase-0 indicator on Packet Decoder dialog (#2766)**
- Surfaces the Experimental status with a dedicated banner so operators
  understand TX coverage (HF 300 baud only) vs RX coverage (HF 300 +
  VHF 1200).

**FreeDV/RADE sync + SNR indicator in VFO widget (#2776, NF0T)**
- New VFO widget chrome on FDVU/FDVL slices: shows sync state and
  decoded SNR alongside the standard frequency / mode / filter display.

**RADE info row — far-end callsign, SNR, freq offset (#2660)**
- Per-slice RADE info row exposing the far-end's callsign, decoded SNR,
  and frequency offset for live awareness during a QSO.

**RADE EOO callsign decode + FreeDV Reporter posting (#2659)**
- Decodes the end-of-over callsign from the far end and posts it to
  FreeDV Reporter for spotting / heard-list integration.

**FlexWaveformModel for firmware v4.2.18 waveform status (#2759, NF0T)**
- New `FlexWaveformModel` parses `waveform` status objects from firmware
  v4.2.18 and surfaces them in a dedicated Waveforms dialog (#2779) under
  File menu for WFP status and per-waveform management.

**4m / 2m bands on FLEX-6500 and FLEX-6700 (#2757)**
- Surfaces 4 m and 2 m band buttons on FLEX-6500 / FLEX-6700 via
  `ModelCapabilities`. Reads the per-model XVTR allocation from FlexLib
  `ModelInfo.cs` (authoritative source) and routes click-to-band through
  the existing XVTR slice creation path.

**Smart Spot Filtering — dim unmatched DX voice spots (#2555)**
- New SpotHub filter that dims/hides DX voice spots that don't match a
  user-defined pattern (country/zone/mode). Companion to the existing
  callsign-pattern filter; user-configurable match window in #2699.

**Auto-squelch with 3-way SQL cycle (#2544)**
- New tri-state squelch toggle: Off → SQL (manual) → Auto (tracks the
  noise floor). Cycles via the SQL button or the keyboard shortcut.

**Local antenna display names (#2620)**
- Per-radio antenna labels (e.g. "ANT1 → 80m Dipole") stored
  client-side and applied across slice context menus, RX applet, and
  TX band dialog. Radio still uses ANT1/ANT2/RX-A for the wire protocol.

**ATU band pre-tune sweep + clear memories (#2630)**
- New ATU action: sweep the active band in 20 kHz steps and pre-tune
  every segment, building a band-wide ATU memory table in one pass.
  Plus a per-band Clear Memories action.

**License confirmation modal for Antenna SWR Sweep (#2691)**
- Adds a one-time license-acknowledgement modal before allowing
  Antenna SWR Sweep — clarifies that the sweep transmits.

**Numeric entry for Aetherial Audio effect parameters (#2697)**
- All Aetherial Audio Channel Strip knobs now support direct numeric
  entry alongside drag-to-set.

**FlexRadio profile import/export via .ssdr_cfg (#2641)**
- Import and export TX / Mic / Global profiles in FlexRadio's
  `.ssdr_cfg` wire format. Round-trip compatible with SmartSDR; lets
  operators migrate profile libraries from Windows to Linux/macOS.

**FlexControl CWX F1-F12 macro actions (#2725)**
- The FlexControl USB knob can now trigger CWX (and DVK) F-key macros
  via button mappings.

**Independent RX/TX CW decode toggles (#2638)**
- RX-side and TX-side CW decode can be enabled independently; previously
  toggled together.

**Auto-Save inline when TX/Mic profile name collides (#2790, #2712)**
- The profile-save warning dialog now offers Auto-Save as an inline
  option rather than requiring a separate menu trip.

**Persist TX Tune Mode + EQ applet RX/TX view (#2735, #2736)**
- Tune Mode (single-tone vs two-tone) and the EQ applet's RX/TX
  selection persist across launches. Note: TX Tune Mode persistence
  was subsequently reverted to a right-click menu in #2787 to avoid
  the connect-time re-apply trap; the right-click stays.

**Two-tone TUNE selector via right-click (#2787)**
- TUNE button right-click exposes Mono Tone / Two Tone selection;
  cleaner than the persisted-setting form.

**TNF status tooltip (#2789)**
- Hovering a TNF marker now shows depth/permanent state inline.

**Audio device hotplug + notification dialog (#2583)**
- Detects audio device add/remove events from the OS and routes the
  current PC audio path through the change with a notification dialog
  so the user can choose to follow the new default or stay.

**PEP peak-hold tick on TX FWDPWR gauge (#2596)**
- Adds the canonical PEP peak-hold tick (with configurable hold time)
  to the FWDPWR meter, matching SmartSDR's PEP display semantics.

**Profile Manager non-modal + persist geometry (#2591)**
- Profile Manager dialog migrated to the PersistentDialog base class —
  non-modal, geometry persists, frameless chrome support.

**Ctrl-drag waterfall rate control (#2783)**
- Holding Ctrl while dragging the waterfall vertically adjusts the
  scroll rate (50 ms – 500 ms per line).

**Ctrl-drag dBm scale zoom (#2717, #2726)**
- Holding Ctrl while dragging the dBm scale zooms vertical range
  symmetrically; tooltip and help docs (#2726) document the gesture.

**Pan-follow-VFO triggers off flag outer edge (#2784, #2761)**
- When a VFO flag approaches the panadapter edge, pan-follow now
  triggers off the outer edge of the flag rather than the center —
  no more cropped flags when the user pans to track a signal.

**Spectrum overlay Display panel reorder + SQL Margin → RX Applet (#2695)**
- Display panel controls reordered for clarity; SQL Margin moved from
  the spectrum overlay to the RX applet where it belongs.

**Audio Device Detected dialog gets frameless chrome (#2618)**
- Standard 18 px gradient title bar + chrome treatment, matching the
  rest of the app.

**Floor: auto-adjust — asymmetric smoothing + transient rejection (#2653)**
- The Floor: auto-adjust algorithm now uses asymmetric smoothing
  (fast attack on rising floor, slow release on falling) and rejects
  transient peaks. Replaces the old zoom-based response with a pan-only
  response so signal heights stay visually stable as the floor moves.

**Radio-letter slice display mode for Multi-Flex (#2606, #2708)**
- New display mode shows radio-side slice identifier (A/B/C/D) instead
  of client-local letter for Multi-Flex sessions where multiple clients
  may have different letter assignments.

**Network audio packet-loss concealment (#2732)**
- PLC algorithm fills brief network audio dropouts (1–3 packets) with
  spectrally-matched filler instead of silence; eliminates audible
  pops on lossy Wi-Fi.

**Lock split-pair VFO panels to opposite sides (#2744, #2663)**
- Split-pair VFOs (A/B in split mode) now lock to opposite sides of
  the dual-VFO layout to prevent operator confusion.

**Right-click context menu on CWX history bubbles (#2752)**
- CWX history bubbles get a right-click context menu for delete /
  resend / edit actions.

**Cluster overflow popup emits spotTriggered (#2741)**
- Clicking a spot inside the cluster overflow popup now triggers the
  full `spotTriggered` flow (slice retune + spot record), not just
  the bare frequency move.

**RF Gain and AGC-T keyboard shortcuts (#2710, jensenpat)**
- Adds `rf_gain_up/down` and `agc_t_up/down` shortcut actions for
  keyboard and FlexControl mapping.

**Interlock notification system (#2586)**
- Per-band interlock state changes (band-cancel, amp-warmup, ATU-busy)
  now route through a unified notification system rather than ad-hoc
  modal popups.

**Smart Spot Filter match window (#2699)**
- User-configurable time window for the Smart Spot Filter — controls
  how long a matched spot's marker remains highlighted.

### Bug fixes

**Spectrum dBm range echo smoothing (#2793, rfoust)**
- After releasing a dBm scale drag, in-flight FFT frames encoded for
  the old range no longer snap the trace back. Rebases up to 10 frames
  with a 0.75 dB median-improvement guard. Also splits auto-floor echo
  handling from manual drag echo so the smooth animation no longer
  steps when the radio is slow to ack.

**Panadapter layout / floating persistence + macOS GPU lifecycle (#2786, #2780, rfoust)**
- Panadapter layout state restores cleanly on radio reconnect across
  multi-pan and floating-pan window configurations. Bundles the macOS
  QRhi GPU lifecycle fix (reparenting on dock/float and clean shutdown).

**Extend panadapter startup layout restore window (#2792, rfoust)**
- Restore window widened from 5 s to 30 s for slow reconnects, with a
  user-intent suppress flag so layout-button presses during the wider
  window don't get clobbered.

**Spectrum FFT line width slider produces visible pixel-width line (#2706)**
- Slider value previously produced sub-pixel-width strokes on some
  scale factors. Now floor-rounded to an integer pixel width before
  draw.

**M-message severity respected so Info notices don't pop modals (#2785)**
- The radio's M-message severity bits (Info / Notice / Warning / Error)
  were previously ignored; Info-level messages popped error modals.
  Now routed to the status bar for Info/Notice and the modal only for
  Warning/Error.

**Parse hex meter num field + handle M-prefix radio messages (#2771, NF0T)**
- Meter-number field was decimal-only, breaking on radios that send
  hex. Plus the `M`-prefix radio message form is now parsed correctly.

**Panadapter vanished panstack on left-dock startup (#2733, #2704)**
- Empty panstack on first launch when the applet panel was left-docked.
  Two related size-setter bugs (#2746 follow-up) addressed.

**XVTR waterfall offset matching (#2709, jensenpat)**
- Waterfall display offset for XVTR slices now matches the spectrum
  offset on unvalidated profile entries.

**Spot auto-mode slice routing (#2713, jensenpat)**
- Auto-mode spots now route to the correct slice based on
  mode-compatibility rather than always-slice-0.

**Spot tooltips disappearing near waterfall edge (#2740, M7HNF-Ian)**
- Tooltips clipped past the waterfall edge are now anchored above the
  cursor.

**FFT floor lock edge cases (#2715, rfoust)**
- Edge-case crashes in the FFT-floor-lock state machine resolved.

**XVTR direct-entry tuning above 450 MHz (#2621)**
- Direct frequency entry on XVTR slices accepted values above 450 MHz
  in display but the slice tune command rejected. Now passes through.

**Spot tooltip cursor offset + re-anchor suppression (#2631, #2654)**
- DX / band-plan tooltips offset from the cursor to avoid covering
  the spot label; suppresses re-anchor flicker during slow mouse moves.

**Spot label gap in spectrum (#2702, algojogacor)**
- Inter-label spacing now enforced so adjacent spot labels don't
  overlap.

**TX/Mic Save button silently no-ops on existing names (#2707, #2637)**
- Profile Save with an existing name now shows a confirm dialog
  instead of failing silently.

**Layout: second size-setter also clobbered left-dock startup (#2746)**
- Follow-up to #2733; a second size-setter path was also clobbering
  the left-dock layout.

**Restore WAVE TX scope for digital audio (#2688)**
- Digital-mode TX (DIGU/DIGL/FT8/RTTY) now restores the WAVE-TX scope
  selection on reconnect.

**RADE auto-deactivate when slice mode changes externally (#2747, NF0T)**
- When the radio (or another client) changes a RADE slice to a non-
  RADE mode, RADE now auto-deactivates client-side.

**Restore unconditional radeActivated(false) emit on RX applet (#2745, NF0T)**
- RX applet's RADE state emit was elided in some teardown paths,
  leaving downstream consumers stale.

**CharGap between live-mode Pending entries in CwxLocalKeyer (#2754, NF0T)**
- Successive Pending entries in the CWX live-mode buffer were
  concatenated without the inter-character gap.

**Suppress amp/tuner popup during normal TX (#2678)**
- Interlock notification popup was firing during normal TX engagement.
  Now gated on `tx_allowed` to fire only when the interlock actually
  blocks TX.

**ATU walk segment list per discrete-channel band (#2686)**
- ATU sweep was sampling continuous-band centers on discrete-channel
  bands (60 m). Now walks the segment list instead.

**wasVisible-guard on remaining setGeometry sites (#2685, #2635)**
- Four (plus three) more frameless-toggle setGeometry sites adopted
  the wasVisible-guard pattern to avoid first-show off-screen positions.

**SQL threshold line auto-hide restored (#2684)**
- Manual SQL threshold line auto-hides after 3 s of inactivity again.

**Waterfall pace TX/FFT-fallback rows to radio line_duration (#2667)**
- During TX or FFT-fallback, the waterfall now paces row insertion to
  the radio's reported `line_duration` instead of free-running.

**Spot dedup cluster echo of manually-posted spot (#2661)**
- A manually-posted spot is no longer re-displayed when the cluster
  echoes it back.

**Profile Manager 'next tab' wording (#2729)**
- Wording clarified from ambiguous "next" to specific tab name.

**TX timeout display in seconds, not milliseconds (#2642)**
- TX timeout indicator was showing milliseconds with a "s" unit.

**Tear down float window when dock-side icon clicked (#2673)**
- Re-docking a float window via the dock-side icon now tears down the
  float window cleanly.

**CW Int16 fallback for sidetone sink on Int16-only WASAPI devices (#2668)**
- CW sidetone now falls back to Int16 on WASAPI devices that don't
  support float32.

**Don't toggle radio dax flag on Windows from slice-mode changes (#2670, #2315)**
- Windows slice-mode-change side-effect that toggled the radio's
  DAX flag eliminated.

**Guard m_daxBridge ref in deactivateRADE() (#2662)**
- Null-guards added to the RADE deactivation path for Windows / no-
  PipeWire builds.

**RADE DAX stream lifecycle on macOS (#2633)**
- RADE DAX stream lifecycle on macOS now matches Linux/Windows
  behavior.

**macOS Bluetooth headset mic capture rate (#2615)**
- macOS Bluetooth headset mic capture now correctly negotiates the
  supported rate (was hardcoded to 48 kHz, breaking 16 kHz BT codecs).

**S-History voice signal carrier detection (#2549)**
- Voice signal carrier frequency detection improved — handles slightly
  off-tune voice signals.

**Dx-cluster strip non-printable control chars (#2700)**
- Cluster console lines now have control characters stripped before
  display.

**TCI spec/impl compliance + RF2K-S amplifier interop (#2597)**
- Several TCI v2.0 command spec divergences corrected; RF2K-S
  amplifier now interoperates correctly via TCI.

**Prevent remote_audio_rx stream on TCI autostart if PC audio disabled (#2557, #1137)**
- TCI autostart no longer creates a remote_audio_rx stream when PC
  audio is disabled in settings.

**Make set_split_vfo / tx_enable idempotent to stop radio TX watchdog (#2568)**
- These two CAT commands now idempotent — repeated state-equal calls
  no longer trip the radio's TX watchdog.

**Drive CWX/DVK F1-F12 shortcuts by active slice mode (#2590, #2582)**
- F-key shortcut routing now respects the active slice's mode.

**Frameless title bar drag on macOS (#2576)**
- macOS frameless title bar drag now uses the native window-drag API
  instead of synthetic mouse events.

**FPS meter overlay redraw stutter (#2578)**
- FPS meter overlay was redrawing every frame; now diff-gated.

**HID automoc source registration (#2577)**
- HID source files now correctly registered with automoc.

**macOS iconset 512@2x size (#2579)**
- macOS iconset 512@2x size corrected.

**TX mic stereo channel canonicalization (#2572)**
- Stereo mic channel order canonicalized to L=mic / R=mic across
  all platforms.

**Streamdeck tune-up/down step (#2714, #2409)**
- Streamdeck tune step bumped from 100 Hz to 1 kHz (matches main app
  step semantics).

**Consolidate duplicate aether.ax25 Q_LOGGING_CATEGORY (#2770, #2763)**
- The `aether.ax25` logging category was declared in two TUs; consolidated
  to a single declaration in `LogManager.cpp` with shared header access.

**dBm-scale Ctrl-drag zoom tooltip + help docs (#2726)**
- Companion to #2717; documents the gesture in tooltip + help.

**Log diagnostic when noise-floor capture bails (#2727, #2720)**
- New diagnostic log line when the noise-floor capture path returns
  early without producing a sample.

**ANT-tab slider default Qt border (#2751)**
- Restores the default Qt slider border on the ANT-tab sliders.

### Infrastructure

**PersistentDialog base class + showOrRaisePersistent migration sweep (#2644)**
- New `PersistentDialog` base class providing frameless-chrome support,
  geometry persistence, and lazy-construct/raise lifecycle via
  `showOrRaisePersistent<T>()`. Migrated dialogs include Profile Manager,
  Profile Import/Export, Memory, Network Diagnostics, PropDashboard,
  TxBand, Waveforms, RadioSetup (primary + 3 secondary entries), DxCluster,
  AetherDsp, AX.25 Packet Decoder, MIDI, MQTT, SpotHub. The migration
  closes a long list of legacy duplicate-window and frameless-toggle bugs.

**Migrate three secondary RadioSetupDialog entry points (#2795, #2781)**
- FlexControl, USB Cables, and XVTR overlay entry points now use
  `showOrRaisePersistent` and converge on the single tracked instance.
  Closes the duplicate-RadioSetup-window class of bug.

**Migrate seven dialogs to PersistentDialog (#2676)**
- Bulk migration of Memory, NetworkDiagnostics, AetherDsp, MIDI, MQTT,
  SpotHub, and AX.25 Packet Decoder dialogs to the new base class.

**Migrate PropDashboardDialog to PersistentDialog (#2775)**
- Companion to #2676.

**Extract TxBandDialog to its own class + migrate to PersistentDialog (#2774)**
- TxBandDialog extracted from MainWindow.cpp inline construction into
  its own class file + base-class migration.

**Log rotation + startup retention cap (#2765, #2498)**
- AsyncLogWriter now rotates the log file on size threshold and caps
  retained log files on startup.

**Unit tests for AsyncLogWriter (#2760, #2497)**
- New test harness for AsyncLogWriter rotation, buffering, and
  shutdown behavior.

**Unit tests for PerfTelemetry (#2782, #2500)**
- New test harness covering frame-time histogram, heartbeat capture,
  and aggregation.

**Lock IARU R1 reference table for computeCenters() (#2687, #2648)**
- New test guards the IARU R1 reference table used for ATU center
  computation.

**Make VirtualAudioBridge thread-safe for DAX RX DirectConnection (#2762, #2486)**
- Thread-safe accessors added to VirtualAudioBridge for the DAX RX
  DirectConnection path.

**CI post-merge guard + drop stale-branch pre-merge enforcement (#2749)**
- Adds a post-merge job that catches stale-snapshot reverts; removes
  the brittle pre-merge stale-branch check that was producing too many
  false positives.

**Refresh in-repo URLs to aethersdr/AetherSDR org (#2723)**
- Sweep across docs, CI workflows, and source comments to update the
  GitHub org reference following the org move from `ten9876` →
  `aethersdr` (2026-05-16).

**Bundle zlib 1.3.1 under third_party/ (#2698, #2651)**
- zlib now vendored to remove the system-dep variation across distros.

**Add spec-kit constitution at .specify/memory/constitution.md (#2636)**
- Establishes the project's eight inviolable principles (FlexLib
  authority, MeterSmoother canonicality, UI-label-driven naming,
  region-aware band data, nested-JSON config, CHAIN-widget TX DSP
  entry, auto-generated Contributors list) as the authoritative
  reference for AI-agent contributions.

**Frameless guidance for popout dialogs in CLAUDE.md (#2619)**
- Documents the canonical frameless pattern for popout dialogs.

**Add frameless support for popout dialogs (#2580)**
- Foundational frameless chrome support for popout / floating
  dialogs (sets up the PersistentDialog work that followed).

**Document _tci._tcp.local mDNS discovery schema (#2613, #2502)**
- TCI mDNS discovery schema documented.

**Document AetherSDR audio pipeline (#2571)**
- New docs page covering the full audio pipeline (TX mic path, RX
  audio path, DAX, RADE, TCI, AetherModem).

**Drop dead SHistorySoftEdgeDb settings key (#2608, #2607)**
- Removes an unused settings key + saves a migration step.

**Add PC Audio device tooltip (#2587)**
- The PC Audio panel now shows a tooltip with the underlying device
  identifier on hover.

**Remove dead startSystemMove branch in ContainerWidget (#2645)**
- Dead-code cleanup.

**Remove dead drawFpsMeters() painter path (#2632, #2602)**
- Replaced by the overlay-based FPS meter from #2480.

**Add aethersdr-wallpaper.png to docs**
- Project wallpaper for marketing / community.

## [v26.5.1] — 2026-05-10

### 1.0 — first stable release; CalVer cutover

The 1.0-equivalent. After eight 0.9.x cycles the client now covers
every documented SmartSDR feature on the FLEX-6000, FLEX-8000, and
Aurora platforms, and ships the Aetherial Audio Channel Strip's RX and
TX paths in full. Confidence in stability across Linux / macOS /
Windows is high enough to drop the pre-1.0 framing.

Coincident with the milestone, the project switches from semver to
**CalVer** (`YY.M.patch`). Rationale: AetherSDR's release cadence is
driven by FlexLib protocol changes, ham contest seasons, and a steady
trickle of community fixes — not by API stability commitments to
downstream consumers. CalVer communicates "this is what shipped in
month X" more honestly than a 1.x semantic version would. The previous
`v0.9.x` tags remain in git history; new tags use the `vYY.M.patch`
form starting here.

55 commits since v0.9.8; highlights below. Big thanks to **@jensenpat**
(macOS `.icns` build-time generation, FlexControl wheel modes, popout
frameless lifecycle), **@chibondking** (Windows UI scaling settings
path), **@rfoust** (DAX IQ Windows/non-PipeWire-Linux fix, RX output
panel rebuild, TCI volume/drive/rx_volume dispatch), **@aethersdr-agent**
(AetherClaude — parallel implementations on `aetherclaude-eligible`
issues, including #2550 spot background_color), **@s53zo**, and
**@M7HNF-Ian** for contributions across this cycle.

### New features

**S-History v2 — voice signal detection + QRM classification (#2426)**
- New CNN classifier on the panadapter that distinguishes confirmed
  voice signals from QRM channels in the Signal History overlay.
  Voice markers age out independently of QRM markers (30 s after the
  last voice-width hit); QRM channels with voice riding on top get
  both markers simultaneously so operators can see the interference
  AND the person trying to work through it.

**Connected Stations dialog when multiFLEX disabled (#2488)**
- Operators running with multiFLEX off can now see who else is on
  the radio via Help → Connected Stations. Lists every connected
  client with handle, program, station, and ownership state.

**FPS meters + performance telemetry (#2480)**
- Footer FPS readouts for spectrum / waterfall plus a
  `PerfTelemetry::recordUiHeartbeat()` hook that captures the Qt
  event-loop cadence at 50 ms intervals. Feeds the future System Info
  diagnostics dialog (#2554).

**8-axis edge resize for frameless main window (#2522)**
- The main window's frameless mode now supports the full eight-axis
  resize grip (N/S/E/W/NE/NW/SE/SW) matching the Aetherial Audio
  Channel Strip and other frameless dialogs.

**SpotHub Display tab + tunable Signal History (#2506)**
- Consolidates the SpotHub display controls into a single tab with
  Signal History tunables (qualify duration, hide-after, suspect-QRM
  threshold). Sub-tabs replaced with grouped sections.

**Frameless chrome on Memory Channels dialog (#2509)**
- The Memory Channels dialog gets the standard 18 px gradient title
  bar, grip glyph, drag-to-move, double-click-to-maximize, 8-axis
  resize — matching NetworkDiagnosticsDialog and the Channel Strip.

**Add Memory action moved into MemoryBrowsePanel (#2533)**
- The "Add memory" button now lives in the MemoryBrowsePanel itself,
  closer to where users are looking when they want to capture the
  current slice. The slice-letter badge variant was dropped — adding
  always targets the active slice without per-letter selection.

**Float popped-out applet panel as frameless project window (#2536)**
- Popping out the applet panel via the new title-bar icon (or
  Ctrl+Shift+S) now floats it as a frameless project window with the
  full chrome treatment: gradient title bar, drag handle, 8-axis
  resize. Three new title-bar icons (dock-left / dock-right / pop-out)
  replace the legacy status-bar `☰` toggle.

**Always-on-Top pin on popped-out applets (#2430, #2479)**
- Per-container pin on the popped-out applet header keeps the float
  above the panadapter window for instrument-on-instrument workflows.

**FlexControl WheelRit / WheelXit modes (#2452, #2455)**
- Two new FlexControl USB knob assignment modes drive RIT and XIT
  offsets directly without touching the slice frequency.

**Easter egg: Ctrl+Shift+A toggles starstruck pan-drag sound (#2534)**
- Hidden audio cue tied to pan dragging — off by default, no shortcut
  reservation, harmless when not enabled.

**Status-bar +PAN icon redrawn as a jagged FFT trace (#2537)**
- Replaces the previous generic icon with a 30-point jagged FFT
  spectrum trace at 5 distinct peaks — matches the actual spectrum
  visual idiom AetherSDR is built around.

### Bug fixes

**Linux waterfall smear after TX (#2527, fixes #2527-class reports)**
- 10–18 second post-TX vertical-stripe smear on Linux only. Root
  cause: `WaterfallBlanker` retained a TX-era "last good row" across
  the TX→RX transition; subsequent impulse-detection substitutions
  painted that row as a frozen vertical stripe until the ring rolled
  past it. Fix: clear `m_wfLastGoodRow` and reset `m_wfBlankerRingCount`
  on TX-exit; gate blanker substitution on `!m_transmitting`.

**Slice reduction crash + float/dock GPU corruption + exit crash (#2495, #2512)**
- Three Multi-Flex / multi-pan crashes consolidated. Slice reduction
  on disconnect cascade tripped a use-after-free in
  `wirePanadapter()`. Float→dock transitions on QRhiWidget panes
  corrupted the spectrum framebuffer until the next full repaint.
  Exit crash on rapid quit-while-streaming traced to a connection
  teardown ordering bug. All three guarded with lifetime checks.

**Spot `background_color` honored when override is off (#2550, #2560)**
- `SpotData::backgroundColor` was parsed from the FlexLib protocol
  and stored in the model, but dropped at the model→view boundary:
  `SpotMarker` didn't carry the field, the converter silently dropped
  it, and `drawSpotMarkers()` only drew the pill from the local
  Override Background color. Third-party spot sources
  (wave-flex-integrator, SpotCollector) encoding priority /
  worked-before via `background_color` now render correctly when
  Override Background is off.

**ALC gauge rewired to SW ALC meter; mirror across Phone + CW (#2552)**
- HWALC (RCA voltage telemetry) was driving the SmartSDR-equivalent
  ALC gauge, producing meaningless readings. Split into hwAlc /
  swAlc; the gauge now consumes the SW ALC meter (post-software-ALC
  SSB peak). Mirror identical gauge across Phone and CW panels with
  the new `HGauge::setFillFromRight` mode.

**DSP curve widgets: HarfBuzz reshape on every paint (#2546, #2556)**
- The DSP curve widgets (`ClientCompCurveWidget`,
  `ClientGateCurveWidget`, `ClientDeEssCurveWidget`) were calling
  `QPainter::drawText` for axis labels on every paint, forcing a
  full HarfBuzz reshape per frame. Cache axis labels as `QStaticText`
  with `AggressiveCaching`; ~7× drop in `shapeText` cost.

**macOS app icon missing on local builds (#2558, jensenpat)**
- `docs/AetherSDR.icns` was never committed (only generated inside
  the macos-dmg CI runner), so every local macOS dev build silently
  produced a generic-icon `.app`. Fixed by moving icns generation
  into CMake via `add_custom_command` sourced from the committed
  `docs/logo-circle.png`. No new build dependencies.

**Stream Deck mute toggle TCI command (#2519)**
- The mute action on Stream Deck mappings sent the wrong TCI command.
  Corrected to the canonical mute/unmute primitive.

**DAX IQ silently broken on Windows / non-PipeWire Linux (#2524)**
- DAX IQ streams produced no output on Windows and on Linux systems
  without PipeWire (PulseAudio-only). Root cause: the IQ stream
  setup path assumed PipeWire native and silently returned without
  initialization on other platforms. Adds the proper fallback.

**Windows ClangCL build failures (#2525)**
- ClangCL toolchain failures for NR4/specbleach and other modules
  resolved with target-specific include path adjustments.

**Memory recall radio state sync (#2526)**
- Recalling a memory entry now correctly syncs all radio state (mode,
  filter, slice settings) instead of just frequency.

**RADE mode not disabled on PTT release (#2517)**
- Mic metering during RADE TX could be interrupted if RADE auto-
  disabled on key-up. Now stays in RADE mode across PTT cycles.

**RX output panel rebuild with canonical meter ballistics (#2514)**
- Aetherial Audio Channel Strip's RX output panel rebuilt to use the
  canonical `MeterSmoother` ballistics (30 ms attack, 180 ms release
  at 120 Hz) — matches every other meter in the app.

**XVTR Flex band-stack key uses status index, not order (#2342, #2511)**
- Band-stack persistence keyed off the position-in-list of XVTR
  entries; if the radio reordered them, recall pulled the wrong
  entry. Now keys on the radio-supplied status index.

**Frameless float popout follows frameless setting (#2449)**
- Popped-out applets now respect the global "use frameless windows"
  preference — previously they were always system-decorated.

**MIDI toggle params + modeUp/Down + category filter (#2446)**
- Toggle MIDI params no longer auto-revert on note release.
  Adds `modeUp` / `modeDown` actions. Category filter in MIDI Learn
  expanded to cover every action group.

**RTTY squelch auto-disable (#2504, #2510)**
- Switching to RTTY mode now auto-disables squelch (squelch on RTTY
  notches characters out and breaks decoding).

**NR4 button disabled when libspecbleach unavailable (#2484, #2508)**
- Builds without `libspecbleach` left the NR4 button live but
  no-op — confusing operators about what NR4 actually does. Button
  is now disabled with a tooltip when the dependency is missing.

**CWX release TX when queue drains (#2450, #2507)**
- CWX macros could leave the radio stuck in TX if the queue drained
  without an explicit end-of-message marker. Now releases TX when
  the queue is empty.

**Audio mute restore on reconnect (#2489)**
- Reconnecting to the radio no longer restores per-slice audio mute
  state — that's a radio-authoritative setting per the
  Radio-Authoritative Settings Policy.

**QsoRecorder QTimer playback → QAudioSink pull mode (#2295, #2487)**
- QSO recorder playback used a `QTimer` to push audio buffers,
  producing audible glitches under high system load. Rewritten to
  `QAudioSink` pull mode for glitch-free playback.

**Slice capacity clamp to model limit (#2477)**
- Some radio models report a slice count higher than they actually
  support. Client now clamps to the model-specific limit.

**DVK F1-F12 shortcut conflict with CwxPanel (#2464, #2469)**
- F-key shortcuts triggered both DVK and CWX panels when both were
  visible. Resolved via panel-scoped shortcut routing.

**ShackSwitch button stale visibility (#2456)**
- Top-right tray's ShackSwitch button stayed visible after the
  ShackSwitch feature was disabled. Now toggles in lockstep.

**RX applet radeActivated guard (#2376)**
- `RxApplet` was missing the RADE-active guard that `VfoWidget`
  uses — could fire stale mode-change events while RADE was busy.

**Avoid unchanged spot overlay repaints (#2474)**
- Spot marker list was being rebuilt and repainted on every spot
  source update even when no visual change occurred. New
  `spotMarkersVisuallyEqual()` predicate gates the repaint.

**Reassert desired panadapter and waterfall display rates (#2465)**
- Radio reconnect could silently leave the panadapter and waterfall
  at the radio's default display rate (10 Hz) instead of the
  client's configured rate. Now reasserted on every reconnect.

**Windows 48 kHz L-R audio pan (#2403, #2459)**
- L-R audio panning was incorrect on the Windows 48 kHz resampler
  path. Restored to match the macOS / native-rate paths.

**Protocol: send client program/low_bw_connect before client gui (#2466)**
- Subscription order matters on the FlexLib side; sending `client
  program` and `client low_bw_connect` after `client gui` caused
  intermittent rejection of metadata. Reordered to match what
  SmartSDR-for-Windows does.

**TCI volume/drive/rx_volume command dispatch (#1764, #2463)**
- Three TCI v2.0 commands had no dispatch wiring on AetherSDR's
  side. Added the three handlers + bidirectional state sync.

**Spectrum cursor update guards (#2461)**
- Spectrum cursor position updates fired without checking widget
  lifetime, producing rare null-deref crashes during layout changes.

**AmpApplet telemetry forwarding (#2444)**
- Amplifier telemetry from the radio (id, vac, meffa, temp) was
  parsed but not forwarded to `AmpApplet`. Wired through.

**Windows UI scaling settings path (#2443)**
- Pre-Qt settings read on Windows used the Linux path convention.
  Corrected to `%APPDATA%/AetherSDR/`.

**FreeDV Station Msg live-update without restart (#2476)**
- The Station Msg field in FreeDV settings required an app restart
  to take effect. Now live-updates the reporter.

**Container pop-out glyph + size (#2499)**
- Switches the pop-out glyph to `⧉` at a larger size for
  discoverability.

**Minimal mode hides dock-side icons (#2562)**
- The three dock-side selectors (left dock, right dock, pop-out)
  and their separator are now hidden in minimal mode — they have
  no meaning when the spectrum is hidden.

**View → Applet Panel / Pop Out menu entries removed (#2540)**
- Both menu entries are redundant with the new title-bar dock-side
  icons (#2536) and the Ctrl+Shift+S window-scoped shortcut.

**Reconnect dialog chrome polish (#2541)**
- The reconnect dialog gets the frameless gradient title bar and
  proper chrome to match the rest of the app.

### Closed without fix

**`_platform_memmove` cost in macOS backing-store flush (#2547, #2548)**
- Two related claims about Qt's widget-RHI fallback cost on macOS
  (#2547) and 30 Hz hidden-widget timer storms (#2548). Implemented
  the #2548 fix in full (~150 LOC, 10 widgets timer-gated on
  visibility) and measured before/after with `perf record -F 99 -g`
  on Linux: paint pipeline cost moved 16.8 % → 15.7 %, within noise.
  Reverted; closed both as `wontfix`. Reopen with matched windowed
  macOS profiling if the cost ever reproduces.

### Infrastructure

**Buffered async file logging (#2478)**
- AetherSDR's log file path now flushes via a buffered async writer
  to avoid blocking the GUI thread on slow disks (especially network
  shares on macOS).

**FPS meters + perf telemetry framework (#2480)**
- Foundation for the System Info diagnostics dialog (#2554) — see
  Features above.

**actions/cache bump 4 → 5 (#2493)**
- Dependabot bump for the GitHub Actions cache action.

**Qt 6.7+ GPU rendering requirement documented (#2531, #756)**
- README now documents the minimum Qt version for GPU spectrum
  rendering. Older Qt falls back to CPU rasterization.

**README supported hardware refresh (#2532)**
- Aurora 510M, RT-series, ML-series, CL-series additions to the
  Supported Hardware list. AU-520 confirmed working.

**libopengl0 build dep documented (#2523)**
- Ubuntu/Debian build instructions now list `libopengl0` explicitly.

**1.0 release prep — stale reference sweep (#2515)**
- Sweep across CLAUDE.md, README.md, CHANGELOG.md, and
  `docs/architecture-pipelines.md` removing pre-1.0 framing and
  TODO breadcrumbs.

## [v0.9.8] — 2026-05-06

### Aetherial Audio Channel Strip RX + community-driven reliability sweep

A focused 24-hour sweep around the Aetherial Audio Channel Strip's RX
side and a long list of community-contributed fixes. The RX panel
gains DESS, full final-output / waveform stages, ADSP cluster bypass,
and parity with the TX layout. Hamlib NET-rigctl interoperability is
unblocked end-to-end — Not1MM, MacLoggerDX, fldigi, and any other
standard rigctld client can now drive AetherSDR's frequency, mode,
filter passband, RF power, PTT, and CW. The release also clears two
silent Windows footguns (UI Scale on Windows was completely broken;
spot clients crashed on disconnect cascade), and lands a CW-knob UX
upgrade plus a dialog chrome refit on the AetherDSP Settings window.

Big thanks to **@chibondking** (CJ Johnson — editable CW value fields,
cwDelay optimistic-update, UI Scale Windows + reset-to-100% fix, macOS
relaunch via `open -n`, PGXL OPERATE/STANDBY sync), **@jensenpat**
(spot client thread affinity Windows crash, hardware-PTT mic_selection
fix, filter width indicator, mode-correct widen/narrow shortcuts,
Not1MM rigctld interop), **@rfoust** (Robbie Foust — status bar
separator dot fix, audio shutdown null-guards), and **@s53zo** (MIDI
VFO jog-wheel stabilization). Also crediting **@ct1drb** for the
original orphaned rigctld pipe-separator fix that #2438 cherry-picked.

### New features

**Aetherial Audio Channel Strip — RX side (#2425)**
- The Channel Strip now has a fully-built RX path alongside the TX
  path. RX side mirrors the TX layout: AGC-G (formerly AGC-T), EQ,
  De-Esser, Compressor, Tube/Saturator, PUDU, Final Output, and
  Waveform tiles, all wired through the inline RX DSP pipeline in
  AudioEngine.
- The CHAIN status row's ADSP tile is now a Stage-style toggle that
  bypasses the entire client-side NR cluster (NR2 / NR4 / MNR / DFNR
  / RN2 / BNR) with a single click; an in-memory snapshot restores
  the prior NR state on un-bypass.
- ChannelStripPresets schema gains an optional `rx` block so saved
  profiles capture both TX and RX state. Old preset files without
  the block leave RX state untouched (forward-compatible).
- New StripRxOutputPanel with peak/RMS meter, MUTE, BOOST. Strip-side
  Waveform panel becomes side-aware so the same widget renders the
  TX or RX scope tap based on a `Side::Tx | Side::Rx` flag.

**Editable CW Delay / Speed / Sidetone / Pitch fields (#2429, chibondking)**
- The four CW value labels in the PhoneCwApplet are now `QLineEdit`
  widgets. Click any value and type a number directly — SmartSDR
  parity. `QIntValidator` clamps to the valid range, `editingFinished`
  drives the slider so the existing slider→model command path fires
  unchanged. `hasFocus()` guards on the slider valueChanged path AND
  on `syncCwFromModel` keep mid-edit input from being clobbered by
  radio echoes.

**Hamlib NET-rigctl / Not1MM interoperability (#2438, jensenpat, closes #2048 #2108)**
- Pipe separator (`|`) — rigctld's wire convention. AetherSDR only
  recognised `;`, so every pipe-prefixed command returned `RPRT -4`.
  Cherry-picks the orphaned PR #2051 fix from @ct1drb with full
  attribution.
- Multi-line bare `b` send_morse — Hamlib spec allows the morse text
  on the next line. Per-connection `m_pendingMorseLine` flag arms the
  next handleLine call to consume morse text verbatim. Required by
  Not1MM contest CW.
- RFPOWER `get_level` / `set_level` — was returning `RPRT -11` (silent
  failure). Wires through to `TransmitModel::setRfPower / rfPower()`
  with proper 0.0–1.0 ratio ↔ 0–100 percent conversion. `dump_state`
  set_level mask was hardcoded to KEYSPD only — now correctly reflects
  the full RFPOWER + KEYSPD set.
- Per-mode filter passband placement — `cmdSetMode` was applying the
  USB convention to every non-LSB mode, so `M CW 250` produced a
  155 Hz filter centered at 172 Hz instead of a 250 Hz filter centered
  on 0. Mirrors the canonical mode→filter-edge mapping from
  `VfoWidget::applyFilterPreset`.

**AetherDSP Settings dialog refit + VFO DSP launchers**
- AetherDSP Settings dialog gets the standard frameless 18 px gradient
  title bar (matching NetworkDiagnosticsDialog and AetherialAudioStrip)
  with grip glyph, min/max/close trio, drag-to-move, double-click to
  maximize, 8-axis resize.
- New `setDialogMode(true)` on AetherDspWidget bumps every inline
  font-size to 13 px to match the VFO DSP toggle row, applies a
  parallel 60×24 / 70×26 toggle-button geometry. Applet path leaves
  this off.
- Per-slice VFO DSP grid gains an `ADSP` button (opens AetherDSP
  Settings — same entry point as the Settings menu) and an
  `AetherVoice` button (toggles the Aetherial Audio Channel Strip).

### Bug fixes

**Spot client thread affinity Windows crash (#2420, jensenpat, fixes #1929 #2418)**
- `QTcpSocket` / `QUdpSocket` / `QWebSocket` create an internal
  `QSocketNotifier` whose Win32 message-loop affinity binds to the
  *construction* thread, not the QObject's current thread. Constructing
  on main-thread then `moveToThread()` to `SpotClients` left the
  notifier bound to main-thread; disconnect-cascade socket events
  tripped a cross-thread `sendEvent` assert. Each client now defers
  socket creation to an `initialize()` slot dispatched on the worker
  thread via `Qt::QueuedConnection` so internal notifiers bind there.

**Audio callbacks during shutdown (#2413, rfoust)**
- Quit-time crash on macOS where `WanConnection::~WanConnection`
  triggered a TLS disconnect cascade that emitted
  `radioTransmittingChanged(false)` after `m_audio` was already null
  but before `QObject::~QObject` had auto-disconnected the connected
  lambdas. Surgical null guards on the three TX-state callbacks
  (`moxChanged`, `txAudioGateChanged`, `radioTransmittingChanged`)
  match the existing `if (m_audio && ...)` pattern used elsewhere in
  the file.

**Hardware PTT into radio with mic_selection=PC (#2431, jensenpat, fixes #2373 #2200)**
- The interlock gate was forcing `setTransmitting(false)` whenever
  `m_txRequested` was false — even when the radio reported our handle
  as TX owner. Hardware-keyed PTT (mic-line, ACC footswitch, RCA
  TXREQ) had no bypass. PC mic audio with hardware PTT had likely
  never worked through this branch. Parses the radio's interlock
  `source=` field (SW / MIC / ACC / RCA / TUNE per FlexLib v4.2.18
  `ParsePTTSource`) and adds `MIC|ACC|RCA` to the bypass list. SW
  source still requires `m_txRequested` so SSB optimistic-off
  responsiveness is preserved.

**UI Scale not applied on Windows; reset to 100% silently ignored (#2432, chibondking)**
- Two unrelated bugs in `main.cpp`'s pre-Qt settings read. (1) The
  pre-Qt path was hardcoded to `~/.config/AetherSDR/` (Linux
  convention); on Windows AppSettings actually writes to
  `%APPDATA%/AetherSDR/`, so the file was never found and
  `QT_SCALE_FACTOR` never set. Adds a `Q_OS_WIN` branch using
  `qEnvironmentVariable("APPDATA")`. (2) The `if (pct != 100)` guard
  meant a child process inheriting `QT_SCALE_FACTOR` from a scaled
  parent kept using it after the user reset to 100%. Always-set fix
  writes `1.00` so the inherited value is overridden.

**macOS relaunch via `open -n` on UI Scale restart (#2434, chibondking)**
- `applyUiScale()` was calling `QProcess::startDetached` on
  `applicationFilePath()`, which on a `.app` bundle launches
  `Foo.app/Contents/MacOS/Foo` directly — bypassing Launch Services.
  The relaunched instance shows as a separate dock entry, loses
  proper activation policy, and on notarized/sandboxed builds can
  fail entirely. Walks up to the bundle root and launches via
  `open -n <bundle>` with `--args` pass-through.

**MIDI jog-wheel VFO tuning stabilization (#2422, s53zo, fixes #2421)**
- Three independent failure modes on touch-sensitive jog wheels (e.g.
  Hercules DJControl Starlight). MIDI Learn could bind the touch
  sensor instead of the movement CC. Saved bindings without the
  relative flag interpreted unit-pulse 1/127 as absolute endpoints,
  turning each detent into ~±63 steps. Tuning targets read
  `SliceModel::frequency()` which lags behind in-flight tune commands,
  making rapid spins jump backward. Fixed in three layered passes
  with a 250 ms idle-timer-cleared in-flight tune target.

**VFO filter width indicator showing 0.1 kHz too low (#2435, jensenpat, fixes #2197)**
- `updateFilterLabel()` computed raw `filterHigh - filterLow`, which
  for SSB/digital modes is ~100 Hz less than the named preset width
  due to the intentional low-cut. Bug had been reported three times
  (#794, #1225, this issue) — the structural fix promotes
  `RxApplet::formatFilterWidth` to public static and has VfoWidget
  call it directly. Single source of truth prevents the next swap.

**Mode-correct filter widen/narrow shortcuts (#2436, jensenpat, fixes #2208)**
- The `filter_widen` / `filter_narrow` shortcuts (also bound to MIDI
  `global.filterWiden` / `global.filterNarrow`) only adjusted the
  upper edge by ±100 Hz. On LSB / CWL / DIGL / RTTY the upper edge is
  fixed near 0, so "widen" actually collapsed the passband. New
  `RxApplet::stepFilterWidth(direction)` walks the active mode's
  preset list and routes through `applyFilterPreset` so all modes get
  mode-correct edge geometry.

**PGXL OPERATE/STANDBY button stuck on OPERATE (#2437, chibondking)**
- AmpApplet button was only updated via the direct PGXL TCP path; if
  that lagged, the button stayed on the old label. The click handler
  reads `m_operateBtn->text()` to decide toggle direction, so a
  stuck OPERATE button emitted `operate=0` on click — sending another
  standby command instead of returning to OPERATE. Wires
  `RadioModel::ampStateChanged` (authoritative) to also call
  `AmpApplet::setState`.

**Status bar separator dots for unsupported optional sections (#2412, rfoust)**
- TGXL and PGXL separator dots stayed visible even when the indicators
  were hidden, leaving stray `· ·` clusters in the bottom status bar
  on radios without those accessories. Optional separators now toggle
  in lockstep with their matching indicators.

**cwDelay optimistic-update prevents Delay slider reset (#2428, chibondking)**
- `setCwDelay()` was the lone outlier in the CW setter family —
  didn't cache `ms` into `m_cwDelay` before emitting the command.
  Subsequent `phoneStateChanged` emissions read stale `m_cwDelay`
  and snapped the Delay slider back, silently re-enabling QSK on
  amplifiers that can't tolerate it. Now matches the
  `setCwBreakIn` / `setCwIambicMode` optimistic-update pattern.

**DSP-level slider missing on launch**
- The leveled DSP toggles (NR / NB / ANF / NRL / NRS / NRF / ANFL)
  push onto a target stack so the shared 4th-row slider re-targets
  the most recently activated DSP. State changes arriving from radio
  status (profile load, reconnect) went through a `QSignalBlocker`'d
  path that suppressed the `toggled()` signal — so on launch the
  slider was hidden until the user manually toggled the DSP off and
  back on. New `connectLeveledDsp` helper mirrors the user-click
  stack push/pop on every state change.

**Channel Strip initial height on sub-4K displays (#2440)**
- The Aetherial Audio Channel Strip opened at its natural 1620 px
  height on every display. On 1080p the bottom edge landed offscreen
  with the resize grip unreachable. Queries
  `QGuiApplication::primaryScreen()->availableGeometry().height()`
  at construction; caps initial height to 960 px on anything below
  ~4K (2000 px threshold catches 1080p / 1440p / 1600p). 4K and 5K
  still get the natural 1620.

### Infrastructure

**CI runtime + reliability sweep**
- Skip CodeQL on doc-only PRs (#2397).
- Cache Windows third-party deps — Opus, FFTW, hidapi, DeepFilterNet
  (#2398) — drops cold Windows builds by ~3 min.
- Mirror Opus download to GitHub Releases with retry/fallback (#2399)
  — replaces the unreliable Xiph mirror that occasionally 503s.
- Scope CodeQL to first-party sources (#2400) — drops third_party/
  noise from the analysis.
- Speed up macOS Intel build with pinned Qt + ccache (#2402).
- Bump Qt to 6.8.3 LTS across all release workflows (#2404).

## [v0.9.7] — 2026-05-05

### CW keying overhaul + reliability sweep

A focused follow-up to v0.9.6 with two themes: a substantial CW operator
upgrade — keyboard and MIDI-mapped straight key + iambic paddles, full
break-in / QSK respect on both paths, netCW timing fixes, and Apollo-era
Quindar tones on PTT engage/disengage — and a reliability sweep covering
SpotHub auto-reconnect, a TCI crash on quit, the long-standing waterfall
TX trail, hardware PTT regressions, and DAX RX latency.

Big thanks to **@jensenpat** (CW keyboard / MIDI controls, compression
meter gating, Windows MSVC runtime), **@M7HNF-Ian** (Spot Lines,
HiDPI gauge clipping, TCI crash on quit, SpotHub auto-reconnect),
**@filemakers** (connect-to-last-radio opt-out checkbox), **@NF0T**
(RADE TX policy naming + tests), **@chibondking** (live voltage gauge),
plus issue reporters **@VU2CPL** (TCI crash with full call stack),
**@luigiverdicchio1-prog** (SpotHub failed-reconnect repro), **@LU5DX**
and **@rnash2** (spectrum amplitude scale issue — under investigation).

### New features

**CW keyboard + MIDI controls (#2361, #2391, jensenpat)**
- Three new shared shortcut + MIDI actions: `Trigger straight key`,
  `Trigger CW Left Paddle`, `Trigger CW Right Paddle`. Same action IDs
  and display names for keyboard shortcuts and MIDI mappings (`cwkey` /
  `cwdit` / `cwdah`); `MidiSettings` migrates legacy dotted IDs
  (`cw.key` / `cw.dit` / `cw.dah`) on read.
- Straight key is a true momentary control: keyboard press + MIDI gate-on
  assert the netCW key path, release + gate-off release it. Paddles feed
  the local iambic keyer when running so sidetone and on-air timing stay
  aligned with WPM; falls back to a held-key path otherwise.
- Both paths now honor the radio's `break_in` setting fully — with
  `break_in=1` (QSK), key edges trigger TX and `break_in_delay` holds
  the relay between elements; with `break_in=0`, keys are queued and the
  operator engages PTT manually (Space PTT, MOX, hardware PTT). The
  previous auto-PTT envelope masked break-in OFF and killed QSK hang
  time on release.
- New clickable red TX status badge in the status bar acts as an
  emergency transmit cancel — checks Multi-Flex ownership, clears all
  local CW state, forces sidetone key-up, and sends `cw key 0` /
  `cw ptt 0` / `transmit tune 0` / `xmit 0`.
- Slider focus lease no longer blocks momentary CW shortcuts or Space
  PTT — clicking the CW delay slider previously froze J/K/L paddle
  keying and Space PTT for 2+ seconds while the lease's arrow-key
  capture timer expired.
- Pre-existing typo fix: J tile in the on-screen keyboard widget was
  bound to `Qt::Key_I`, so binding a shortcut to J in the editor
  selected I.

**Quindar tones — Apollo-era K/BK on PTT (#2334, fixes #2262)**
- Optional 2525 Hz "K" / 2475 Hz "BK" tones on PTT engage / disengage,
  modeled after Apollo CapCom audio. Lock-free DSP module wired between
  PC mic gain and the final TX limiter so tones are mixed into the
  outgoing audio. Local sidetone via a dedicated 48 kHz `QAudioSink`
  (mutually exclusive with CW mode at the mode level — Quindar and CW
  share the same sidetone bus).
- Added as the QUIN chip in the Final Output Stage panel of the
  Aetherial Audio Channel Strip with a frameless editor dialog
  (test-tone buttons + Done in the same row as the title bar).
- Disabled by default; opt-in via the chip toggle.

**netCW keying fix + trace logging (#2336, fixes a long-tail of issues)**
- Fixes netCW timing for both straight-key and iambic CW. Adds detailed
  `aether.cw` trace category logging across MIDI / keyboard input,
  netCW UDP scheduling, VITA stream/index details, and TCP fallback /
  backstop paths so future timing regressions are diagnosable from logs.

**Spot Lines toggle in SpotHub (#2349, M7HNF-Ian)**
- New "Spot Lines" toggle in the SpotHub Display settings draws vertical
  lines from the spectrum up to each spot label. Off during contests to
  reduce visual clutter; on for casual ops to keep label-to-frequency
  mapping legible at a glance.

**Network Diagnostics live logs tab (#2333)**
- New tab in the Network Diagnostics dialog with live tail of the same
  logs the diagnostics report references, scoped to the diagnostic
  categories (`aether.connection`, `aether.dxcluster`, `aether.cw`,
  etc.). Lets users diff what they're seeing against what was logged
  without leaving the dialog.

**Connect-to-last-radio opt-out checkbox (#2390, filemakers)**
- New "Connect to last radio on start up" checkbox on the connection
  dialog; defaults to ON so existing users keep current behavior. When
  unchecked, AetherSDR no longer auto-connects on startup, on
  broadcast-discovery, or on routed-radio probe — useful for operators
  who want to pick a radio manually each session. The dialog
  auto-launches at startup when opt-out is enabled so the user has a
  clear path to pick a radio.

### Bug fixes

**SpotHub auto-reconnect after failed connection attempt (#2394, M7HNF-Ian, fixes #2380)**
- SpotHub (DX cluster / RBN) didn't automatically reconnect after a
  Wi-Fi drop or any failed connection attempt — Qt's `QAbstractSocket`
  only emits `disconnected()` on Connected → Unconnected transitions,
  so when the socket failed during `ConnectingState` (host blocked,
  refused, or timed out), the reconnect timer was never armed. Extracts
  a `scheduleReconnect()` helper called from all three failure paths
  (live drop, socket error, connect timeout) with guards against
  double-scheduling. A per-call epoch counter prevents stale timeouts
  from a previous attempt aborting a later successful one. Backoff
  sequence unchanged: 5 s → 10 s → 20 s → 40 s → 60 s.

**TCI crash on quit when TciServer outlives RadioModel (#2386, M7HNF-Ian, fixes #2385)**
- `TciServer` was constructed as a `QObject` child of `MainWindow`, so
  Qt deleted it during `~QWidget::deleteChildren()` — which runs *after*
  `MainWindow`'s value members (including `m_radioModel`) have already
  destructed. `~TciServer()` → `stop()` → `releaseDaxForTci()` then
  dereferenced freed memory. EXC_BAD_ACCESS @ `0x38`, 100% reproducible
  on quit when a radio was connected. Reported by VU2CPL with full call
  stack. Fixed by explicitly tearing down `m_tciServer` in
  `~MainWindow()` after the audio thread is stopped but while
  `m_radioModel` is still alive, plus a belt-and-braces
  `QPointer<RadioModel>` so the existing null guards in
  `releaseDaxForTci()` catch any future regression automatically.

**Waterfall unfreezes on radio interlock state, not MOX edge (#2368, fixes #1927)**
- Waterfall freeze/unfreeze previously gated on the local MOX edge,
  which fired the instant the user released PTT — the radio kept
  streaming TX-contaminated tiles for the `UNKEY_REQUESTED` window, and
  those rows then took 10–23 s to scroll off the visible waterfall.
  Now driven by `RadioModel::radioTransmittingChanged` (interlock
  `state=TRANSMITTING`), so the freeze is held until the radio
  actually leaves TX. Multi-Flex bonus: any client TXing now triggers
  the freeze, not just our client.

**Honor cw break_in for keyboard CW keying (#2391)**
- `RadioModel::sendCwKey` and the iambic keyer's `onPaddleEvent` both
  unconditionally wrapped each key/squeeze in a `cw ptt 1`...`cw ptt 0`
  envelope. Made break-in OFF a no-op (auto-PTT forced TX anyway) and
  killed the radio's break_in_delay hang time with break-in ON
  (force-dropped PTT after every element). Stripped the auto-PTT in
  both paths so the radio's break-in setting decides TX behavior,
  matching SmartSDR semantics.

**Spot list double-click switches mode (#2372, fixes #2298)**
- Double-clicking a spot in the Spot List now switches the radio's
  mode along with the frequency. Previously only the frequency moved,
  leaving the user in the wrong mode for the spotted signal.

**Spot trigger includes pan= for external CAT clients (#2369, fixes #2366)**
- The spot click protocol now includes `pan=<panId>` so external CAT
  clients (N1MM Logger, etc.) consuming AetherSDR's spot triggers can
  route the click to the correct panadapter in multi-pan setups.

**Compression meter gates on radio TX state (#2363, jensenpat)**
- The TX compression meter now displays only while the radio reports
  `state=TRANSMITTING`. Previously it could read live during RX from
  stale interlock state, confusing operators about what compression
  was actually applied to their signal.

**Voltage gauge label shows live radio voltage (#2362, chibondking)**
- The voltage gauge label now reflects the live voltage value reported
  by the radio instead of the static "VOLTS" placeholder.

**Minimal mode revert on macOS when entered while maximized (#2367, fixes #2365)**
- Entering minimal mode while the window was already maximized on
  macOS would revert to a non-minimal layout immediately on toggle.
  Layout / state ordering corrected so the entry sticks regardless of
  the prior maximized state.

**MidiControlManager: drop dead paramAction signal (#2370)**
- Cleanup follow-up after #2336 introduced `paramActionTrace` —
  `paramAction` was still emitted but had zero connectors. Removed
  the orphan signal + matching declaration.

### Infrastructure

**DAX RX native pw_stream source on Linux (#2312, fixes #1008)**
- DAX RX latency on Linux drops from ~400 ms → ~200 ms via a native
  PipeWire `pw_stream` source path, replacing the previous PulseAudio
  client. Brings DAX RX latency in line with macOS / Windows.

**RADE TX policy naming + tests (#2353, NF0T, fixes #2343)**
- Renames the RADE TX policy enum to align with the issue tracking
  language (`HostedDaxBridge` etc.) and adds unit tests covering each
  `(reason, platform, mode) → (allowed, note)` decision row.

**HiDPI gauge clipping (#2346, M7HNF-Ian)**
- HGauge tick labels and TxApplet value labels were clipping on HiDPI
  displays. Layout / paint regions updated to honor device-pixel ratio.

**Windows portable ZIP includes MSVC runtime (#2364, jensenpat)**
- Windows portable ZIP now bundles the MSVC runtime DLLs so the
  portable build runs on systems without Visual C++ Redistributable
  installed — matches the experience of the installer build.

**Connection-panel slider focus lease split**
- `shortcutInputCaptured()` was conflating "slider has focus and
  arrow-key lease is active" with "user is typing in a text field",
  blocking momentary CW shortcuts and Space PTT during slider drags.
  Now split into `textInputCaptured()` (text widgets only) and
  `shortcutInputCaptured()` (text + slider lease) so QShortcut
  dispatch keeps the arrow-key behaviour while momentary actions
  bypass the lease. Bundled with #2391.

## [v0.9.6] — 2026-05-04

### Aetherial Audio Channel Strip + AetherSweep Phase 2

Headline additions: a unified **Aetherial Audio Channel Strip** that brings
every TX DSP stage (gate, EQ, compressor, de-esser, tube, AetherVoice,
reverb, and a brand-new Final Output Stage with brickwall limiter) into a
single editable window with a savable preset library — and **AetherSweep
Phase 2**, a polish pass on the in-panadapter SWR analyzer with log scale,
threshold-band shading, interpolated bandwidth at SWR ≤ 1.5 / 2.0, a
resonance caret, and band-change-aware auto-stop.  Network Diagnostics
gained trend graphs and per-series gutter hints.  RADE finally ships in
the official Windows installer.

Big thanks to **@jensenpat** (oscillator reference fix, AetherSweep
Phase 2, Connect-by-IP recents, minimal mode polish, Windows installer
packaging), **@NF0T** (RADE on Windows CI/installer, RADE mic meter
fix), **@rfoust** (Network Diagnostics trend graphs), and
**@AetherClaude** (stderr non-draining-pipe deadlock) for landing the
bulk of this release.

### New features

**Aetherial Audio Channel Strip (#2307, #2326)**
- New unified TX DSP window covering every stage in the chain: gate, EQ,
  compressor, de-esser, tube, AetherVoice (the exciter formerly known as
  PooDoo), Freeverb, plus a brand-new Final Output Stage with brickwall
  limiter, output trim, DC block, and a 1 kHz test tone.
- Savable preset library at `~/.config/AetherSDR/ChannelStrip.settings`
  (separate from `AetherSDR.settings`).  Captures every TX DSP knob, the
  user's chain order, and final-limiter parameters.  Save / Delete /
  Export Preset / Export Library buttons in the strip's preset row.
- Master/Aux BYPASS button stays in lock-step between the docked Chain
  applet and the strip via a shared engine-level snapshot — flipping it
  in either place updates both.
- Two new 4th-row panels: **Aetherial Waveform — TX** (1–20 s scope window
  with SCOPE/ENVELOPE/HISTORY modes) and **Final Output Stage** (peak-hold
  meter with GR overlay + draggable amber ceiling triangle, mouse-wheel
  ±0.1 dB).
- Logo / title rebrand: PooDoo™ exciter → **AetherVoice™** "Aetherial
  Voice Processor" with Body / Clarity emphasis controls.  Docked tile
  (`PUDU`) renamed to `EVO` on the chain widget; container button
  renamed `VUDU`.
- Double-click any TX chain tile launches the Channel Strip — replaces
  the legacy easter-egg launch nub on the Chain applet and the
  per-stage TX floating editors (`ClientDeEssEditor`,
  `ClientReverbEditor`, etc.).
- FreeVerb tile in the docked applet panel now shows the same
  decay-tail viz as the Channel Strip's reverb panel, signal-driven by
  a new `AudioEngine::clientReverbStateChanged` signal (no polling).

**AetherSweep Phase 2 (#2320, jensenpat)**
- Logarithmic SWR scale for cleaner low-ratio detail; threshold-band
  shading for SWR 1.0–1.5 (green), 1.5–2.0 (amber), and >2.0 (red).
- Resonance caret + dot at the best measured SWR point; visual
  start/end notches mark sweep endpoints.
- Interpolated bandwidth brackets at SWR ≤ 1.5 and ≤ 2.0, with a
  concise BW readout in the corner label.
- Resonant frequency display trimmed to kHz precision; corner readout
  switched from "latest sample" to best SWR + resonance frequency.
- Cleared sweep plots when the swept TX slice changes bands; if a band
  change happens while a sweep is running, the sweep stops and clears
  without restoring the old freq/pan range so cleanup doesn't fight
  the user's new band.

**Network Diagnostics trend graphs (#2309, rfoust; #2316)**
- Per-metric trend plots with Timeframe selector (1 min / 5 min /
  15 min / 1 h / 1 d / 1 w).
- Logarithmic Y-axis on the Rates tab — 0 / 1 / 10 / 100 / 1k kbps
  decades all visible at once instead of being squashed at the
  baseline.
- Per-series last-sample hints in the left gutter — small
  color-coded values that always show the latest reading for each
  visible stream, with a 6 px-tall solid centerline alpha gradient
  to keep the labels legible against the trace.
- Frameless chrome with draggable title bar + 8-axis resize matching
  the rest of AetherSDR's floating windows.

**RADE in the official Windows installer (#2324, NF0T)**
- `ENABLE_RADE=ON` for both the Windows CI job and the official
  installer workflow.  Vendored RADE-prepared Opus + neural-net
  weights are statically linked, so no new DLLs ship — `AetherSDR.exe`
  grows ~8–12 MB to match the AppImage.
- New `Build Opus (RADE dependency)` CI step provides clean failure
  attribution and matches the AppImage workflow's pattern.  Closes
  reports of "RADE absent from Windows installer".

**Connect by IP recents dropdown (#2296, jensenpat)**
- Connect-by-IP field is now a combo box pre-populated with the last
  five addresses (stored as `RecentDirectIpAddresses`).  Successful
  connect promotes the address to the top of the list.

**Black slider auto-offset (#2328)**
- Spectrum Overlay menu's Black slider now drives a noise-floor target
  offset while AUTO is engaged: 50 = at the noise floor (today's
  behaviour); lower = darker (push threshold above the floor); higher
  = lighter.  Manual mode keeps existing semantics.  Both stored
  values persist independently so toggling AUTO swaps the slider
  position without losing either preference.

**Minimal mode polish (#2290, jensenpat; #2299)**
- Title bar drag now reaches the gutter and other previously-dead
  zones via sub-pixel hit testing.  Exit paths from minimal mode
  consolidated; layout stops fighting on toggle.

**Aetherial Noise Reduction docked applet (#2297)**
- Client-side NR controls live in their own docked applet under
  PooDoo Audio (RX); the redundant DSP sub-panel was removed from
  the Spectrum Overlay menu for a cleaner DSP surface.

### Bug fixes

**Oscillator reference status no longer goes stale (#2329, jensenpat)**
- New `RadioModel::oscillatorChanged()` signal drives the status-bar
  reference label and Radio Setup combo immediately from oscillator
  state, instead of relying on event ordering with GPS status.
  Eliminates the case where the status bar got stuck on the previous
  reference if a GPS update never arrived after the oscillator
  transition.
- Radio Setup dropdown preserves currently-selected/actual options
  during presence transitions, so transient flag blips no longer
  drop the active option from the combo.  Renamed `External` →
  `External 10 MHz` to match the FlexLib API description.
- Status-bar tooltips now show desired setting, actual source, lock
  state, and GPS/external details.

**RADE mic level meter + gain slider (#2292, NF0T)**
- The RADE TX mic level meter now updates while the modem is active,
  and the gain slider is enabled instead of stuck at zero.  Fixes
  "no apparent way to set RADE mic level" reports.

**Stderr non-draining pipe deadlock (#2300, AetherClaude)**
- When stdout/stderr was redirected to a pipe with no reader, the
  audio worker thread could deadlock writing log lines.  Replaced
  blocking writes with a non-blocking path that drops on the floor
  rather than stalling the thread.

**Windows installer runtime packaging (#2303, jensenpat)**
- Tightened Windows runtime-DLL bundling for a smaller installer and
  fewer "missing DLL" reports on first launch.

### Infrastructure

**v0.9.5.1 release-notes expansion (#2288, #2289)**
- The CHANGELOG entry for v0.9.5.1 was expanded post-release to
  cover all post-v0.9.5 fixes (#2113 reachability sweep, the four
  rfoust polish PRs, NR2 audio-thread + Qt-log hotfixes) with full
  contributor shoutouts.

## [v0.9.5.1] — 2026-05-02

### Stability & polish hotfix sweep

A focused follow-up release that lands the TCI TX policy hotfix plus six
post-v0.9.5 fixes already in main: SmartLink WAN reconnect after radio
drops, sequenced WAN disconnect teardown, RX slice-tab reset between
radios with different slice counts, macOS panadapter pop-out live
updates and dock-splitter layout, NR2 wisdom-generation safety on the
audio thread, and a Qt log-handler serialization fix that resolves a
macOS tune-time crash.

Big thanks to **@rfoust** (four SmartLink / disconnect / macOS polish
fixes) and **@jensenpat** (NR2 audio-thread safety + Qt log
serialization) for landing the bulk of this release.

### Bug fixes

**TCI TX silent on Windows / Linux non-PipeWire (#2276)**
- `evaluateDaxTxPolicy()` now always allows `DaxTxRequestReason::TciTxAudio`
  regardless of platform / hosted-DAX availability.  TCI receives audio
  over WebSocket and feeds it into a dedicated `dax_tx` stream that's
  independent of SmartSDR DAX2 (which owns the Windows DAX *audio
  devices*, not the radio's `dax_tx` stream slot — multiple GUI clients
  can each register their own).
- Test assertions in `tests/radio_status_ownership_test.cpp` flipped to
  match the corrected policy and a new Linux-non-PipeWire test case
  added.

**SmartLink reconnect after WAN drop (#2282, rfoust)**
- `MainWindow` now owns a WAN reconnect timer that re-requests a
  SmartLink radio connection using the last selected WAN radio when
  `RadioModel` reports an unexpected WAN disconnect, instead of
  leaving the app stuck on the "Radio disconnected — waiting for
  reconnect" popup.
- `SmartLinkClient::reconnect()` refreshes Auth0 credentials via the
  saved refresh token before reconnecting, avoiding reuse of an
  expired `id_token`.  Auth-refresh failure stops the retry loop and
  shows a sign-in-required status instead of retrying forever.
- `RadioModel::forceDisconnect()` now handles WAN connections so
  missed pings transition the app to disconnected/reconnecting
  promptly.  Ping watchdog logs and forces disconnect once per outage
  rather than every second.
- `PanadapterStack::prepareShutdown()` releases QRhi GPU resources
  before main-window teardown to avoid the macOS Metal teardown crash
  that could fire after a stale-state Disconnect.

**SmartLink disconnect teardown (#2278, rfoust)**
- WAN disconnect previously closed the WAN socket *after* disconnecting
  RadioModel's signals and nulled `m_wanConn` directly, skipping
  `RadioModel::onDisconnected()` cleanup entirely.  Panadapters,
  slices, meters, and streams stayed alive in the model after the user
  clicked Disconnect.
- Now runs the normal model teardown path on intentional WAN
  disconnect, so WAN sessions emit `connectionStateChanged(false)` and
  clear model state the same way LAN disconnects do.

**Reset RX slice tabs on disconnect between radios with different slice counts (#2254, rfoust)**
- `RxApplet::clearSliceButtons()` tears down generated slice tab buttons
  and restores the static slice badge on disconnect.  Stale A–H buttons
  no longer linger after switching from a high-slice radio to a
  smaller one.
- `MainWindow`'s `infoChanged` initializer is now per-connection rather
  than one-shot, so each radio rebuilds its slice row from its own
  `maxSlices`.  Slice button click connections are guarded against
  duplicate signal handlers across reconnects.

**Qt log handler serialization fixes macOS tune-time crash (#2284, jensenpat)**
- The global `qInstallMessageHandler` callback wrote through a single
  `QFile*` without synchronization, and concurrent `qCInfo`/`qDebug`
  output from main + worker threads corrupted Qt's internal file
  write-buffer state.  The tune-policy diagnostic line happened to be
  the log call that exposed the corruption — the failing object was
  the logging sink, not the Flex tune command path.
- Now serializes the handler with an intentionally leaked mutex
  (mirroring the existing shutdown-safe treatment of the redaction
  regexes) and replaces per-message `QTextStream` wrapping with a
  direct UTF-8 `QFile::write()`/`flush()` path.  No change to radio
  command ordering or panadapter policy.

**macOS panadapter pop-out refresh + multi-pan dock layout (#2280, rfoust)**
- Detached panadapter windows on macOS no longer show a static/stale
  spectrum image.  Cross-window reparenting now resets the native
  QRhi/Metal surface and re-requests pan dimensions from the radio
  after every float/dock cycle.
- Saved floating-window state is no longer restored after later
  user-added pans, so adding a second panadapter does not spawn an
  unwanted blank floating window.
- `rebuildDockedSplitter()` keeps the main-window splitter compact
  when multiple pans float/dock — no more empty placeholder slots.

**NR2 wisdom generation no longer freezes the audio thread (#2275, jensenpat)**
- `AudioEngine::needsWisdomGeneration()` previously only checked
  whether `aethersdr_fftw_wisdom` existed.  If the file was stale or
  incompatible (e.g. `fftw-3.3.10` header on a build that uses FFTW
  3.3.11), `setNr2Enabled()` ran full FFTW wisdom generation on the
  audio worker thread, blocking RX audio and the WAVE scope for
  several minutes.
- Adds `SpectralNR::loadWisdom()` for import-only validation, makes
  `needsWisdomGeneration()` return true when the file exists but
  cannot be imported, and routes generation through the existing
  background progress dialog instead of the audio thread.  If wisdom
  import fails at enable time, NR2 falls back to runtime
  `FFTW_MEASURE` plans rather than hanging audio.

## [v0.9.5] — 2026-05-02

### Reliability sweep + DAX2 coexistence overhaul

A focused stability release.  Headline is **DAX2 coexistence policy
(jensenpat)** — a centralized decision table that makes AetherSDR's
behaviour next to SmartSDR DAX, hosted DAX (macOS / PipeWire), and
external Flex tooling explicit, testable, and consistently logged.
Plus substantial improvements to **disconnect teardown** (sequenced
`stream remove` with response wait — fixes the Flex-6300 unreachable-
after-reconnect bug), **worker-thread shutdown** (no more `killTimer`
warnings on app exit), **panadapter zoom** (no spectrum flash), and
**multi-flex spot/ATU handling** (DXCluster spot trigger_action removal,
ATU per-frequency toggle).

A two-PR **issue triage cleanup** closed 12 long-standing issues across
small UI bugs, contrast, indicator routing, click-to-tune behaviour,
and visible regressions from recent feature work.

### Features

**Aetherial Parametric EQ Smoothing combo (#2236)**
- New Smoothing combo on the EQ analyzer with 1/24, 1/12, 1/6, 1/3,
  1/1 octave fractional smoothing options (linear-power averaging).
  Persists via `EqAnalyzerSmoothing`; defaults to 1/12 octave.
- Audio-domain band-plan strip on the EQ canvas + dashed yellow filter
  cutoff guides for both TX and RX.  Cutoff guides are draggable to
  retune filter edges with mode-aware USB/LSB/AM/FM offset conversion.
- Phone applet Low/High Cut buttons snap to nearest 50 Hz.
- ~30 % faster smoothing inner loop after replacing
  `std::pow(10, x/10)` with `std::exp(x * ln(10)/10)` (#2239).

**Custom filter edges with persistence (#2272)**
- Right-click a filter button → **Set Custom Edges...** opens a
  2-spinbox dialog.  Asymmetric `lo:hi` pairs persist in
  `FilterPresets_<mode>` via a new mixed `width` / `lo:hi` storage
  format (backward compat preserved for existing width-only entries).
- **Reset to Default** is now per-slot.
- FilterPassbandWidget shows signed lo/hi/center values so LSB/CW
  filters reveal sideband.
- Filter-edge grab zone in the spectrum bumped from 5px to 8px so
  edges sitting near the VFO line are easier to grab.

**Spot click client-side mode mapping (#2272)**
- All client-ingested spots (DXCluster, RBN, POTA, SpotCollector,
  FreeDV, WSJT-X, manual) now ship with `trigger_action=none`.  The
  radio no longer auto-acts on stored mode strings (which mishandled
  "SSB" and similar non-Flex tokens, falling back to CW).
- AetherSDR's auto-mode mapping handles tune+mode client-side.
  Default flipped to enabled.
- FreeDV-source spots activate the RADE engine via `activateRADE()`
  (sets DIGU/DIGL plus the OFDM modem) instead of just landing on a
  plain digital mode.

**ATU per-frequency toggle (#2272)**
- ATU button now mirrors SmartSDR's behaviour: first click on a
  frequency tunes; second click at the same frequency bypasses; freq
  change resets the toggle so the next click tunes again.

### Bug fixes

**DAX2 coexistence policy (#2271, jensenpat)**
- New `DaxTxPolicy.h` decision table: `(reason, platform, mode) →
  (allowed, note)`.  Centralizes platform-aware DAX TX decisions with
  explicit reasons (HostedDaxBridge, TciTxAudio, ExternalDaxRouteOnly,
  GenericAudioRecreate) and consistent log notes at every branch.
- Lazy `dax_tx` stream creation via `RadioModel::ensureDaxTxStream()`
  — eager creation at GUI attach is removed so SmartSDR DAX can own
  the Windows route.
- `daxTxStatusCanUpdateLocalState()` anti-stomp: foreign DAX TX
  status can no longer accidentally adopt our tracked stream slot.
- LAN VITA UDP rebind path: on Flex's `0x500000A9` "Port/IP pair
  already in use" error, AetherSDR rebinds to an OS-assigned ephemeral
  port and retries `client udpport`.  WAN/SmartLink already used
  ephemeral ports and is correctly excluded from the rebind path.
- Dead-orphan DAX RX detection: `client_handle=0 ip=0.0.0.0` entries
  are ignored as not-our-stream rather than treated as legacy-compat.
- External DAX TX/RX visibility: separate seen-once log lines so the
  radio's external streams are visible in diagnostic logs without
  polluting our local state.
- 24 + test assertions across `testStreamStatusOwnershipCompatibility`,
  `testDaxTxStatusOwnership`, `testDaxTxPolicy`, and
  `testUdpRegistrationPolicy`.
- Linux non-PipeWire builds correctly skip both `setDax(1)` and DAX
  stream creation, so digital TX falls back to the physical mic input
  rather than going silent (#2273).

**Sequenced radio disconnect teardown (#2247, jensenpat)**
- Fixes Flex-6300 / 6400 unreachability after reconnect (#2113, #2218).
- Disconnect now sends `stream remove 0x<id>` and waits up to 2 s for
  the radio's response before closing TCP.  Without this, fire-and-
  forget teardown left stale Flex sessions that refused subsequent
  connects until reboot.
- Drops the rejected `client disconnect <self_handle>` send — verified
  against FlexLib's own self-disconnect path, which uses the `0x04`
  "dying gasp" byte before TCP close.  AetherSDR now does the same.
- Defensive auto-reconnect arms on refused-connect paths that don't
  emit a `disconnected` signal.

**Worker object shutdown thread affinity (#2248, rfoust)**
- Eliminates `QObject::killTimer: Timers cannot be stopped from
  another thread`, `QObject::moveToThread: Current thread is not the
  object thread`, and `~QObject` warnings on app exit.
- Worker-thread QObjects (audio engine, spot clients, ext controllers,
  radio connection, panadapter stream) are now destroyed via
  `deleteLater()` on their owning threads before each thread quits.

**Panadapter zoom keeps spectrum visible (#2246, rfoust)**
- New `reprojectSpectrum()` mirrors the existing waterfall reprojection.
  Zoom changes that overlap with the previous range now interpolate
  existing FFT bins forward instead of clearing them, eliminating the
  spectrum flash/blank during pinch / wheel / drag zoom.

**TGXL detected via direct TCP only (#2250, chrisb1964)**
- TUN applet now appears for TGXLs that don't report through the
  amplifier API (Flex-8600 fw 4.2.18).  New `m_directPresence` fallback
  in `TunerModel`; `isPresent()` returns true via either path.
- Handles bare `amplifier <handle> removed` form correctly via a new
  `ampRemovedRe` regex (matches FlexLib's `s.Contains("removed")`
  semantics for both bare-flag and kvs-form removals).

**RX slice tab capacity initialization (#2243, chibondking)**
- RX slice tab capacity is now seeded from the radio model on connect
  rather than left at the default — fixes empty tabs when reconnecting
  to a different radio model.

**Radio model label refresh (#2244, jensenpat)**
- Radio model label refreshes on `info` command response, so
  reconnecting to a different model immediately reflects the new
  identity in the UI.

**FreeDV Reporter OS string (#2269, tmiw)**
- OS field lowercased to match the official FreeDV client convention.
  Previously AetherSDR users showed as "other" in monthly platform
  reports.  Thanks Mooneer.

**FreeDV Reporter checkbox visibility (#2268, NF0T)**
- Three checkboxes in the FreeDV Reporter tab were rendering with no
  visible borders against the dark background.  Applied the standard
  `ConnectionPanel` checkbox style.

**AppletPanel scrollbar (#2257, Chaosuk97)**
- AppletPanel scrollbar widened from 6 px to 12 px with a dim handle
  at rest that brightens to `#4a6880` on hover or drag.  500 ms
  delay before dimming back so quick scroll gestures don't flicker.
  Always-visible track to match the rest of the app.

**Sweep 1 — 7 small UI / contrast fixes (#2270)**
- `AetherSDR vX.Y.Z` shown again in the in-app title bar — restored
  after the frameless-window mode regression (#2027).
- FlexControl knob press tokens are bare `S` / `C` / `L`, not
  `X4S` / `X4C` / `X4L`.  Captured against LB2EG's hardware (#2263).
- Å Morse sequence corrected to `01101` (`·−−·−`) per ITU-R M.1677-1
  (#2264, LB2EG).
- Single-click-to-tune suppressed across the entire K / SFI / WNB /
  RF Gain / WIDE indicator strip in the top-right of the spectrum,
  not just the propagation text (#1564).
- Slice record (⏺) and play (▶) indicator alpha values doubled for
  legibility against the dark spectrum background (#1576).
- Status indicators (TNF / CWX / DVK / FDX) unified to cyan
  (`#00b4d8`) for active and dark grey (`#404858`) for off; TNF init
  colour bug fixed (was semi-transparent white) (#1581).
- Status bar time stack: removed grid-square label; date + UTC time
  use the same 2-row layout as every other telemetry stack (#1583).

**Sweep 2 — 5 small bug fixes (#2272)**
- Double-clicking an off-screen slice indicator (`<A` / `A>`) recenters
  cleanly without retuning to a wrong frequency (#2237).  Qt's
  press → release → double-click → release sequence was leaving
  `m_spotClickConsumed=false` on the trailing release, so the
  single-click-to-tune path fired against the new center.
- HAVE_RADE without HAVE_WEBSOCKETS compile error (#2204) —
  `startFreeDvReporting` / `stopFreeDvReporting` now wrap their
  bodies with `#ifndef HAVE_WEBSOCKETS` no-op fallback.
- (See "Features" above for #1846 spot mode, #1993 ATU toggle, #2259
  custom filter edges.)

## [v0.9.4] — 2026-05-01

### AetherSweep, ShackSwitch, and multi-client startup hardening

A heavy community-contribution release.  Headline feature is **AetherSweep**
(jensenpat) — an in-panadapter SWR sweep analyzer that walks the current TX
band, plots SWR live on the spectrum surface, and handles TGXL bypass +
external-amplifier safety.  **ShackSwitch** support (nigelfenton) lands as a
new Peripherals tab + dedicated applet, integrating any builder's open-source
Arduino antenna switch over the Antenna Genius protocol.

A trio of substantial reliability fixes from jensenpat addresses long-standing
multi-client startup issues — when SmartSDR or DAX is already connected,
AetherSDR's panadapter creation, slice ownership, and PC audio stream
acquisition now all hold up under the racey status interleavings the
ShackSwitch and DAX users were hitting.  Plus a focused mix of platform
fixes: macOS DMG dark-theme on pop-out windows (Chaosuk97), Windows
without-Qt6::SerialPort build (NF0T), NRL gating correctness on 6000-series
radios.

### Features

**AetherSweep — in-panadapter SWR analyzer (#2202, #2220, #2230, jensenpat)**
- New Start/Clear Sweep buttons under the ANT slice menu, plus a 1-10 W
  sweep-power slider with cross-panel sync.  Persisted as
  `SwrSweepPowerWatts` and defaults to 1 W.
- Walks the current TX band stepping a tune carrier in 20 kHz increments
  with edge guards, sampling fresh SWR + forward-power meter data per step,
  and overlays the curve on the panadapter directly under the slice flag.
- Per-band-edge guard, max-260-points cap, and 60 m channelized-band
  refusal.  Refuses to start when split is active, when transmitting,
  when the band is wider than the radio's max pan width, or when a PGXL
  amplifier is in OPERATE mode (forces user to STANDBY first).
- ⚠️  **Third-party amplifiers are not auto-detected.** AetherSweep
  only knows how to put a Power Genius XL into standby — it has no way
  to talk to ACOM, SPE, Elecraft KPA, OM Power, or other linear amps.
  **If you have a non-PGXL amplifier, manually place it in BYPASS or
  STANDBY before starting an SWR sweep.**  The sweep will run a tune
  carrier through whatever path the radio sees, and a non-bypassed
  external linear will amplify that carrier into your antenna.
- Full TGXL handling: snapshots OPERATE/BYPASS state, places TGXL into
  BYPASS to read raw antenna SWR, restores original state on completion
  or abort.  Reads radio-side SWR while TGXL is bypassed (TGXL stops
  emitting RL meter packets in bypass — #2229).
- 5-phase state machine with explicit timeouts: WaitingForTgxlBypass,
  TgxlBypassSettle, Sweeping, StoppingTune, RestoringTgxl.
- Esc to abort.  Inputs locked during sweep so the user can't accidentally
  retune mid-pass.  Disconnect mid-sweep cleanly stops the carrier and
  releases TGXL.
- Optimistic `setTunePower()` update so the sweep's chosen power lands
  immediately rather than racing the radio's status echo.
- Sweep result label shows source — `RADIO` for direct measurement,
  `TGXL BYPASS` for tuner-bypassed measurement.

**ShackSwitch antenna switch integration (#2214, #2227, nigelfenton)**
- New **Peripherals** tab in Radio Setup with auto-discovery and manual-IP
  connect for ShackSwitch devices via the Antenna Genius (AG) UDP/TCP
  protocol on port 9007.
- New **ShackSwitchApplet** — compact panel with up to 8 labelled
  antenna-port buttons, click-to-switch, active-port highlight.  SO2R
  dual-radio mode shows Input A / Input B side-by-side with conflict
  detection.  Single-radio mode (4-port R4 hardware) hides Input B
  automatically.
- Dummy-load / deselect (clicking the active port deselects it) and
  per-port labels driven by the device's own configuration.
- Integration is invisible to users without ShackSwitch hardware —
  detected by the `name="ShackSwitch"` field in the AG broadcast beacon.
- Web UI launcher button opens the device's local web interface.
- Reference hardware: ShackSwitch v2.0 (Arduino Uno Q, SO2R, 8-port) and
  ShackSwitch R4 (Arduino Uno R4 WiFi, single-radio, 4-port).

### Bug fixes

**Multi-client startup panadapter creation (#2222, jensenpat)**
- AetherSDR's startup pan creation now holds up cleanly when SmartSDR,
  DAX, or another GUI/audio client is already connected.  The radio
  replays status for all current pans/slices/streams on connect; without
  this fix, AetherSDR could correctly reject other clients' objects but
  then fail to instantiate its own `PanadapterModel` if ownership status
  arrived out of order.
- Adds `display panafall create x=100 y=100` (FlexLib v4.2.18 syntax)
  with capability-based fallback to legacy `panadapter create`.
- New `ensureOwnedPanadapter()` factory; deferred-status replay queue
  for `display pan` frames that arrive without `client_handle`; waterfall
  ordering recovery via `panadapter=...` parent ID lookup.
- Routes failure cases through the existing `panadapterLimitReached` /
  `sliceCreateFailed` status-bar signals so radio resource exhaustion
  is visible instead of looking like a startup hang.

**PC audio remote stream ownership (#2226, jensenpat)**
- Fixes PC Audio failure when SmartSDR / DAX is already running — the
  shared `remote_audio_rx` pipe was being silently removed because
  AetherSDR was parsing the `stream create` response body as decimal
  instead of hex.  `"4000009"` (no `0x` prefix) was becoming
  `0x003D0F09` instead of `0x04000009`, so the create-response stream
  didn't match the status-reported stream and AetherSDR removed the
  real one.
- Extracts a new `RadioStatusOwnership` helper (header-only, fully
  unit-tested) that handles ownership decisions for both panadapters
  and remote audio streams — defer when no `client_handle`, claim
  when ours, ignore when another client's.
- Stream-acquisition state machine now tracks create-pending,
  remove-requested, and adopted-from-status separately so toggling
  PC Audio doesn't race the create response.
- Adds `radio_status_ownership_test` with 25 assertions covering all
  the ownership decisions and the headline parse bug.
- Fixes #2037, #1418, #1473.

**TGXL meter goes silent during bypass (#2229, #2230)**
- AetherSweep aborted with "no fresh TGXL SWR meter data" whenever the
  TGXL was in OPERATE before sweep start.  The TGXL stops emitting `RL`
  (return-loss) meter packets while in BYPASS — bypass relays are
  passive wire-through, no measurement engine.
- Switches the meter source to RADIO while the TGXL is bypassed; the
  radio's own SWR coupler measures the same physical signal through
  the bypassed relays.  UI label still shows `TGXL BYPASS` since
  that's what describes the configuration.

**Pop-out applet panel white background on macOS (#2190, Chaosuk97)**
- macOS DMG builds rendered floating windows with a white background
  even though the dark-theme stylesheet was applied — the CI-built
  `libqcocoa.dylib` enforced `Qt::WA_StyledBackground` more strictly
  than Homebrew's Qt 6.11.0 build.
- Adds `setAttribute(Qt::WA_StyledBackground, true)` on all three
  floating-window classes (`FloatingContainerWindow`, `PanFloatingWindow`,
  `MainWindow::floatAppletPanel`).  Cross-platform safe: the attribute
  is harmless on Linux/Windows where it was a no-op, and now the
  pop-out applet panel correctly themed on every platform (was
  defaulting to system theme on Linux/Windows too — latent bug fixed
  as a side effect).

**Windows build without Qt6::SerialPort (#2195, NF0T)**
- Compile error introduced by #2147: the `<QElapsedTimer>` include was
  guarded by `#ifdef HAVE_SERIALPORT` while `m_debounceTimer` is declared
  under `#if defined(HAVE_SERIALPORT) || defined(Q_OS_WIN)`.  Mismatch
  broke the Windows-without-SerialPort build configuration.  Widens the
  include guard to match.

**NRL DSP filter visible on 6000-series radios (#2219)**
- Fixes regression from #2184 where NRL was incorrectly grouped with
  the 8000-series-only firmware DSP filters (NRS, RNN, NRF).  NRL is
  available on 6000-series too — only NRS/RNN/NRF require BigBend /
  DragonFire hardware.  Fixes #2198.

### Acknowledgements

Massive contributor batch this cycle:

- **jensenpat** — AetherSweep (3 PRs: feature + power-control polish + TGXL
  meter fix), multi-client panadapter startup, PC audio ownership.  This
  release wouldn't be the leap it is without his work.
- **nigelfenton** — ShackSwitch integration (3 PRs across the cycle:
  protocol fix, full integration, callsign-detection cleanup).
- **NF0T** — Windows build fix.
- **Chaosuk97** — macOS DMG dark-theme fix.

## [v0.9.3] — 2026-04-30

### External APD, FreeDV Reporter, Slice Colors, and v4.2 firmware updater

A broad release.  Headline feature is **External APD** support for
SmartSDR firmware 4.2.18, which lets the radio sample its outgoing
RF from a coupled feedback path on one of the RX/XVTR inputs so the
predistortion engine trains against the actual transmitted signal —
required when a FLEX-8x00 drives an external linear amplifier.
**FreeDV Reporter** station reporting (NF0T) lands the long-requested
RADE-integrated reporting flow.  **chibondking** ships customizable
slice colors plus two serial / pop-out fixes.  AetherSDR's firmware
updater is rewritten to extract `.ssdr` files natively from
FlexRadio's v4.2+ MSI installer (no bundled tools needed), and the
NAVTEX data-layer plumbing lands ahead of the upcoming applet.

A long tail of UI polish and Windows-platform fixes also lands —
TX→RX waterfall continuity, scroll-wheel debounce, 48 kHz audio
sink on Windows, applet pop-out persistence, and floating-window
dark theming.

### Features

**External APD (#2187)**
- New "APD" tab in Radio Setup with per-TX-antenna external sampler
  selection (ANT1 / ANT2 / XVTA / XVTB → INTERNAL / RX_A / RX_B /
  XVTA / XVTB).  Tab is hidden unless the radio reports `apd
  configurable=1`, so it stays invisible on 6000-series radios and
  pre-4.2.18 firmware.
- Equalizer Reset button issues `apd reset` to clear all per-antenna
  training data.
- Protocol additions: `apd sampler` sub-object parsing (with
  fallback-to-INTERNAL for invalid `selected_sampler`), bare
  `equalizer_reset` flag handling, and the matching `setApdSamplerPort`
  command path on `TransmitModel`.
- Cross-checked against `Flex.Smoothlake.FlexLib` v4.2.18 source.

**FreeDV Reporter station reporting with RADE integration (#2173, NF0T)**
- Adds FreeDV Reporter (https://qso.freedv.org) station-reporting
  support driven by the RADE modem's sync / SNR / freq-offset events.
- New connection toggle in the DX Cluster dialog plus per-slice
  reporting toggle that mirrors RADE engine state to the Reporter
  WebSocket session.
- Builds opportunistically — without `Qt6::WebSockets` or `librade`,
  the toggles silently become no-ops.

**Customizable slice colors (#2155, chibondking)**
- Per-slice color selection through a new `SliceColorManager`
  singleton.  Color assignments persist across sessions and are
  visible in VFO widgets, panadapter overlays, and meter strips.

**Native MSI firmware installer support (#2169)**
- The firmware updater can now extract `.ssdr` files directly from
  FlexRadio's v4.2.18+ MSI installer (which switched from PE/COFF
  self-extracting `.exe` to WiX 6 MSI).  Vendored `libmspack`
  (LGPL-2.1) handles LZX-compressed CABs; a small OLE Compound File
  reader pulls the embedded CAB from the MSI envelope.
- "Browse .ssdr" → "Select Installer..." now accepts `.msi`, `.exe`,
  and `.ssdr`.  No external tools required (no `7z`, no MSI runtime).
- Format auto-detection on the first 8 bytes (OLE magic vs. PE/COFF MZ).

**NAVTEX data-layer plumbing (#2186)**
- New `NavtexModel` covers the SmartSDR `navtex` waveform protocol —
  per-message Pending → Queued → Sent / Error state, status parsing
  for `navtex` and `navtex sent`, and a `navtex send` command path
  with proper quote/backslash escaping for `msg_text`.
- Foundation for the upcoming NAVTEX applet UI; data layer ships
  first so other clients (TCI, scripting) can already publish NAVTEX
  traffic.
- 21-assertion unit test covers escaping, idempotency, error paths.

**CWX active tracking (#2181)**
- `RadioModel` tracks CWX send state so the audio gate stays open
  during long character sends.

**Floating-window dark theme (#2096, AetherClaude)**
- Pop-out panadapter and floating-container windows now inherit the
  full dark theme via a new `Theme.h` shared stylesheet, eliminating
  flash-of-light-theme on window construction.

**Center active VFO when zooming in from keyboard (#2183, jensenpat)**
- The keyboard zoom-in shortcut now centers the active VFO in the
  viewport (matches the mouse-wheel zoom behavior).

### Bug fixes

**TX→RX waterfall continuity (#2171, #2182)**
- Force FFT-fallback rendering on TX→RX transition to prevent a
  visible gap in the waterfall (#2171).
- Blank waterfall rows for 400 ms after TX ends so any residual
  hardware tail doesn't paint over the noise floor (#2182).

**Hide 8000-series DSP filters on 6000-series radios (#2184)**
- `NRL`, `NRS`, `RNN`, `NRF` filters are FLEX-8x00-only.  6000-series
  radios no longer show greyed-out controls for filters they can't
  use.

**Stream-status helper consolidation (#2145)**
- Deduplicates the per-stream-type status helpers.  Annotates
  `m_daxTxClientHandle` and documents the TCI receiver-index policy
  for future contributors.

**TCP close handshake before socket destroy (#2113)**
- Wait for `disconnected()` before tearing down the QTcpSocket on
  user-initiated disconnect.  Fixes occasional "connection reset"
  reports on the radio side.

**rigctld short-form split direction (#2111)**
- `S` (set split) and `s` (get split) were transposed in the rigctld
  mapping table, so external loggers couldn't read or set split.

**CW sidetone on Windows (#2105)**
- `startSidetoneStream()` was never called on Windows because the
  audio backend init order put it after the first key event.  CW
  sidetone now starts immediately on connect.

**48 kHz RX audio sink on Windows (#2123)**
- Prefer 48 kHz over 24 kHz on Windows where WASAPI's resampler
  introduces audible high-frequency artifacts at 24 kHz.

**Scrollbar styling on applet panels (#2088)**
- The applet-panel scroll area was inheriting Qt's default thin grey
  scrollbar; restyles to the dark-theme bar.

**USB mic level gauge on connect (#2086)**
- The USB mic-level gauge wasn't drawn when connecting with `mic
  source = PC`; gauge now appears immediately.

**VOX phoneStateChanged on keyboard shortcut (#2084)**
- VOX setters didn't emit `phoneStateChanged()`, so the UI didn't
  refresh when VOX was toggled via keyboard shortcut.

**Scroll-wheel debounce (#2151)**
- Debounces high-frequency `pixelDelta` scroll events from
  precision-scroll mice in `VfoWidget` and `SpectrumWidget`.

**Marker-width settings cleanup (#2156, mvanhorn)**
- `Slice<N>_MarkerThin` setting key is now removed after migration
  to `Slice<N>_MarkerWidth` instead of being left in place.

**Serial PTT — Win32 WaitCommEvent path (#2147, chibondking)**
- FTDI VCP drivers only refresh DSR/CTS in the completion of a
  `WaitCommEvent` call; AetherSDR's polling-based detection missed
  edges on Windows.  Adds a Win32 native event-wait path on top of
  Qt's serial port for DSR detection.

**Applet pop-out persistence race (#2154, chibondking)**
- Floating applet windows were sometimes restored at the wrong
  position because the geometry-save signal raced the close event.
  Systemic fix wires save before destruction.

**Windows build guard for FreeDV Reporter (#2186)**
- The FreeDV Reporter `freedvReportingToggled` connect-lambda used
  RADE-guarded symbols inside an `#ifdef HAVE_WEBSOCKETS` block.
  Windows has WebSockets but no RADE → undefined-identifier compile
  errors.  Added the missing `HAVE_RADE` inner guard.

### Docs

- **Data-modes help refresh (#1939)** — replaces lingering "DIGI
  applet" references with the current independent-tile UI vocabulary.
- **DIGI applet scrub (#2179)** — sweeps the remaining stale
  references in tooltips and dialog text.

### Acknowledgements

Thanks to **NF0T** for the FreeDV Reporter station-reporting
integration; **chibondking** for slice colors, the serial PTT
WaitCommEvent fix, and the applet pop-out race; **jensenpat** for
the keyboard zoom-center shortcut; **mvanhorn** for the marker-key
cleanup; and **AetherClaude** for the bulk of the long-tail bug
fixes.

## [v0.9.2.1] — 2026-04-29

### TGXL direct autotune for firmware 4.2 compatibility

Hotfix release.  After upgrading to SmartSDR firmware 4.2, multiple
users on both AetherSDR and SmartSDR report the TUNE button no longer
works on the 4O3A Tuner Genius XL.  The radio's `tgxl autotune` command
path was reworked in 4.2 firmware and broke for many configurations.

This release routes the TUNE button through the TGXL's native
port-9010 channel when a direct TGXL connection is configured,
bypassing the affected firmware path.  Users without a direct TGXL
connection are unaffected by this change and should still configure
direct connection in Radio Setup → Tuner to recover TUNE.

### Bug Fixes

**TGXL TUNE works again on firmware 4.2 (#2163)**
- When a direct TGXL connection (port 9010) is configured, the TUNE
  button now sends the native `autotune` command directly to the TGXL
  instead of routing `tgxl autotune handle=<H>` through the radio's
  command channel.  Falls back to the radio path when no direct
  connection is available.
- The TGXL drives radio PTT via its hardware interlock cable when it
  receives the native `autotune` command, so client-side keying is
  not required.  Existing tuning state and SWR readout in the Tuner
  applet are unchanged.

**Log redaction no longer mangles 4-component version strings**
- The PII redactor's IPv4 regex matched anything that looked like
  four dot-separated 1-3-digit numbers, so the application's own
  version string `0.9.2.1` was being redacted to `*.*.*. 1` in
  logs and support bundles.  Negative lookbehind/lookahead now skip
  quoted (`"0.9.2.1"`) and v-prefixed (`v0.9.2.1`) version forms
  while still redacting bare IPs in log output.

## [v0.9.2] — 2026-04-28

### WAVE Phase 2, v4.2.18 firmware support, and community polish

A focused community-driven release.  Major work from **jensenpat** lands
the WAVE Phase 2 applet (four visualizations, settings drawer), proper
TCXO frequency-offset calibration, and the DAX/TCI multi-stream routing
needed for FlexRadio firmware 4.2.18.  **chibondking** and **NF0T**
ship a serial PTT fix and an r8b heap-corruption crash fix.  The VFO
DSP panel gets a UX pass (collapsed Marker + Filter Edge buttons), and
the v4.2.18 discovery beacon is parsed.

### Features

**WAVE Phase 2 — applet visualization controls (#2124, jensenpat)**
- Double-click the WAVE waveform to open a settings drawer (replaces
  the prior clear-on-double-click behavior).  Drawer is open by default
  on first launch for discoverability.
- New compact `View` dropdown with four visualizations:
  Scope (the original waveform trace), Envelope (filled RMS with peak
  outline), History (side-scrolling activity bars), Bands (vertical
  EQ-style frequency bands).
- Persisted Zoom and FPS controls in the drawer.  Zoom is amplitude
  gain across all views, not a time-window preference.
- Drawer participates in the applet size hint when expanded so the
  applet stays compact when collapsed.

**DAX-aware TCI multi-stream routing (#2140, jensenpat)**
- Adapts the DAX/TCI audio path for FlexRadio firmware 4.2.18's
  explicit stream-ownership reporting.  Filters DAX RX/IQ stream
  status by `client_handle` so we don't accidentally register another
  client's stream, while accepting ownership-less status from older
  firmware.
- Advertises TCI receivers as contiguous owned-slice indexes
  (`0..N-1`) rather than raw Flex slice IDs.  Fixes WSJT-X TCI1/TCI2
  multi-slice operation when this client owns slice 1 but another
  client owns slice 0.
- Honors per-client `audio_start:<receiver>` so multi-slice WSJT-X
  receives only the intended audio.
- Stream-removal handling: unregisters DAX/IQ streams and clears TCI
  DAX placeholders on `removed` status.
- Adds focused diagnostics for first DAX VITA packets, TCI receiver
  maps, DAX RX delivery, and DAX TX route selection — gated behind
  `lcDax` / `lcCat` categories so default users see no extra noise.

**TCXO frequency-offset calibration (#2119, jensenpat)**
- Replaces the unsupported `radio calibrate` command (firmware
  v4.1.5 returns `0x50000016` "unknown command") with the documented
  SmartSDR / FlexLib sequence: `radio set cal_freq=`, `radio set
  freq_error_ppb=0`, `radio pll_start`.
- Watches `pll_done` status for completion with run-specific guards
  against stale events from a prior calibration firing during a new
  run.
- Cross-checked against FlexLib v2.10.1 source.  Fixes #1237, #2095.

**VFO marker controls — tri-state Marker, single Filter Edge (#2141)**
- The four-button row (Thin / Thick / Edges / Hide) collapses to two:
  - **Marker** cycles `Off → 1 px → 3 px` on click.  Off skips both
    the center line and the top triangle, leaving only the passband
    bracket.
  - **Filter Edge** is a checkable on/off — checked = edges shown.
- New `Slice<N>_MarkerWidth` int settings key with one-shot migration
  from the old `Slice<N>_MarkerThin` bool (`True` → 1, `False` → 3).

**v4.2.18 discovery beacon parsing (#2138, AetherClaude)**
- Parses two new fields from the FlexLib v4.2.18 discovery packet:
  `is_system_model` (flags bench / system-build radios) and
  `turf_region` (turf region indicator distinct from the existing
  `region` field).  `turf_region` is shown in the connect dialog
  detail line.

**Other**
- ATU start added to the keyboard shortcut actions list.
- Clear action added to the CW decode window context menu (#2116).

### Bug fixes

**Resampler heap corruption (#2114, NF0T)**
- `r8b::CDSPResampler24::process()` doesn't bounds-check its `l`
  parameter; passing a block larger than the constructor's
  `aMaxInLen` silently overflows internal filter buffers.  Bug
  manifested as a crash when the Qt event loop stalled long enough
  for `QAudioSource::readAll()` to return more than 4096 frames in
  one call (typical during DX-cluster spot bursts or heavy UI
  redraws).  Fix: chunk-and-recurse on each `process*()` path so each
  call stays within the configured limit.

**Serial PTT input non-functional (#2125, chibondking)**
- Three layered bugs blocked the serial-PTT path:
  1. No way to open the port without restarting AetherSDR — the
     Serial tab had no Open / Close buttons and the auto-open label
     implied a restart was required.
  2. `loadSettings()` defaulted CTS / DSR polarity to `ActiveLow`
     while the dialog combo defaulted to "Active High".  Users who
     left polarity at the displayed default got silently inverted
     logic — footswitch press read as inactive, PTT never fired.
  3. `updatePolling()` was missed when the port was opened, so the
     polling loop didn't pick up the configured input function.
- Adds Open / Close buttons to the Serial tab, fixes the polarity
  default mismatch, and tracks the explicit Open state separately
  from auto-open via a new `SerialPortOpen` setting.

**Slice audio loss after band changes (#2128, jensenpat)**
- After `display pan set band=`, the radio sometimes stops mixing
  slice audio without echoing `audio_mute=1`, leaving the model in a
  "unmuted but silent" state.  Fix: 300 ms after the band-change
  command, reassert `slice set <N> audio_mute=0` for any unmuted
  slice on that pan.

**CWX Live toggle and Send action (#2122, jensenpat)**
- `Live` button is now a true toggle so operators can turn live
  keying back off (was force-on only).
- `Send` button now submits the current input when Live is off
  (matching Enter-key behavior).
- Setup exits Live cleanly without duplicate-sending text already
  keyed character-by-character.
- Adds `cwx_panel_test` covering Live toggling, Send-click
  submission, Enter submission, and Live exit safety.

**Connect-radio dialog grouping polish (#2121, jensenpat)**
- Scopes the `QFrame` callout stylesheet so child labels and
  checkboxes don't inherit the panel border / background, fixing
  nested-bordered-box rendering on macOS, Windows, and Linux.
- Tightens the SmartLink Remote radio list height and moves the
  Connect Remote button outside the Remote group.

### Internal

- `bool m_markerThin` → `int m_markerWidth` (0 / 1 / 3) propagated
  through `VfoWidget`, `SpectrumWidget::SliceOverlay`, and the
  `markerStyleChanged` signal.  Render path skips both the center
  line and the top triangle when `markerWidth == 0`.

### Acknowledgements

Thanks to **jensenpat** for the WAVE Phase 2, DAX/TCI, calibration,
band-change, CWX, and dialog polish work; **chibondking** for the
serial PTT triple-fix; and **NF0T** for catching the r8b heap-
corruption bug.

## [v0.9.1] — 2026-04-27

### Local iambic CW keyer, unified sidetone controls, and CW transmit reliability

A focused follow-up to v0.9.0.  The headline feature is a software
iambic keyer that turns any MIDI / serial paddle into a sub-5 ms
sidetone source via the new PortAudio backend (issue #2079) — the
radio still produces the on-air signal, but the local sidetone
gate fires the moment the paddle moves instead of waiting for the
radio's keyed-back signal.  Three latent bugs in the netcw protocol
path that prevented CW from ever transmitting on FLEX-8600 v4.1.5
firmware are also fixed.  Plus the CW panel collapses three sidetone
widget groups into one set of controls and finally wires up the L/R
pan slider that's been a placeholder since the panel was first added.

### Features

**Local iambic keyer for sub-5 ms paddle sidetone (#2079)**
- Software iambic state machine that runs alongside the radio's RF
  iambic engine.  Both engines see the same paddle inputs at the same
  WPM and produce identical Morse timing — but the local keyer drives
  the sidetone gate directly, avoiding the 50–200 ms round-trip
  through the radio's keyed-back signal.
- Modes A and B implemented (Ultimatic / Bug / Straight follow in a
  later phase).  Hooked into both MIDI Gate params (`cw.dit`,
  `cw.dah`) and serial paddle paths (DTR/CTS via SerialPortController).
- Driven by the existing radio Iambic toggle — no new UI clutter.
  The keyer mirrors the radio's iambic state, mode, and WPM via the
  TransmitModel `phoneStateChanged` signal.
- Dedicated worker thread with `std::chrono::steady_clock` timing
  and a lock-free atomic key gate on the audio side.  9 unit tests
  covering single dit/dah timing, squeeze alternation, inter-element
  gap, mode A release behaviour, WPM scaling, paddle swap, idempotent
  start, and idle behaviour.

**Sidetone controls unified (#2079)**
- The CW panel previously had three separate sidetone widget groups:
  the radio's "Sidetone" toggle/volume, a "Local STn" toggle/volume
  for the local PortAudio sidetone, and a "Follow" pitch row with a
  manual override slider.  All three are now collapsed into the
  single existing **Sidetone** button, which drives both engines in
  lockstep.  The volume slider drives `mon_gain_cw` on the radio and
  the local sidetone identically.  Pitch always follows the radio's
  `cw_pitch`.

**CW pan slider wired up (#2079)**
- The L/R pan slider in the CW panel was a dead UI element with a
  TODO comment.  Now drives both the radio's `mon_pan_cw` (radio-side
  sidetone pan within the RX audio stream) and the local sidetone
  with constant-power pan law (cosine/sine for equal-loudness sweep,
  no center dip).  Double-click on the slider recenters to 50.

**Slice capacity notification (#48)**
- Status-bar warning when adding another panadapter would exceed the
  radio's slice limit.  Three guard points cover the pre-flight check
  in the layout dialog, the runtime check in `applyPanLayout`, and
  the async fallback when the radio rejects a `panafall create`.
  Includes the radio model name and slice count in the message.

**PortAudio sidetone via JACK on Linux (#2075 follow-up)**
- The PortAudio sidetone backend (introduced in v0.9.0) now prefers
  the JACK host API on Linux when available, with `paFramesPerBuffer
  = 128` + `suggestedLatency = 0` for sub-5 ms quantum.  PipeWire's
  ALSA shim silently breaks callback-mode streams on some setups
  (`Pa_StartStream` returns success but the audio thread never
  schedules the callback); pipewire-jack delivers reliable callbacks
  at the device's native sample rate.  48 kHz is now the universal
  preference, with `Pa_IsFormatSupported` guarding fallback.

### Bug fixes

**CW transmit: invalid paddle command form**
- `RadioModel::sendCwPaddle` was emitting `cw key 1 0` (a 2-arg paddle
  form) which the radio's protocol does not accept — FlexLib only
  ever sends single-state `cw key 1` or `cw key 0` and expects the
  client to do iambic timing locally.  The 2-arg form was silently
  dropped, so paddle keying produced no RF.  Now collapses to a
  straight-key form when the local iambic keyer isn't running, or
  routes through the new `sendCwKeyEdge` + `sendCwPtt` primitives
  when it is.

**CW transmit: lowercase hex in netcw payload**
- FlexLib formats `time=0x...` and `client_handle=0x...` with C#
  `ToString("X")` (uppercase), and the radio's status messages do
  too (e.g. `S23A59BDF|...`).  Firmware v4.1.5's netcw parser is
  case-sensitive on these fields and silently dropped lowercase
  packets.  Now formatted explicitly uppercase.

**CW transmit: dead TCP fallback**
- The post-UDP TCP fallback was sending the netcw-decorated form
  (`cw key 1 time=0x... index=... client_handle=0x...`) which the
  radio rejects with `0x50001000` ("command syntax error") on TCP.
  Removed when the netcw stream is up; the no-netcw fallback (for
  firmware that doesn't support netcw stream creation) is preserved
  separately.

**Optimistic updates for CW model setters**
- `setCwIambic`, `setCwIambicMode`, `setCwSpeed`, and `setCwPitch`
  on `TransmitModel` previously sent the command to the radio
  without updating local state, on the assumption that the radio
  would echo the new value back.  Firmware v1.4.0.0 doesn't reliably
  echo iambic flags, WPM, or pitch in transmit status messages, so
  any code reading these properties after a UI toggle saw stale
  values until the next periodic transmit status arrived (or never).
  Now follow the same optimistic-update pattern as `setCwBreakIn`.

**Block on graceful disconnect (#1996, openstreem)**
- `RadioModel::disconnectFromRadio` now uses `Qt::BlockingQueued
  Connection` for both the `gracefulDisconnect` lambda and the
  fallback `disconnectFromRadio` call, matching the destructor's
  pattern.  Without this, the queued work could be cancelled before
  it ran during app teardown — leaving the radio with a stale
  session that required a power cycle.

**Memory recall: restore repeater offset and tone (#1871, #1965, jensenpat)**
- Recalled FM repeater memories properly restore `repeater_offset_dir`,
  `fm_repeater_offset_freq`, derived `tx_offset_freq`, and CTCSS tone
  state.  Previously, switching from a repeater memory to simplex
  could leave stale TX offset state on the slice.

**`m_activeTxSlice` initializer mismatch (#2076)**
- Default-init was `0` but `clear()` sets to `-1`.  Now both default
  to `-1` so a fresh `MeterModel` and a cleared one have identical
  `activeTxSlice()` state.

**Per-slice compression meter resolution (#2073)**
- TX-chain `COMPPEAK`, `AFTEREQ`, and `SC_MIC` meters are now
  resolved per active slice instead of last-match-wins, fixing
  Multi-Flex setups where the wrong slice's compression value was
  surfaced in the UI.

**ContainerWidget restoreState displaces children**
- `restoreState` re-inserted children at indices 0..N–1 even when
  they were already in the layout, displacing non-saved children
  like the CHAIN widget.  `insertChildWidget` is now a no-op when
  the child is already present.

### Build and CI

- `M_PI_2` replaced with a local `kPiOver2` constexpr in
  `CwSidetoneGenerator.cpp` so the Windows MSVC build doesn't
  fail (`<cmath>` doesn't define `M_PI_2` without `_USE_MATH_DEFINES`).

### Acknowledgements

Thanks to the operators who tested CW workflows on real hardware
and surfaced the netcw protocol issues that had been silent since
the netcw backend landed, and to **jensenpat** and **openstreem**
for the community fixes bundled in.

## [v0.9.0] — 2026-04-25

### Aetherial RX Audio Suite, frameless UI, and local CW sidetone

A milestone release. The PooDoo Audio chain — Aetherial Parametric EQ,
AGC-T gate, AGC-C compressor, Tube saturator, and PUDU exciter — now
runs on **both** RX and TX with independent settings, drag-to-reorder
on each path, and matching frameless editor windows.  The whole UI
goes frameless: the main window plus every applet pop-out and editor
gets a Discord-style minimise / maximise / close trio with title-bar
drag.  CW operators get a dedicated low-latency local sidetone sink
that survives CWX too.  Plus community fixes for the FLEX-6000
compression meter, RTTY mark-default reset, RADE decoded audio, and
the usual stack of UX polish.

### Features

**PooDoo Audio RX chain (#1998)**
- Reuses the existing TX DSP modules (Parametric EQ, gate/expander,
  compressor, tube saturator, PUDU exciter) on the RX path with
  fully independent state from TX.  Each stage is a tile in the
  CHAIN widget on the RX side: single-click toggles bypass,
  double-click opens the floating editor.  RX chain order is
  drag-to-reorder.  RADIO and SPEAK status tiles bookend the chain;
  the DSP tile shows whichever client-side noise reducer is active
  (NR2 / NR4 / BNR).
- All applet titles rebrand to **Aetherial**: Parametric EQ,
  AGC-T (gate), AGC-C (compressor), Tube, PUDU.

**Frameless main window with custom title bar (#1926)**
- Main window now uses `Qt::FramelessWindowHint` with a custom
  20 px title bar carrying drag-to-move via `startSystemMove`,
  double-click maximise, and a Discord-style minimise / maximise /
  close trio.  Resize via standard window-edge grip.

**Frameless pop-out windows for applets and panadapters (#1922)**
- Every floating applet and panadapter pop-out gains the same
  frameless title bar with the trio.  Single-click trio actions,
  drag the bar to move, double-click to maximise.

**Frameless editor windows for the entire PooDoo chain (#1998)**
- Aetherial Parametric EQ, AGC-T, AGC-C, Tube, PUDU, and Reverb
  editors all use the same shared title-bar widget with drag and
  trio.  Title text reads "Aetherial &lt;Stage&gt; — &lt;Side&gt;" so the
  TX vs RX instance is identifiable at a glance.

**Polish frameless title bar (#1931)**
- Title-bar trio refined to a Discord-style sequence; dropped the
  lightbulb icon in favour of a minimal arrow accent so the trio
  sits flush against the right edge.

**Local CW sidetone with low-latency sink + CWX support (#1969)**
- Dedicated low-latency local sidetone path that bypasses the
  protocol-level monitor for keying feedback that doesn't fight
  network jitter.  Works for paddle, straight key, and CWX
  generated transmissions.  Pitch and gain follow the existing
  `pitch` / `mon_gain_cw` controls.

**Two-Tone Tune shortcut (#1995, jensenpat)**
- New unassigned `Two-Tone Tune` keyboard action under the TX
  shortcut category.  Sends `transmit set tune_mode=two_tone`
  before starting Tune, toggles off on a second press, and
  restores `tune_mode=single_tone` on stop so the regular TUNE
  press isn't surprised by sticky two-tone state.

**XVTR diagnostic logging (#1964)**
- New `xvtr` logging category captures status messages, RF↔IF
  frequency translation, and pan-bandwidth conversions so XVTR
  setup issues can be diagnosed from log bundles instead of pcap.

**XVTR policy regression tests (#1960)**
- Test harness covers transverter active/inactive state, RF↔IF
  conversion, and the family of pan-recenter cases that used to
  drop the waterfall when crossing IF/RF boundaries.

### Bug fixes

**Flex compression meter derivation across radio families (#1992, jensenpat)**
- FLEX-8000 series exposes `TX/AFTEREQ` at 20 fps as the post-EQ
  reference for compression display; FLEX-6000 captures don't
  expose `AFTEREQ`, so the best matching reference is `TX/SC_MIC`
  at 10 fps with `TX/COMPPEAK` still at 20 fps.  Mixed cadence is
  guarded by a freshness check so a fresh `COMPPEAK` isn't compared
  against a stale `SC_MIC` sample.  When `AFTEREQ` is present it
  takes precedence over `SC_MIC`.

**Preserve rtty_mark_default when radio resets mark on band change (#1968, chibondking)**
- The radio resets `slice rtty_mark` to 2125 in the status broadcast
  that follows a band change, regardless of the configured
  `rtty_mark_default`.  `SliceModel::applyStatus()` was accepting
  the 2125 value blindly, overwriting the user's configured mark.
  Now tracks `m_rttyMarkDefault` (seeded from `RadioModel`) and a
  user-override flag so the correct default is pushed back when
  the radio resets, without fighting an intentional 2125 selection.

**Fix choppy/harsh RADE decoded audio: dedicated buffer + sample-wise mix (#1953, NF0T)**
- RADE decoded audio was sharing the SSB output buffer, producing
  glitches and harsh artefacts on decoded voice.  Split into a
  dedicated buffer with per-sample mixing into the speaker output
  so RADE and SSB don't fight for the same write window.

**Fix CoreMIDI init crash during MIDI auto-connect (#1949, jensenpat)**
- macOS CoreMIDI initialisation could crash on launch when
  auto-connect ran before the MIDI client was fully constructed.
  Init order tightened so the client is ready before any
  auto-connect attempt.

**Restore shortcuts after slider nudges (#1952, jensenpat)**
- Shortcut bindings could go stale after the user nudged a slider
  with the keyboard, because focus stayed on the slider and ate
  subsequent shortcut keystrokes.  Focus now returns to the main
  window after a slider nudge.

**Preserve MIDI bindings when saving device settings (#1951, jensenpat)**
- Saving MIDI device settings was overwriting bindings with an
  empty map because the save path read from the wrong source.
  Now the existing binding map is merged into the saved settings.

**Cancel frequency entry with Escape (#1954, jensenpat)**
- The numeric frequency-entry buffer didn't honour Escape, so a
  user who started typing a frequency had to clear the buffer
  manually.  Escape now cancels the entry cleanly.

**Gate XVTR waterfall tile shifts (#1925, jensenpat)**
- Waterfall tile-shift logic continued to fire on XVTR pans even
  when the IF↔RF frequency relationship made the shift meaningless,
  producing visual tearing.  Shifts are now gated on transverter
  state.

**Close all floating windows when main window closes (#1920, chibondking)**
- Floating applet pop-outs and panadapter pop-outs were left as
  orphaned top-level windows when the main window was closed,
  forcing the user to close each one individually.  All
  floating children now close together with the main window.

### Build and CI

**cmake: declare Qt 6.2 minimum (#1962)**
- `startSystemMove` is gated behind Qt 6.2; the build now declares
  the minimum explicitly so older systems get a clean configure
  error rather than a confusing link failure.  Matches Ubuntu
  22.04 LTS shipped Qt.

**ci(windows): switch to Ninja so sccache actually wraps cl.exe (#1963)**
- The Windows CI job was using MSBuild, which sccache can't wrap
  for compilation caching.  Moved to Ninja so the sccache layer
  established in #1913 actually has effect, cutting Windows build
  times further.

**ci: speed up Windows build with sccache + Qt/FFTW caching (#1913)**
- Adds sccache for compiler caching plus GitHub Actions cache
  layers for Qt and FFTW dependencies.  First-run pulls populate
  the cache; subsequent CI runs reuse compiled objects and prebuilt
  third-party artefacts.

**Add /bigobj to MSVC compile options (#1910 → #1911)**
- The compilation unit hit the C1128 section count limit on
  MSVC.  `/bigobj` raises the limit so the unit compiles cleanly
  without splitting the file.

### Dependencies

- Bump `mozilla-actions/sccache-action` from 0.0.6 to 0.0.10 (#1941)
- Bump `actions/cache` from 4 to 5 (#1942)

### Acknowledgements

Massive cycle.  **jensenpat** delivered eight PRs this release —
including the FLEX-6000 compression meter derivation, two-tone
Tune shortcut, XVTR diagnostic logging and regression tests, and
four crash/UX fixes (CoreMIDI init, MIDI binding persistence,
slider-nudge focus, frequency-entry Escape).  **chibondking**
shipped two PRs (RTTY mark-default preservation and the
floating-window close cascade).  Thanks also to **NF0T** for the
RADE decoded-audio dedicated-buffer fix.

73, Jeremy KK7GWY

## [v0.8.22] — 2026-04-25

### Connection visibility plus community polish

A connection-focused point release: new login/disconnect notifications
for remote clients, passive spots mode, find-search in help, plus a
stack of community-reported regression fixes (TCI WSJT-X TX, RADE
silent SSB, spectrum-click edge jumps, MultiFlex PTT).

### Features

**Band-stack auto-save on dwell**
- New "Auto-save dwell" setting in the band-stack ⚙ menu (Off / 10 s
  / 30 s / 60 s, default Off).  When enabled, the active slice is
  automatically added to the band stack after dwelling on a frequency
  for the configured time.  Skips entries within ±100 Hz of an
  existing bookmark (so you don't re-stack the same station), skips
  during transmit, and waits 5 s after connect before the first
  auto-save.  Caps auto-saved entries at **5 per ham band** —
  overflow drops the oldest auto-saved entry in that band, but
  manually-added bookmarks are never displaced.  Pairs naturally
  with the existing Auto-expiry option for a self-pruning rolling
  history.

**LAN and SmartLink client login notifications (#1892, jensenpat)**
- New connection-event notifications when remote LAN, SmartLink, and
  Maestro clients log in to the radio.  Surfaced as toast-style
  banners in the status bar so operators see Multi-Flex / shared-radio
  activity without watching the log.

**Remote client disconnect flows (#1889, jensenpat)**
- Disconnect remote LAN, SmartLink, and Maestro clients from inside
  AetherSDR.  Operator confirmation prompt + clean protocol teardown
  so the disconnected client sees a graceful exit rather than a stale
  session.

**Passive Spots mode (#1897, jensenpat)**
- New spots mode that ingests spot data without sending any commands
  to the radio — useful for read-only / observer scenarios where you
  want spot decorations on the spectrum but don't want the spot
  source to influence radio state.

**Device details in Slice Troubleshooter (#1898, jensenpat)**
- Slice Troubleshooter now reports audio and control device details
  alongside the existing slice diagnostics so support bundle reviewers
  can see the full I/O path from radio to host without a separate
  step.  Device-ID values are SHA-256 fingerprinted for privacy.

**Find search in help dialogs (#1899, jensenpat)**
- Help dialogs gain Ctrl+F find with case-insensitive search, F3 for
  Find Next, and Shift+F3 for Find Previous — the same shortcut
  behaviour users expect from any text viewer.

### Bug fixes

**Click-to-tune no longer recentres for clicks inside the visible spectrum (#1906, #1907)**
- Click-to-tune was triggering a comfort-margin pan recenter even
  when the click landed well inside the visible spectrum, making
  the outer 18% of each pan effectively a dead zone that auto-pulled
  the slice inward.  Two layers of fix: the spectrum-click path
  itself uses a 5% margin, *and* the `RevealOffscreen` intent that
  fires from the deferred active-slice change after a click now only
  reveals when the slice is truly outside the visible window (margin
  0).  Net behaviour matches SmartSDR — click anywhere visible and
  the panadapter stays put.

**TCI tx_enable echoes immediately to unblock WSJT-X v3.0.0 TX (#1689, #1691)**
- WSJT-X v3.0.0 waits for a `tx_enable` ack before keying.  AetherSDR's
  TCI handler now matches the `cmdTrx` pattern (immediate echo plus
  pending notification) so WSJT-X TX is unblocked on current builds.

**Fix silent audio in SSB/Digital modes when m_radeMode is active (#1875, #1879, NF0T)**
- RADE-mode activation toggled a DAX route flag in `AudioEngine` that
  left SSB and Digital modes silent on TX.  Audio path is now
  restored when RADE mode is not active.

**MultiFlex PTT label, row-selection, and correct PTT command (#1893, chibondking)**
- Three related Multi-Flex PTT issues: the PTT button label showed
  the wrong station (other client's name instead of ours), row
  selection had no effect, and the wrong protocol command was being
  sent.  `client set enforce_local_ptt=1` returns `0x50001000` on
  v1.4.0.0 — the correct command is `client set local_ptt=1`, which
  is now used and documented in `CLAUDE.md` quirks.

**Network health indicator stabilised for SmartLink and remote paths (#1900, jensenpat)**
- Status-bar network indicator was twitchy on LTE, SmartLink, and
  hotel Wi-Fi.  Now uses remote-friendly RTT thresholds, evaluates
  VITA packet loss over a rolling 10-second window, includes audio
  inter-arrival jitter, and adds display hysteresis so the label
  only changes state when the underlying condition is sustained.
  Tooltip and diagnostics dialog now also show recent packet loss
  and network jitter.

**Suppress startup client login replays (#1904, jensenpat)**
- When AetherSDR subscribed to `client all` after connecting, the
  radio replayed the current client list as `client ... connected`
  status messages — which the new notification handler from #1892
  treated as fresh logins.  Existing client handles captured from
  discovery and the SmartLink radio list are now treated as already
  present, with a 5-second startup grace window as a fallback.

**Persist CAT/TCI/DAX IQ enabled state across restarts (#1890, chibondking)**
- CAT, TCI, and DAX IQ enabled flags weren't written to AppSettings,
  so toggling any of them off and restarting brought them back on.
  Now persisted under their respective enabled keys and restored at
  startup.

**Reduce minimum window height (#1793, #1798)**
- Operators with smaller vertical footprints (low-resolution
  displays, cramped multi-monitor setups) couldn't shrink the main
  window below the prior height floor.  Lowered the minimum so
  small layouts fit.

**What's New release date now visible (#1783, #1785)**
- The release date line on the What's New dialog was being clipped
  off the visible area.  Layout now anchors the date inside the
  dialog frame.

**Restore Flex band-stack selection keys (#1887, jensenpat)**
- The hardware Flex front-panel band-stack keys had stopped routing
  to the band-stack selection action after a recent refactor.
  Re-wired so keypad band selection works again.

**Tighten incremental pan-follow margins**
- Trigger margin lowered from 12% → 5% and settle from 18% → 6% so
  trackpad / wheel / arrow-key tuning at the pan edge feels snappier
  without losing the smooth follow animation.

**Shorten client-connection status message from 10s to 3s**
- The transient banner shown after a successful connection was
  staying up too long and obscuring slice activity.

**Fix versioning label issue on Windows**
- Build version label on Windows could render incorrectly when the
  build metadata included certain characters.  Resolved.

**Band-stack ⚙ / × / + buttons no longer clip their glyphs**
- The three control buttons in the band-stack bottom row now have
  zero internal padding so the multiplication-sign, plus, and gear
  glyphs render fully inside the small fixed-size buttons regardless
  of font/DPI.

### Acknowledgements

Big thanks to **jensenpat** (six PRs this cycle), **chibondking**
(two PRs plus the new About → Contributors entry), and the community
reporters whose detailed issues drove the AetherClaude fixes —
especially **NF0T** for the RADE silent-audio diagnosis.

73, Jeremy KK7GWY

## [v0.8.21] — 2026-04-23

### Recenter policy unification plus Band Stack management

Another community-heavy point release.  Five community PRs plus six
AetherClaude fixes address crash + UX regressions uncovered in v0.8.20,
a Wayland+FFmpeg crash class, a RADE TX regression from v0.8.19, and
a large tuning-policy refactor that consolidates pan/zoom/reveal
decisions into one shared intent model.  Plus the first Band Stack
management feature (clear all, band grouping, auto-expiry).

### Features

**Unified tuning/recenter policy (#1861, jensenpat)**
- Large refactor (+1038/-207, new `RECENTER.md` design doc) that
  moves pan-follow, reveal, and hard-center decisions out of individual
  call sites and into a shared `MainWindow` policy layer.  Every tuning
  input path (VFO wheel, spectrum wheel, trackpad, keys, MIDI,
  FlexControl, HID, memory, spots, clicks, band stack) now dispatches
  through a small `TuneIntent` enum: `IncrementalTune`, `AbsoluteJump`,
  `CommandedTargetCenter`, `ExplicitPan`, `RevealOffscreen`.
- Incremental tuning now uses step-quantized follow (12 % trigger
  edge, 18 % settle, 110 ms animation) so trackpad / wheel / keys
  at the pan edge nudge smoothly instead of page-flipping.
- Combined pan/zoom operations now send one coherent
  `display pan set center=X bandwidth=Y` command instead of two
  separate commands — fixes the P1 waterfall-loss / edge-drop bug
  seen during bandwidth drag and trackpad zoom.
- Fixes the P2 keyboard zoom +/- drift seen after memory recall and
  mode jumps (USB ↔ SAM).
- Memory spot clicks no longer go through an intermediate generic
  spot path — memory recall owns the tune+center ordering end-to-end.
- See `RECENTER.md` for the full architecture, intent semantics,
  call-site classification, and intentional exception list (band
  shortcuts, `restoreBandState`, RxApplet direct entry, and CAT/TCI
  external-control paths still use `tuneAndRecenter`).

**Band Stack management (#1471 → #1472)**
- New UI affordances in the Band Stack sidebar:
  - **× button** — "Clear All" with a confirmation dialog, so
    accidental clears during contests don't wipe a day's work.
  - **⚙ button** — opens a settings menu for "Group by band" and
    auto-expiry (Off / 5 / 15 / 30 / 60 min).
- Grouped mode uses the existing band-definition table; each band
  section gets a header label and a right-click "Clear band" action.
- Auto-expiry defaults off.  Enabled expiry uses a 30-second timer
  that starts on radio connect and stops on disconnect, and
  stale-entry pruning also runs on startup so entries saved under
  an expiry threshold are cleaned when the app reopens.
- Backwards-compatible XML schema: pre-existing entries carry a
  `CreatedAtMs = 0` sentinel and are never auto-expired.
- Thanks to LU5DX for the feature request and LU5FF for the idea.

### Bug fixes

**Install tolerant X11 error handler (#1840)**
- Qt Multimedia's FFmpeg backend probes VA-API / VDPAU hardware
  accel via X11 even under native Wayland.  On systems that ship
  `libvdpau-va-gl` by default (openSUSE Tumbleweed + Packman is the
  canonical case, Fedora RPMfusion is adjacent), VDPAU calls into
  X11/GLX under Wayland → `BadAccess` → Xlib default handler calls
  `exit()` → AetherSDR crashes on Audio-tab open.
- Install a tolerant X11 error handler (via `dlopen(libX11.so.6)` +
  `XSetErrorHandler`) that logs the error and returns 0 instead of
  aborting.  Linux-only, no build-time X11 dependency.
- `AetherX11ErrorEvent` struct layout corrected to match XErrorEvent
  field order so the diagnostic log reports accurate error codes.

**DXCC prefix resolution off GUI thread (#1844)**
- 0.8.19 regression surfaced at 125 k-QSO log sizes: when the ADIF
  auto-reloader (new in 0.8.19) re-processed the logbook,
  `onParseFinished` did O(N) DXCC prefix lookups on the GUI thread,
  producing 0.5 – 1 s waterfall freezes.
- Move prefix resolution into the existing ADIF parse worker thread
  via an optional `AdifParser::setCtyParser()`.  `CtyDatParser` is
  demonstrably read-only after `loadCtyDat()`, so cross-thread access
  is safe.  GUI thread only does the cheap `m_workedStatus.load()`
  hash inserts on the queued callback.

**TCI RX meter post-gain (#1716 → #1717)**
- TCI RX level meter and DAX RX level meter in the same applet panel
  used different measurement points: TCI was pre-gain, DAX was
  post-gain.  At default gain 0.5 that made TCI read ~6 dB higher
  than DAX for the same signal.
- Maintainer decision (community RFC by NF0T): move TCI to post-gain
  to match DAX convention, SmartSDR convention, and make fader
  movement immediately visible on the meter.

**XVTR waterfall always black (#1845)**
- When a transverter is active the radio reports pan center in RF
  domain (e.g. 144.2 MHz) but the VITA-49 waterfall tile header
  carries IF-domain frequencies (e.g. ~28 MHz).  The existing
  frequency-accurate bin mapping then produced bin indices outside
  `srcSize` for every pixel and the scanline stayed all-black.
- Detect the tile/pan frequency mismatch in
  `MainWindow::waterfallRowReady` and shift the tile's reported
  low/high by `panCenter - tileCenter` so the existing
  frequency-accurate rendering in `SpectrumWidget` aligns with the
  pan.  HF panadapters are unaffected (their tiles always overlap,
  the shift branch never fires).

**PAN freeze in Minimal Mode with popped-out panadapter (#1748)**
- `toggleMinimalMode(true)` called `setUpdatesEnabled(false)` on
  every applet in `m_panStack->allApplets()`, which includes
  floating (popped-out) pans.  Their content froze even though
  their window was still visible.
- Skip floating pans on the suspend path via
  `PanadapterStack::isFloating()`.  CPU savings for normal
  (no-pop-out) minimal-mode use are preserved unchanged.

**rigctld TCP CAT per-port slice binding (#1621 → #1623)**
- WSJT-X sends `V VFOB` on init, which `cmdSetVfo()` was treating
  as an authoritative slice-index override.  Every WSJT-X instance
  across multiple rigctld ports then ended up controlling Slice B.
- `cmdSetVfo()` now accepts the command without changing
  `m_sliceIndex` (which is set per-connection by the TCP port
  binding in `RigctlServer::onNewConnection`).  `cmdGetVfo()`
  always reports VFOA (the current VFO for this connection) and
  `cmdGetSplitVfo()` decouples the client-visible VFO label from
  the internal slice id.
- Follow-up #1868 filed for the separate `cmdSetSplitVfo` stub on
  the enable=true path.

**RADE TX no-waveform regression from #1780 (#1865, NF0T)**
- v0.8.19 added `emit daxRouteRequested(on ? 0 : 1)` to
  `AudioEngine::setRadeMode` on the mistaken theory that RADE
  wanted the radio's mic path.  On PTT in RADE mode this sent
  `transmit set dax=0`, which made the radio discard every
  `dax_tx` VITA-49 packet from the RADE encoder and transmit
  silence from the physical mic.
- Remove the `daxRouteRequested` signal and its wiring entirely.
  `updateDaxTxMode` already sets `dax=1` when the TX slice is in
  DIGU/DIGL (which RADE activation configures), so the correct
  routing is always in place before PTT.

**Applet pop-out persistence and app-exit regression (#1860, chibondking)**
- Four bugs around the migration from the retired
  `FloatingAppletWindow` (System A) to the new `ContainerManager`
  (System B):
  1. `ContainerGeometry_P/CW` uses `/` which is invalid in
     AppSettings XML element names → silent save/load failure for
     the P/CW applet → position lost on every restart.  Fixed by
     sanitizing `/` → `_` in `geometryKeyFor`, matching System A's
     `floatKey()` helper.
  2. Legacy float-state migration read the raw ID while System A
     wrote the sanitized key → P/CW users migrating from older
     versions saw the applet docked.  Same sanitizer applied to
     the read path.
  3. Floating windows kept the process alive after the main window
     closed, because they inherited `WA_QuitOnClose=true` as
     top-level widgets.  `WA_QuitOnClose=false` now marks them as
     secondary.
  4. `moveEvent`/`resizeEvent` wrote AppSettings on every pixel of
     a window drag (~60 Hz).  Replaced with a 400 ms debounce
     timer matching System A's original pattern.

**Arduino WiFiServer TCP deadlock for ShackSwitch R4 (#1859, nigelfenton)**
- ShackSwitch R4 is an Arduino-based Antenna Genius.  Arduino's
  `WiFiServer::available()` only returns a connected client once
  the client has sent data, but the AG protocol is
  server-speaks-first (`V1.0 AG`).  Result: AetherSDR waits for the
  greeting, R4 waits for client data → TCP deadlock → every AG
  connect silently hangs.
- Detect ShackSwitch devices (`name.contains("ShackSwitch")`) and send
  an empty `\r\n` on TCP connect.  R4 firmware discards empty lines
  before the greeting, protocol proceeds normally.  Real 4O3A Antenna
  Genius devices do not match the check — zero impact on existing AG
  behaviour.
- Also fix a reconnect-race by replacing the `if (m_connected)` guard
  with an unconditional socket abort in `connectToDevice`, plus
  add an `isConnecting()` helper for UI state.
- Contributed by Nigel Fenton (G0JKN) — the author of the ShackSwitch
  device itself.

### Contributors

Community: NF0T (Ryan Butler), jensenpat, chibondking (CJ Johnson),
nigelfenton (Nigel Fenton), and LU5DX / LU5FF for the Band Stack
feature request.

## [v0.8.20] — 2026-04-22

### Community regression round-up + crash batch

Eight community contributions plus three AetherClaude crash fixes.
The community batch covers real-world regressions found on Windows,
macOS and Linux: RADE RX on Windows, macOS FT8 DT bias, TCI DAX
crosstalk, RX applet pan-with-NR, gain defaults, a Windows build
break, a Canadian band plan refresh, and seamless ADIF logbook
auto-reload.  Three crash fixes address applet reorder, Wayland
Radio Setup dialog, and popped-out panadapter freeze.

### Bug fixes

**Fix RADE RX not decoding on Windows (#1820, NF0T)**
- On platforms without a DAX audio bridge (Windows, Linux without
  PipeWire), `startDax()` is compiled out so `stream create dax_rx`
  is never sent.  RADE mode showed "activated" but produced no
  decoded audio because `feedRxAudio()` never got called.
- `activateRADE()` now creates the `dax_rx` stream directly on
  non-bridge platforms and registers its stream ID with
  PanadapterStream.  TCI borrow-safe cleanup on deactivate — won't
  tear down a stream another client is reading.
- Verified end-to-end on Windows 11 + FLEX-8400.

**Clamp stale DAX RX backlog on macOS (#1822, jensenpat)**
- FT8 decodes in WSJT-X / JTDX showed a consistent +1.5 s to +1.6 s
  `DT` offset on macOS even with healthy LAN latency — severe enough
  to cause missed QSOs.
- Root cause: macOS `VirtualAudioBridge` writes DAX RX audio into a
  POSIX shared-memory ring large enough for ~2 s of audio.  The TX
  path had live-edge backlog protection; the RX path did not.  If
  the CoreAudio reader fell behind the ring accumulated stale audio
  and served it as if current.
- New live-edge clamp advances the reader when backlog exceeds
  ~200 ms, keeping a ~40 ms target.  Per-channel timing counters
  behind the existing `aether.dax` debug category.  Scoped to macOS
  only — Linux uses `O_NONBLOCK` named pipes (drops at syscall),
  Windows has no in-process buffer.

**Fix TCI DAX audio crosstalk between slices (#1815, Chaosuk97)**
- Two simultaneous DAX channels (dual-band WSJT-X, satellite
  full-duplex RX) caused audio from slice A to bleed into slice B.
- Root cause: `r8brain CDSPResampler` is stateful.  `ClientState`
  held a single shared resampler reused across channels, so filter
  state from channel 1 carried over into channel 2.  The
  accumulation buffer was already per-channel; the resampler wasn't.
- Replaced the shared `Resampler*` with
  `QHash<int, Resampler*> resamplers` keyed by DAX channel.  Lazy
  per-channel creation, `qDeleteAll()` on rate change / disconnect.

**Fix RX applet pan slider broken with NR active (#1799, Chaosuk97)**
- PR #1460 re-applied pan after client NR mono-mix but only tracked
  pan changes from the VFO panel slider.  The RX applet, MIDI, and
  keyboard shortcuts call `SliceModel::setAudioPan()` directly —
  `AudioEngine::m_rxPan` stayed at 50, making the RX applet pan
  slider a no-op when NR was active.
- Listen to `SliceModel::audioPanChanged` instead — one connection
  now covers VFO panel, RX applet, MIDI, and radio echo-back on
  connect.

**Fix TCI RX gain default fallback: 1.0 → 0.5 (#1811, NF0T)**
- `TciServer` fallback for a missing `TciRxGain<n>` key was "1.0"
  while `TciApplet` used "0.5".  Fresh install or post-migration
  without the key ran the server at full gain while the slider
  showed half — TCI RX audio 2× louder than DAX at equal positions.

**Fix missing HAVE_SERIALPORT guard on m_flexCoalesceTimer (#1812, NF0T)**
- PR #1606 guarded `m_hidCoalesceTimer` behind `HAVE_HIDAPI` but
  left `m_flexCoalesceTimer` unguarded on the line above — broke
  Windows builds without `Qt6::SerialPort`.  Wrapped the reference
  in `HAVE_SERIALPORT` to match the member declaration.

**Seamless ADIF logbook auto-reload for DXCC spot colouring (#1801, Chaosuk97)**
- Spot colours only updated when the user manually clicked Browse
  and re-selected the log file.  The "Auto-Reload Log" toggle
  defaulted off and broke in edge cases: atomic rename (N1MM,
  Log4OM), Windows write-lock during export, delete-then-recreate.
- Toggle removed — Browse now always arms the watcher.  2-second
  debounce coalesces rapid change notifications; watcher re-arms
  after debounce so atomic rename (new inode) is caught.  Directory
  watcher on the parent folder catches delete-then-recreate.  Open
  retries 3× at 500 ms intervals for write-locks.  Worked status
  preserved on open failure — colours don't flash empty during
  export.  "Updating…" shown during async parse.

**Fix crash when reordering applets with floating containers (#1745 → #1746)**
- Dragging an applet title bar to reorder crashed AetherSDR when
  any other applet was popped out into a floating window.
- Root cause: `QWidget::mapTo()` traverses the parent chain looking
  for a common ancestor.  When a container is floating (reparented
  to `FloatingContainerWindow` — a separate top-level window), there
  is no common ancestor with the scroll area's content widget and
  `mapTo()` dereferences `nullptr`.
- `dragMoveEvent()`, `dropIndexFromY()` and `rebuildStackOrder()`
  now skip entries whose `ContainerWidget::isFloating()` is true.
  Follow-up filed as #1836 for the separate TXDSP ID-mismatch that
  makes that container silently un-reorderable.

**Lazy-build RadioSetupDialog tabs (#1776 → #1777)**
- Opening Settings → Radio Setup / FlexControl / USB Cables crashed
  on some Wayland / Qt 6.11 configurations (openSUSE Tumbleweed with
  Packman non-free FFmpeg 8.1).  Real crash cause is
  `libvdpau_va_gl` trying to create a VDPAU device via X11/GLX on a
  native Wayland session — an upstream Qt Multimedia backend /
  distro packaging problem, not an AetherSDR bug.
- Converted the dialog from eager to lazy tab construction — only
  the Radio tab builds on open; the other ten tabs get placeholder
  widgets and deferred builders triggered on first tab-switch.
  Dialog opens without crashing (and faster on every platform).
  Users on the affected config will still hit the VDPAU crash if
  they click the Audio tab; the real fix is to switch
  `QT_MEDIA_BACKEND=gstreamer` or use stock Mesa FFmpeg.

**Fix UI freeze when PanAdapter is popped out (#1668 → #1669)**
- Content inside a popped-out panadapter froze — waterfall stopped
  scrolling, spectrum stopped updating — while the main window
  remained fully responsive.  Re-docking restored normal operation.
- Two problems in `PanadapterStack::floatPanadapter()`: the
  SpectrumWidget was shown immediately after reparenting, before
  `refreshAfterReparent()` rebound the Metal render target to the
  new NSView, so the first frame rendered on a stale/transitional
  surface.  And the reparent went through `setParent(nullptr)` —
  on macOS with `WA_NativeWindow`, that creates a transient
  top-level NSWindow before the floating window adopts it, a second
  lesser problem addressed by #1344 for the dock path but never the
  float path.
- New `PanFloatingWindow::adoptApplet()` method reparents directly
  via `m_layout->addWidget()` — single splitter→floating-window
  step, no nullptr intermediate.  `sw->show()` moved into the
  deferred callback after `refreshAfterReparent()` so Metal binds
  to the final NSView before the first render.

**Minor Canadian band-plan corrections (#1817, VE3NEM)**
- RAC band plan updates from a Canadian ham: label corrections on
  overlapping CW/DIG segments (1.800, 7.035), WSPR 80 m frequency
  corrected (3.5926 → 3.5686 MHz), obsolete CW calling frequencies
  removed and standard international QRP calling frequencies added
  (14.060, 21.060, 28.060, etc.), missing SSB calling frequencies
  added on 17 m / 15 m / 12 m / 10 m / 1.25 m.

### Contributors

Community: NF0T (Ryan Butler), Chaosuk97 (Ian M7HNF), jensenpat, VE3NEM.
AetherClaude bug fixes on #1745 / #1776 / #1668.

## [v0.8.19] — 2026-04-20

### Community contributions landfest + macOS MNR + quick memory actions

This release is mostly community work.  Ten community PRs landed, plus an
AetherClaude-draft fix batch covering bandwidth-correction echoes, stale
slice-click tuning, Windows rigctld IPv4, serial port persistence, DX
cluster reconnect, compression-meter gain reduction, and macOS audio
device override.

### Features

**macOS MMSE-Wiener spectral noise reduction (MNR) (#1672, Chaosuk97)**
- New macOS-exclusive client-side NR mode accelerated via Apple
  Accelerate (vDSP / AMX on Apple Silicon).  Decision-directed
  MMSE-Wiener filter with 25-frame minimum-statistics noise-floor
  tracking.  512-point real FFT at 24 kHz, 50% overlap-add with
  sqrt-Hann window, per-bin gain with temporal smoothing to suppress
  musical noise.  ~0.3 ms per hop on M-series chips, one-hop startup
  latency (≈10.7 ms).
- MNR button appears in both the VFO DSP tab and the spectrum overlay
  DSP panel.  DSP Settings dialog gains a new MNR tab with a strength
  slider (0-100).  Mutually exclusive with NR2/RN2/NR4/BNR/DFNR.
  Persists enable state + strength across restart.
- Non-Apple builds: `#ifdef __APPLE__` gated throughout — zero MNR
  overhead, buttons hidden.

**PooDoo™ TX Reverb (#1741) — carried forward, chain now complete**
- Already shipped in v0.8.18 final release notes; listed here for
  completeness since it rolls into the 0.8.19 post-release chain view.

**Quick memory browse + save on slice overlays (#1781, jensenpat)**
- Two new actions in the slice overlay rail between Display and DAX:
  - **MEM▸** opens a compact 252×430 browser drawer, frequency-sorted,
    pre-scrolled to the active slice's nearest memory.  Click-to-recall.
  - **MEM+** opens a small save dialog for the current slice on that
    panadapter.  Captures mode, filter, step, repeater offset, tone,
    squelch, RTTY/DIGL offsets — same field set as the main Memory
    dialog via the new shared `MemoryCommands` helper module.
- Pan-aware routing: save/recall targets the active slice if it lives
  on this pan, otherwise the first slice found on the pan.  The MEM
  buttons now show the target slice letter (e.g. `MEM▸ A`) so users
  can see which slice will be hit before clicking — makes the
  multi-slice fallback visible.

**Improved TNF notch interaction (#1547, rfoust)**
- TNF markers no longer extend into the waterfall — they stop at the
  bottom of the spectrum pane so the waterfall stays a historical
  signal record.
- Depth visualisation via hatching (12/8/5 px spacing for depth 1/2/3).
- Floating "RF Tracking Notch" popup on hover shows frequency + width.
- Width/depth submenu items show a checkmark on the currently-active
  value.
- Bidirectional TNF drag: horizontal tunes, vertical resizes via
  `2^(-dy/48)` octaves, clamped 10-12 000 Hz.
- Preferred-TNF hit-testing for overlapping notches — dragging a TNF
  preserves the target across its sibling's overlap zone.
- Tune guides suppressed while a TNF is hovered or dragged.
- `TnfModel`: optimistic local updates in setters so UI responds
  without waiting for radio echo; width parser tolerates both Hz
  ("width=100") and fractional-MHz forms for firmware-version variance.

**RAC Canada Band Plan corrected + expanded (#1709, VE3NEM)**
- VHF / UHF bands added (6 m, 2 m, 1.25 m, 70 cm) — prior file was
  HF-only, so 4 new ham-band coverage tiers arrive in this update.
- 60 m channels updated to match the Feb 2026 ISED regulation change
  (new 5.3570-5.3598 channel).
- 12 m frequencies corrected.
- FT8 spot at 14.074 added back, one duplicate spot removed.

**Frameless Window toggle (View menu, Ctrl+Shift+F)**
- View → Frameless Window applies Qt's `FramelessWindowHint` so users
  gain ~32 px of vertical screen space at the cost of the OS title bar.
  Works cross-platform (X11, Wayland, Windows, macOS) — KDE window
  rules don't strip Qt's client-side decorations reliably on Wayland,
  so we drop the frame in code.  Move/close via WM shortcuts or
  taskbar.  Persists across restart; applied before first `show()` so
  there's no flash-of-chrome on startup.

**CHAIN-driven TX DSP (v0.8.18 rebase — carried forward)**
- Already in v0.8.18 release notes; called out here because subsequent
  releases continue to build on the CHAIN ordering.

### Bug fixes

**NR2 crackling on strong signals (#1696, Chaosuk97)**
- Cap `gainMax` default 1.5 → 1.0.  The MMSE-LSA estimator is a noise
  *reducer* — the old 1.5 default allowed 50% amplification on strong
  signals, which pushed output past ±1.0 float32 full scale and caused
  QAudioSink digital-clipping crackle.
- Belt-and-suspenders `std::clamp(sample, -1.0f, 1.0f)` in
  `processNr2()`.  A separate Bessel overflow (issue #1507) can still
  produce NaN-burst crackle on Gamma-method NR2 under strong-signal
  conditions — tracked for follow-up, not in this release.

**Audio pan broken with client NR active (#1685, Chaosuk97) + per-slice AF mute persistence (#1560)**
- Client-side NR (NR2 / RN2 / NR4 / DFNR) mono-mixes L+R before
  processing, destroying the radio-applied pan.  New
  `AudioEngine::setRxPan` atomic + static `applyRxPanInPlace` helper
  re-applies a linear pan law to the NR output so the pan slider
  continues to work while client NR is active.  No-op at pan 50
  (centre) — zero overhead when user hasn't panned.
- Per-slice AF mute now persists across sessions.  VFO widget emits
  `audioMuteToggled`; MainWindow saves `SliceAudioMuted_{A,B,C,D}` to
  AppSettings on every user toggle, restores on reconnect.  Uses the
  slice letter (not numeric id) for the settings key so state survives
  sessions where the radio assigns a different numeric id.

**VFO / TNF / filter-edge ±1 px jitter during tuning (#1703, Chaosuk97) + cursor-snap-to-VFO (#1369)**
- `mhzToX()` switched from integer truncation to `std::round` so the
  same frequency always maps to the same pixel column regardless of
  pan-center fractional drift during continuous tune.  One-line fix
  that ripples through every vertical marker — VFO, TNF, filter edges,
  RIT/XIT, band plan, spots, grid.
- Cursor frequency readout snaps to the exact VFO frequency when
  within 8 px of a slice marker (GPU path already had this; software
  `paintEvent` path added for symmetry).
- `setSliceOverlayFreq()` now calls `markOverlayDirty()` and
  early-exits on unchanged value.

**Zoom +/- can't reach full panadapter bandwidth (#1697, Chaosuk97)**
- Replace early-return guard in `emitZoom` / keyboard zoom with
  `std::clamp(newBw, minBwMhz, maxBwMhz)` so the final click/keypress
  snaps to exactly the radio's min/max — matching the mouse-drag
  behaviour which already used clamp.

**PC Audio button shows wrong state on connect (#1695, Chaosuk97)**
- `onConnectionStateChanged` now reads `PcAudioEnabled` once and calls
  both `m_titleBar->setPcAudioEnabled(...)` (sync button, with
  QSignalBlocker) and `audioStartRx()` from the same decision.
  Previously the sink and button could diverge after a profile import.

**Waterfall filter-passband fill removed (#1701, Chaosuk97)**
- The coloured filter-passband fill in the waterfall was only rendered
  by the software-fallback paintEvent path — the GPU renderer already
  skipped it.  Dropping the software `fillRect` brings the two render
  paths into agreement.  Filter edge lines still render in both panes.

**TCI DAX duplicate stream causing 2× audio speed (#1679 / #1678)**
- Two code paths both subscribing to `dax_rx` for the same channel
  made `daxAudioReady` fire twice per period, doubling WSJT-X's
  apparent sample rate and killing FT8 decode.  Registration now
  evicts stale subscribers and TCI uses a borrow protocol.

**TCI TX no RF output (#1680)**
- Always emit `transmit set dax=1` on PTT regardless of the previous
  Low-Latency DAX menu toggle.  The `dax=0` path was advertised for
  external FreeDV over virtual audio cable but never actually worked
  because that route silently drops `dax_tx` VITA-49 packets.  The
  Low-Latency DAX menu has since been retired (#1780).

**Low-Latency DAX consolidated into RADE mode (#1780)**
- The "Low-Latency DAX (FreeDV)" menu item was removed.  RADE mode's
  activation now emits a `daxRouteRequested` signal that MainWindow
  routes to `transmit set dax=N`, flipping the route atomically with
  the RADE button.  One control, one place.  Stale
  `DaxTxLowLatency` AppSettings value is no longer read.

**Bandwidth corrections silently dropped during pan-follow (#1729)**
- The stale-echo guard in `setFrequencyRange` bypassed *any* echo
  during an in-flight pan-center animation when the center matched.
  If the echo also carried a bandwidth correction (e.g. post-resize
  `xpixels` re-sync), it was thrown away.  Guard now also checks the
  bandwidth field so legitimate corrections pass through.

**Pan-follow VFO edge-tracking after manual drag (#1643)**
- After a manual spectrum drag, `PanadapterModel::centerMhz` lagged
  the visible pan center by up to one frame.  `panFollowVfo` now
  reads the live center from the spectrum widget instead of the
  model, so edge-fence checks fire against the user's actual view.

**Propagation overlay click tunes VFO by accident (#1647)**
- Clicking the K/SFI propagation overlay was also triggering the
  release-to-tune path.  Set `m_spotClickConsumed = true` after the
  `propForecastClicked` emission so the subsequent release event
  can short-circuit.

**Off-screen slice click tunes wrong frequency (#1772)**
- Same pattern as the prop-overlay fix: clicking an off-screen slice
  indicator now suppresses release-to-tune so the tuning action
  doesn't pile on top of the slice-activate.

**rigctld TCP server not accepting IPv4 on Windows (#1737)**
- `QHostAddress::Any` sometimes binds IPv6-only on Windows, leaving
  IPv4 clients unable to connect.  Use `QHostAddress::AnyIPv4`
  explicitly.

**Serial port parameters (data bits, parity, stop bits) not persisted (#1610)**
- RadioSetupDialog's serial tab built the combos with defaults but
  never read from AppSettings or connected the save handler.  Added
  both.

**Compression meter shows raw dBFS instead of gain reduction (#1682)**
- COMPPEAK is the compressor *output* level; AFTEREQ is the *input*.
  Meter was displaying raw COMPPEAK, which just shows "how loud is
  the output" instead of "how much is the compressor reducing".  Now
  shows `output - input` (gain reduction, 0 = no reduction, negative
  = compressing).

**macOS audio output device silently overridden (#1705)**
- On macOS, AudioEngine replaced the user's chosen output device with
  `QMediaDevices::defaultAudioOutput()` whenever Qt reported 48 kHz
  as unsupported — but that happens on some normal CoreAudio outputs
  with newer Qt, not just Bluetooth telephony routes.  Narrow the
  override to devices that genuinely can't handle the native 24 kHz
  format.

### Internal

**Issue template tightened**
- Bug-report reproduction steps are now required.  AetherSDR version,
  radio model+firmware, and OS are required fields with platform-
  specific guidance (Linux distro + Qt / macOS version + chip /
  Windows edition).  Improves triage quality on incoming reports.

**Contributors**
- Added VE3NEM, Ian M7HNF (Chaosuk97), and jensenpat to the About
  dialog contributors list.

### Reverted

- The spectrum/waterfall alignment tweak from #1694 was landed as a
  partial fix (alignment math only, skipping an unrelated
  SMOOTH_ALPHA tuning change) then reverted when the trimmed fix
  introduced misalignment rather than correcting it.  Issue #1690
  closed — the originally-reported bug did not reproduce on main.

## [v0.8.18] — 2026-04-20

### PooDoo™ TX Audio — complete six-stage DSP chain, click-bypass, reverb

### Features

**PooDoo™ Audio: Gate, DeEss, Tube, PUDU DSPs + editors (#1661 phases 2-5, #1734)**
- Four new client-side TX DSP stages complete the chain started in v0.8.17:
  - **ClientGate** — downward expander / noise gate with Expander↔Gate
    mode toggle, threshold/ratio/attack/hold/release/floor/return/look-ahead
    parameters, Ableton-style level view with two-threshold hysteresis band.
  - **ClientDeEss** — sidechain-filtered de-esser.  Tunable bandpass
    (1-12 kHz log sweep, Q 0.5-5) feeds the envelope detector; threshold +
    amount parameters control broadband attenuation on sibilance.
  - **ClientTube** — dynamic tube saturator with three models (A: soft
    tanh, B: hard-clip + tanh hybrid, C: asymmetric) + bipolar envelope-
    driven drive, tilt pre-filter, parallel dry/wet mix.
  - **ClientPudu** — exciter modelled on Aphex Aural Exciter + Big Bottom
    (Even mode — asymmetric harmonics + LF saturation) and Behringer
    SX 3040 (Odd mode — symmetric tanh + feed-forward LF compressor).
    Six-knob Poo/Doo layout with glowing PooDoo™ wordmark driven by wet
    RMS level.
- Every stage has a full-featured floating editor with Ableton-style
  visualisations: level histograms, curve overlays, live envelope
  balls.  Editors run lock-free atomic parameter updates with a version
  counter so audio-thread `process()` never locks or allocates.
- Consistent visual language across all editors: uniform 76×76 knobs
  with in-ring labels that auto-shrink to fit, right-aligned Bypass
  buttons, and uniform "Thresh" / "Freq" naming conventions.
- Per-stage test harnesses in `tests/client_*_test.cpp` covering DSP
  invariants (impulse response, ratio curves, mix laws, stereo
  linkage, finite output under extreme params).

**PUDU Monitor — post-chain record + playback (#1734)**
- Two icon buttons in the CHAIN header (⏺ record, ▶ play) capture up to
  30 seconds of **post-PUDU TX audio** without transmitting.  Recording
  writes an int16 stereo 24 kHz WAV to `/tmp/pudu_monitor.wav` for
  offline inspection; playback streams from an in-memory buffer through
  the existing RX sink path.
- Record button is only enabled when MIC=PC and DAX=off — same
  condition that turns the `[MIC]` endpoint green in the CHAIN widget.
  Transitioning out of that state mid-record auto-stops.
- During recording and playback the RX panadapter stream is
  disconnected from the audio sink (same pattern as QsoRecorder) so
  live band audio doesn't bleed under the preview.

**Reverb (Freeverb) stage + CHAIN integration (#1741)**
- New **ClientReverb** DSP — Jezar's canonical Freeverb algorithm: 8
  parallel lowpass-feedback comb filters summed through 4 series
  allpass filters, stereo-spread by 23 samples between channels.
  Pre-delay ring buffer in front of the reverb core.  Sample-rate-
  scaled for 24 kHz native processing; CPU cost under 1 % of one core.
- Five-knob control set: **Size** (0-100 %), **Decay** (0.3-5 s
  exponential), **Damping** (0-100 %), **Pre-delay** (0-100 ms),
  **Mix** (0-100 %).
- Ships **disabled by default** and positioned as the final stage
  (`[MIC]→[GATE]→[EQ]→[DESS]→[COMP]→[TUBE]→[PUDU]→[VERB]→[TX]`) — reverb
  on ham TX is unusual and trades intelligibility for "bigness".  Users
  opt in by single-clicking `[VERB]` in CHAIN.
- Settings persisted as `ClientReverbTx*` in `AppSettings`.  9-case
  test harness covers impulse tail, decay monotonicity, dry/wet laws,
  pre-delay timing, mono/stereo processing.

**CHAIN gesture set — single-click bypass, double-click edit (#1739)**
- CHAIN widget rewires the click gestures:
  - **Single-click** a stage → toggle bypass.
  - **Double-click** a stage → open its floating editor.
  - **Drag** a stage → reorder the chain.
  - **Right-click** → context menu (Edit… / Bypass).
- Deferred single-click timer (at the OS double-click interval) lets a
  genuine double-click cancel the pending bypass toggle so the gestures
  don't fight.
- **Bypass buttons removed from every node editor** — bypass now lives
  exclusively on the CHAIN widget.  Enable / Edit buttons removed from
  every applet tile (GATE, CEQ, DESS, CMP, TUBE, PUDU) since the CHAIN
  is now the single source of truth.

**Applet-tile knob rows (#1739)**
- Every DSP node's applet tile grows a compact 4-5 knob tuning row at
  PUDU-style 38×48 px so common parameters can be tweaked without
  opening the full editor:
  - **GATE**: Thresh · Ratio · Attack · Release · Floor
  - **DESS**: Freq · Q · Thresh · Amount
  - **CMP**: Thresh · Ratio · Attack · Release · Makeup
  - **TUBE**: Drive · Tone · Bias · Output · Mix
  - **PUDU**: Drive · Tune · Mix | Tune · Air · Mix (Poo/Doo groups)
  - **VERB**: Size · Decay · Damp · Pre · Mix
- 30 Hz two-way sync (QSignalBlocker-guarded) keeps applet and editor
  knobs in lock-step when values change in either surface.

**Applet-stack order mirrors CHAIN order (#1739)**
- Dragging a stage to a new position in CHAIN automatically reorders
  the sub-container tiles inside the PooDoo™ Audio group to match.
  On startup the persisted chain order drives the initial tile order.
- CEQ titlebar shows "COMPRESSOR" (was "CMP"); TXDSP group button bar
  label renamed "VUDU" → "PUDU" to match the exciter node; Settings
  key stays `Applet_TXDSP` for migration.

**Visual TX signal chain applet (#1661)**
- New CHAIN tile renders the full TX DSP path as a row of labelled
  boxes — MIC → GATE → EQ → DESS → COMP → TUBE → ENH → TX — with
  live bypass state, drag-drop reordering, per-stage click-to-edit
  and right-click bypass.  Snake layout wraps to 3-3-2 rows so the
  full chain fits inside the 260 px applet panel without cramping
  box labels.
- DSP: generalised TX chain replaces the previous two-option
  CMP↔EQ toggle with a generic ordered stage list stored as a
  packed uint64_t atomic (one byte per slot).  Audio thread loads
  the whole chain in one acquire-read per block; `applyClientTxDsp`
  walks the stage list and dispatches to the matching per-stage
  apply helper.  Gate, DeEss, Tube, Enh are stub slots today —
  placeholders for Phase 2+ Pro-XL work — and users can drag them
  into their preferred position now so the layout is ready when
  their DSP ships.
- Existing ClientCompTxChainOrder (0/1) settings migrate cleanly:
  legacy CMP-first / EQ-first positions carry over into the new
  stage list with the canonical stages (Gate, DeEss, Tube, Enh)
  appended in sensible slots.

**Unified TX DSP container**
- CHAIN, CEQ and CMP are now three sections of a single TXDSP tile
  in the applet tray.  Each section has its own titlebar with its
  own float / close buttons so individual sections can pop out
  independently, while the outer tray toggle, drag-handle and
  settings all treat the group as one coherent unit.
- CEQ and CMP tile visibility is driven by DSP bypass state — right-
  clicking a stage in the CHAIN widget or toggling the Bypass button
  inside an editor hides the corresponding section; enabling it
  brings the section back.  No separate "show tile" toggle.

**Applet panel pop-out**
- View → "Pop Out Applet Panel" (Ctrl+Shift+S) floats the entire
  right-side panel into its own top-level window.  Splitter slot
  collapses to zero; spectrum takes the reclaimed width.  Close
  the floating window (or toggle the menu item) to dock back at
  the remembered width.  Position and size persist across launches.

**Post-fader level meters on DAX / TCI**
- MeterSlider now multiplies its displayed RMS by the gain thumb
  before rendering, so moving the fader on a TCI or DAX channel
  gives immediate visual feedback instead of showing the raw
  pre-fader level.

**Consistent meter motion — MeterSmoother**
- Extracted the asymmetric attack/release smoothing pattern used
  by HGauge into a shared MeterSmoother helper (30 ms attack,
  180 ms release, polled at 120 Hz).  Applied to HGauge,
  ClientCompMeter, ClientCompApplet's GR strip and MeterSlider so
  every metering surface in the app reads with identical ballistics.

### Fixes

**Digital-mode TX routing**
- AudioEngine bypasses client-side Comp + EQ on the DAX/TCI TX path
  so WSJT-X and fldigi tones reach the radio unshaped.  Mic voice
  TX continues to run through the full client-side DSP chain.
- Fixed limiter envelope that could latch "active" forever after
  any trigger because the envelope decays asymptotically toward
  1.0 in float and never reaches it — now uses a 0.005 dB threshold
  to detect "not firing" which releases the active indicator.

**Compressor editor polish**
- Threshold fader on the left of the compressor editor replaces
  the old numeric label + tiny triangle with a combined input-
  level meter + draggable slider, matching the Client EQ output
  fader visual language.
- Threshold chevron on the curve gained a full-height dashed guide
  line so it reads from across the room; hit-test widened to grab
  anywhere along the vertical column.
- Live tooltips on the threshold chevron and ratio handle show the
  current value and describe the gesture.
- Limiter visualization: bright amber ceiling line on the Output
  meter, dim red "no-go zone" above, cyan GR tick showing how much
  the limiter is reducing, and a three-state LIMIT button
  (disarmed / armed / firing) that glows red for 500 ms after each
  clamp event.

### Under the hood

**Container system (#1713, foundation library)**
- New src/gui/containers/ subsystem: ContainerWidget (generic
  dockable wrapper), ContainerTitleBar (header with float/close/
  drag handle), FloatingContainerWindow (top-level host with
  geometry persistence + screen-visibility safety),
  ContainerManager (lifecycle + path lookup + content factory +
  JSON persistence under the ContainerTree settings key).
- Supports arbitrary nesting — a container's body can hold leaf
  widgets, other containers, or any mix.  Each container has its
  own float/dock/visibility state; transitions cascade correctly
  across parent/child relationships (floated parent takes docked
  children with it, floated child is independent of its parent's
  state, etc.).
- 80-assertion test suite across three harnesses covering
  lifecycle, manager persistence round-trip, and all eight of the
  nested-float/dock/hide edge cases called out in the design plan.
- This release uses the container system for just the TXDSP group.
  Future features needing grouped dockable tiles (RX DSP cluster,
  meter groups, macro panels) build on the same foundation without
  re-litigating the architecture.

**ClientCompKnob refinements**
- Shared rotary knob widget gains a **center-label mode** (draws the
  parameter name inside the ring instead of above) used by every new
  editor so the 270° arc, the label, and the value readout live in a
  tight 76×76 footprint.  Label font auto-shrinks to `diameter / 6`
  so "Thresh" / "Release" render at the same size as "Hold" across a
  knob row.
- Value readout now sits in the empty 90° sector at the bottom of the
  arc instead of below the widget — no gap between ring and value.

### Fixes

**CEQ cascade Q honouring (#1739)**
- Fix regression where HP/LP bands ignored the user-set Q on cascade
  slopes.  Cascade-slope refactor had hardcoded the family Q; new
  `resonanceScale = userQ / 0.707` multiplier restores user control
  of resonance for slopes above 12 dB/oct.

**PUDU monitor audio routing (#1734)**
- Earlier implementation mixed recorded int16 directly into the sink
  (wrong format, blew out speakers) and interleaved playback with
  live RX audio.  Fixed: convert int16→float32 before enqueueing at
  10 ms cadence; mute RX by disconnecting the pan stream ↔ AudioEngine
  signal rather than calling `setMuted()` (which silenced our own
  playback too).

## [v0.8.17] — 2026-04-19

### Client-side TX Compressor (Pro-XL-style, Phase 1) + Fully-interactive 10-band Client EQ

### Features

**Client EQ — 10-band parametric, 4 filter families, output fader (#1650 / #1651 / #1658 / #1660)**
- Client-side 10-band parametric EQ on both RX (post-NR, pre-sink) and
  TX (post-mic, pre-VITA-49) paths.  Default layout: HP / Low Shelf /
  6× Peak / High Shelf / LP, all disabled on first launch
- Each pass (HP / LP) band cascades up to 4 biquad sections for slopes
  of 12 / 24 / 36 / 48 dB/oct; peak / shelf bands stay native 2nd-order
- Global filter-family enum: Butterworth (flat maximal), Chebyshev I
  (1 dB ripple), Bessel (tabulated pole Q/mag for orders 2-8), Elliptic
  (Chebyshev-II-ish approximation).  Display path sums section
  magnitudes in dB from the analog prototype, in double precision, so
  the curve stays clean at all frequencies
- Grabbing a handle or clicking an icon auto-enables the band.  Right-
  click handles for type picker, bypass, reset-to-default; HP / LP also
  get a Slope submenu
- Live post-EQ FFT analyzer overlaid on the response curve (2048-point
  Cooley-Tukey at ~25 Hz, fed from an audio-thread ring buffer with no
  UI-side mutex).  Gradient terminates at the last valid bin so there's
  no misleading plateau above Nyquist
- Combined output-fader widget on the right edge: one custom-painted
  widget with a vertical peak meter, dB scale (0 / -6 / -12 / -20 / -40)
  and a horizontal fader handle that overhangs the bar on both sides.
  Click-drag, wheel for 0.5 dB fine step, double-click resets to 0 dB
- Settings migrate cleanly — existing users' saved bands map into the
  new 10-slot layout

**Client-side TX compressor (Pro-XL-style, Phase 1) (#1661)**
- Feed-forward compressor with soft-knee quadratic interpolation, linear-
  domain peak envelope detection, stereo-linked gain application, and a
  brickwall peak limiter on the output
- Parameters: threshold (-60..0 dB), ratio (1:1..20:1), attack (0.1..300 ms),
  release (5..2000 ms), knee (0..24 dB), makeup (-12..+24 dB), limiter
  enable + ceiling (-24..0 dB)
- Chain-order toggle in the editor: CMP→EQ (default, colour-then-shape)
  or EQ→CMP (shape-then-tame) — the two client-side TX DSP stages now
  run in a user-configurable order
- Atomic parameter handoff from UI to audio thread, lock-free, no alloc
  in process(), identical pattern to ClientEq

**Docked Compressor applet + floating editor**
- View-only CMP tile in the applet tray: live transfer curve with a
  glowing ball that slides along the curve at the current envelope level,
  horizontal GR strip beneath, Bypass toggle + Edit… button
- Full floating editor (Ableton-inspired, extended for our limiter and
  chain order): rotary knobs for Ratio / Attack / Release / Knee /
  Makeup / Ceiling, interactive transfer curve with draggable threshold
  handle + ratio handle, vertical Input / GR / Output meters, Limiter
  enable toggle, chain-order selector, persistent window geometry
- Meter ballistics match Phone/CW Level (30 ms attack, 180 ms release)

**DSP test harness**
- tests/client_comp_test.cpp — 11 standalone smoke tests: bypass,
  below-threshold passthrough, static ratio (4:1 and 20:1), makeup,
  limiter ceiling, stereo linking, attack timing, soft-knee monotonicity,
  transient sanity, reset(). Built as `client_comp_test` CMake target.

**Settings**
- Client EQ: ClientEq{Rx,Tx}_Enabled / ActiveBandCount, per-band
  ClientEq{Rx,Tx}_Band{N}_* (type, freq, gain, Q, slope, enabled),
  ClientEq{Rx,Tx}_FilterFamily, ClientEq{Rx,Tx}_MasterGain, plus
  ClientEqEditorGeometry
- Client Comp: ClientCompTx* keys (Enabled, ThresholdDb, Ratio,
  AttackMs, ReleaseMs, KneeDb, MakeupDb, LimEnabled, LimCeilingDb),
  ClientCompTxChainOrder, ClientCompEditorGeometry

**Docs**
- docs/tx-audio-signal-path.md — new "Client-side TX DSP" section
  showing mic → ClientEq + ClientComp → VITA-49 → radio firmware chain
- docs/architecture-pipelines.md — TX pipeline diagram shows
  applyClientTxDsp stage inserted between onTxAudioReady and the
  voice/DAX/RADE fork

Compressor Phase 2+ (expander/gate, de-esser, tube, enhancer, low
contour, IKA/IRC, lookahead limiter, preset system) tracked in #1661
for future releases.

## [v0.8.16] — 2026-04-18

### DIGI Applet Split, TCI Audio Reliability, PGXL-Aware S-Meter, Community Contributions

### Features

**DIGI applet split into CAT, DAX, TCI, IQ (#1627)**
- Four independent applets replace the monolithic DIGI tile
- Each has its own toggle button, drag-reorder slot, and float/dock affordance
- TCI gains per-channel RX gain (TciRxGain1-4) and a decoupled TX gain (TciTxGain)
- DAX and TCI audio gains no longer fight each other
- Migrates existing Applet_DIGI and DaxRx/TxGain settings on first launch

**PGXL standby-aware S-Meter (#1635)**
- VU / S-Meter switches between barefoot and PGXL 2kW scales automatically
- In STANDBY the meter shows exciter FWDPWR on the barefoot scale
- In OPERATE the meter shows amp output on the 2kW scale
- Scale and feed flip live on amp state change — no dialog re-open needed

**Refined HF Propagation Dashboard (#1626, community: jensenpat)**
- Five color-coded metric cards for K, A, SFI, SSN, X-ray with plain-language summaries
- Teaching panels for Solar/Lunar (VIEW/HF/MOON) and VHF (AURORA/E-SKIP)
- Cropped lunar bitmap fills the frame; empty-phase caption no longer leaves a blank line

**Pan-follows-VFO with 5% edge threshold (#1476/#1477)**
- Opt-in toggle; VFO slides freely through the center 90% of the pan
- Pan only shifts when the VFO crosses the 5% edge on either side

**Memory dialog CSV bulk import (#1529)**
- Import memory channels from SmartSDR-compatible 22-column CSV with progress feedback
- Bulk selection and editing flow improvements (#1522)

**RAC (Canada) band plan overlay (#716)**

**Audio Boost toggle (#1445)**
- Optional gain boost in Radio Setup for low-volume AGC audio

**Configurable audio buffer cap (#1505)**
- User-tunable RX buffer size; DSP bypass for external TX paths

**Per-slice VFO marker style toggles (#1526/#1614)**
- Thin / thick / edges / hide choices per slice (restricted to CW/CWL modes)

**Extended frequency line (#1502)**

**rigctl TX PWR / SWR meters for WSJT-X and JTDX (#1594)**
- Expose transmit power and SWR over rigctld

**StreamController TX audio source selector (#1617)**
- MOX and PTT actions in the StreamController plugin can pick DAX vs mic source

**Smooth S-meter peak hold animation (#1501)**

### Bug Fixes

**TCI audio reliability for WSJT-X / JTDX**
- Fix TCI TX timing for WSJT-X (#1624) — 2048/48000 = 21.333ms cadence was rounding to 21ms
- Fix TCI audio for multi-slice: per-channel accumulation buffers (#1595/#1596)
- Fix TCI audio for WSJT-X: DAX stream lifecycle and frame format (#1439)
- Fix TCI trx command ignoring micpc audio source arg (#1534/#1535)
- Fix TCI server crash on stop due to double-free in client cleanup (#1532)
- Decouple TciServer TX gain from DaxTxGain (#1628)

**macOS Tahoe pop-out / re-dock UI freeze (#1344/#1345)**
- Clears stale native window state on reparent

**rigctld shutdown stability**
- Fix rigctld shutdown crash with active CAT clients (#1601)
- Schedule rigctld client sockets for deletion on server stop (#1605)

**PC Audio persistence and recovery**
- Fix PC Audio mute state not persisting across restarts or sleep/wake (#1571/#1572)
- Fix PC Audio silence after Teams/Zoom reconfigures audio endpoint (#1569/#1570)
- Keep local audio sink off when PC Audio is disabled but stream alive (#1622)
- Handle macOS Bluetooth audio hotplug safely (#1602)
- Fix macOS Bluetooth audio route handling (#1486)

**Panadapter and spectrum**
- Fix RF Gain echo-driven command loop (#1498/#1612)
- Fix RF gain / WNB controls affecting wrong panadapter (#1548)
- Fix VFO disappearing from pan area on startup (#1493/#1495)
- Preserve waterfall history on width change instead of blanking (#1608)
- Fix Band Stack Panel expanding main window width (#1487/#1488)
- Recenter panadapter when activating an off-screen slice (#1554/#1555)
- Fix noise floor mismatch between slices by syncing yPixels on resize (#1511)
- Center signal on VFO when zoom-in button/keyboard pushes it offscreen (#1550/#1551)
- Propagate late-arriving DIV eligibility to all VFOs (#1503/#1613)
- Fix PanadapterStream thread affinity (#1489)
- Fix RF Gain slider wiring and keyboard shortcut yield (#1497)

**FlexControl tuning jitter**
- Resolve optimistic/confirmed frequency race that caused knob jitter (#1524)

**CW**
- Fix CW macros requiring CWX window open and ESC not stopping TX (#1552/#1553)
- Fix CW decoder header jitter when WPM changes (#1546)
- Fix Autotune label leaking to non-CW modes and show marker row always (#1620)
- Restrict VFO marker-style buttons to CW/CWL modes and clean up on rebuild (#1615)

**Mic and TX**
- Fix Mic Gain resetting when toggling Processor state (#1573)
- Disable NR2 when Opus audio compression is active (#1597)

**TGXL and band selection**
- Fix TGXL showing wrong SWR after tune and meter freeze (#1530/#1531)
- Map WWV/GEN to their band-stack slot indexes (#1540/#1211 follow-up) (#1616)

**TNF on macOS (#1452)**
- Split setGlobalEnabled into status and command paths so TNF tracks on macOS

**MQTT TLS with OpenSSL 3.5+ (#1483/#1484)**

**Radio connection UX (#1545)**
- Better panadapter feedback during connect
- Larger connection dialog buttons and window
- Clarify disabled CAT/DAX menu items on unsupported platforms (#1556/#1557)

**Windows build**
- Guard autoCatAction / autoDaxAction refs in placeholder-action loop (#1618/#1619)
- Guard m_hidCoalesceTimer reference behind HAVE_HIDAPI (#1606)

## [v0.8.15] — 2026-04-15

### Waterfall Scrollback, Pan-Follow Animation, TCI-via-DAX, Community Contributions

### Features

**Waterfall history scrollback (#1432, community: rfoust)**
- 20-minute ring buffer with scrub controls on the right-side time strip
- Pull up on the time strip to scroll back in time; LIVE button to return
- Paused viewport ages forward as new rows arrive; reprojects on pan/zoom

**HF Propagation Dashboard**
- Click K/A/SFI on the spectrum overlay to open a rich dashboard
- 3-day Kp forecast, band conditions, VHF, X-ray, solar wind, SDO solar images, lunar phase

**Memory dialog search and CSV export (#1436, #1438, community: jensenpat)**
- Search field filters memories by name as you type
- Arrow key navigation with Enter to apply; double-click for inline edit
- CSV export in SmartSDR-compatible 22-column format

**5-8 pan layouts for FLEX-6700 (#1435)**
- New layouts: 3h2, 2x3, 4h3, 2x4 for up to 8 panadapters

**Active Slice Follows TX toggle (#1351)**
- Switch active/displayed slice when TX moves externally (e.g. WSJT-X)
- Mutually exclusive with TX Follows Active Slice; both can be off

**TCI audio via DAX (#1331)**
- TCI audio feeds from DAX instead of main RX path
- Muting PC audio no longer kills TCI client audio
- DAX RX streams created/torn down automatically on audio_start/stop

**Configurable frequency grid spacing (#1390, #1428)**
- Manual grid spacing option in spectrum overlay menu

**System sleep inhibition (#1420, #1427)**
- Prevent system sleep while connected to radio (macOS/Windows/Linux)

**Activate slice on passband click (#1422)**
- Left-click a slice passband to make it active

**S-meter color gradient (#1430, #1434)**
- SmartSDR-style green→yellow→red gradient on VFO and slice flag S-meter bars

### Bug Fixes

**Fix panadapter/VFO scroll jitter, blank spectrum, and floor level revert (#989, community: chibondking)**
- Smooth 200ms pan-follow animation with QVariantAnimation
- Stale echo-back guard prevents pre-animation center from reversing animation
- Per-pan levelChanged signal prevents stale echo-backs from reverting dBm floor
- VFO edge-flip 20px hysteresis prevents oscillation during animation

**Fix duplicate wirePanadapter connections (#989, community: chibondking)**
- Bulk disconnect(this) guard replaces scattered per-signal disconnects

**Fix MQTT "Socket is not connected" on macOS (#1348)**
- Handle MOSQ_ERR_CONN_PENDING for non-blocking connect
- Handle ENOTCONN on Unix/macOS in vendored libmosquitto

**Audio liveness watchdog (#1411)**
- Restart RX stream after 15 seconds of no audio data (fixes silent audio loss after hours of idle)

**Fix SIGSEGV on band change with 2 panadapters (#1433)**
- Null-guard spectrumForSlice() during pan creation/destruction

**Fix manual grid spacing (#1390)**
- Grid lines respect user's manual choice; only labels thin at narrow spacing

**Fix Windows audio silence (#1405, #1419)**
- Remove IdleState restart logic that caused audio restart loops on Windows

**Fix K-index regex capturing UTC time (#1401, #1410)**
- Anchor on "was" keyword to avoid capturing UTC time as K value

**Fix DAX TX stream conflict with SmartSDR DAX on Windows (#1394)**
- DAX RX stream lifecycle managed by TCI server; streams cleaned up on disconnect

### Contributors

Special thanks to the community contributors in this release:
- **Robbie Foust (rfoust)** — waterfall history scrollback
- **CJ Johnson / WT2P (chibondking)** — pan-follow animation, duplicate command fix
- **jensenpat** — memory search/navigation, CSV export

## [v0.8.12.2] — 2026-04-14

### Bug Fixes, Meter Smoothing, Maestro Disconnect Fix

### Bug Fixes

**Fix two-slice band-change crash (#1372)**
- Null-guard all spectrumForSlice() dereferences during pan creation/destruction

**Fix CW key/paddle not triggering TX (#1379)**
- Track CW key/paddle state in interlock handler so break-in keying isn't killed

**Fix popout panadapter frozen after reparent (#1381)**
- GPU render pipeline self-heals: re-initializes lazily on next render frame

**Fix WSJT-X DT drift on DAX RX audio after TX cycles (#537)**
- PipeWireAudioBridge: silence fill during TX keeps pipe clock advancing
- VirtualAudioBridge: wall-clock-accurate silence fill (eliminates QTimer jitter)

**Fix CW auto-tune buttons not shown for SSDR+ users (#1356)**
- Set license flag before VFO widget builds filter buttons

**Fix FlexControl knob press not returning to frequency mode (#1354)**
- Knob press while in Volume/Power wheel mode returns to Frequency

**Fix audio stopping after idle/screensaver with USB devices (#1361)**
- Zombie sink watchdog, IdleState handler, device list monitor (Windows only)
- PipeWire-safe: WASAPI-specific recovery gated behind Q_OS_WIN

**Send graceful client disconnect before closing TCP (#1359)**
- Prevents Maestro lockup on reconnect with same GUIClientID

**Fix panadapter bandwidth limits (#1385)**
- Per-model pcap-verified values; 8400 correctly grouped as single-SCU

**Fix waterfall tooltip descriptions (#1365)**
- Black level and rate tooltips corrected

**Fix AppImage crash on Arch/Fedora (#1362)**
- Removed bundled OpenSSL from AppImage (ABI conflicts with host PipeWire)

### Enhancements

**Asymmetric attack/release smoothing on all HGauge meters**
- 30ms attack, 180ms release on all 13 gauge instances (Level, Compression,
  ALC, Fwd Pwr, SWR, Temp, etc.)

**Smooth slice flag S-meter bar** (community: @rfoust)
- Same attack/release timing applied to VFO slice flag meter

### CI/CD

**Fix macOS Intel CI hang**
- Skip post-run checkout cleanup on ephemeral runners

---

## [v0.8.12.1] — 2026-04-14

### Hotfix: Bundle OpenSSL for Windows and AppImage

**Fix startup crash on Windows (#1341, #1362)**
- Bundle `libssl-3-x64.dll` and `libcrypto-3-x64.dll` in Windows installer and portable ZIP
- Bundle `libssl.so.3` and `libcrypto.so.3` in AppImage
- MQTT TLS support added in v0.8.12 linked OpenSSL dynamically but packaging did not include the runtime DLLs

---

## [v0.8.12] — 2026-04-14

### Major PR Review, Community Contributions, CW & TCI Improvements

### New Features

**QSO audio recorder (#1297)**
- Client-side WAV recording with auto-record on TX and idle timeout
- Radio Side / Client Side mode selector in Radio Setup → Audio
- Playback of last recording through speaker with live RX muted during playback

**TCI IQ stream support (#1182)**
- Stream DAX IQ data to TCI clients for panadapter/waterfall display
- Enables TCI Remote Android app (by ON7OFF) to show live spectrum

**Profile-based memory channel filtering (#1251)**
- Filter memories by global profile in the Memory Channels dialog
- New memories auto-tagged with active profile name

**Configuring AetherSDR Controls help guide**
- Comprehensive 659-line offline guide covering keyboard shortcuts, FlexControl,
  MIDI, USB HID devices, Stream Deck, and serial PTT/CW (community: jensenpat)

### Enhancements

**Spectrum & waterfall**
- Anchor frequency-bar drag zoom to cursor position (community: rfoust)
- Sync waterfall history on pan/zoom — existing rows shift to match (community: rfoust)
- Add WIDE indicator badge to spectrum display (community: jensenpat)
- Guard noise floor auto-adjust during TX to prevent waterfall disappearing (#1302)
- Smooth S-meter needle with asymmetric attack/release timing (community: rfoust)

**CW decoder & keying**
- CW Zero Beat button — client-side zero-beat for non-SmartSDR+ users (#1228)
- Close button on CW decoder panel (session-only, not persistent) (#1287)
- CPY ALL / CPY VIS buttons for copying decoded text (#1299)
- Nordic character support: Æ, Ø, Å (#1280)

**FlexControl**
- Knob button (X4S) as configurable action (#1295)
- Assignable wheel modes: Frequency, Volume, TX Power (#1282)

**Multi-slice & multi-pan**
- 8-slice support for Flex 6700 with 4 new slice colors (#1281)
- Slice tab toggle (A/B/C/D) in RxApplet header — inline for ≤4 slices (#1278)
- Fix popout panadapters on macOS/Windows — GPU rebind on reparent (#1240)
- Guard all null spectrum() dereferences in multi-pan mode (#1199)

**Network & audio**
- Richer network status tooltip with per-stream stats (community: rfoust)
- Non-modal Network Diagnostics dialog with audio playback section (community: rfoust)
- Restart RX audio sink when backend stops unexpectedly (#1303)
- Switch K-index source from hamqsl.com to NOAA SWPC WWV bulletin (#1255)

### Bug Fixes

**Fix XWayland GLX crash when opening child dialogs (#1233)**
- Auto-detect Wayland sessions and set QT_QPA_PLATFORM=wayland
- Add AETHER_NO_GPU=1 runtime fallback for software rendering
- PII redaction regex objects survive abnormal teardown

**Fix TCI audio corruption (#1239)**
- Header length now reports per-channel sample count per TCI v2.0 spec
- Detect mono vs stereo TX audio from WSJT-X
- Handle string format names in audio_stream_sample_type negotiation

**Fix antenna dropdown not populating in overlay menu (#1260)**
- Moved antenna list wiring to per-pan in wirePanadapter() with initial sync

**Fix band button reselection (#1284)**
- Removed same-band guard that blocked re-selecting GEN/transverter bands

**Fix frequency offset calibration Start button (#1237)**
- Added missing `radio calibrate` command

**Filter RX-only antenna ports from TX selector (#1238)**
- Skip ports starting with "RX" in TX antenna dropdown

**Guard QImage::scaled() null waterfall (#1250)**
- Prevents permanent waterfall loss during rapid resize on compositing WMs

**Fix audio input device combo (#1334)**
- Shows active device instead of system default in Radio Setup

### Contributors

- **rfoust** — zoom anchor, waterfall reprojection, S-meter smoothing, network diagnostics, pan-follow-VFO (in progress)
- **jensenpat** — WIDE indicator, controls help guide
- **chibondking** — pan-follow-VFO (in progress)
- **wa2n-code** — K-index NOAA source investigation
- **rskunath** — XWayland crash report with detailed logs
- **WX7Y** — TCI audio persistence and testing
- **LB2EG** — FlexControl X4S protocol documentation

---

## [v0.8.11] — 2026-04-12

### MQTT Station Integration, Triage Fixes, macOS DAX Fix

### New Features

**MQTT station device integration (#699)**
- Subscribe to MQTT topics and display device status in the applet panel
- User-defined publish buttons for rotator, antenna, and device control
- Panadapter overlay: prefix topics with * to show values on the spectrum
- Bundled libmosquitto — no system dependency needed
- Requested by VA3MW for Node-RED antenna/rotator integration

### Bug Fixes

**Antenna Genius race condition (#1213)**
- Always send saved antenna preference on band change; don't skip when AG
  coincidentally reports the correct antenna first (community: scott-mss)

**Heat map QPainter fallback (#1220, #1180)**
- Added heat map gradient to the QPainter spectrum path for systems where
  GPU rendering is unavailable (Windows Intel iGPU)

**DAX channel persistence (#1221)**
- Per-slice DAX channel saved/restored across restarts via AppSettings
- DAX IQ channel tracked on PanadapterModel from radio status; overlay synced

**macOS DAX audio corrupted (#1242)**
- VirtualAudioBridge and PCC_IF_NARROW DAX path updated to float32
- Fixes VARA HF / WSJT-X audio corruption on macOS (community: pepefrog1234)

**VFO filter label mismatch (#1225)**
- VFO header now shows filter width (hi − lo), not filter edge value

**TX power meter jitter (#980)**
- Asymmetric smoothing: fast attack, slow decay for stable TX meter

**Oscillator live-update (#967)**
- Radio Setup oscillator status updates live when external 10 MHz reference
  is plugged/unplugged (community fix: NF0T)

**K-index rounding (#1232)**
- Parse K/A-index as double for decimal XML values from hamqsl.com

**IQ Enable button state (#1221)**
- Reset DAX IQ buttons on reconnect (streams are per-session)

**File dialog z-order (#1011)**
- Background image chooser uses spectrum widget as parent

**Status bar cleanup (#1231)**
- Removed redundant "RADIO:" prefix label

**Cursor button removed**
- Removed from Display panel (redundant with Tune Guides feature)

**NR2 + Opus documented (#1222)**
- Help docs updated: NR2 incompatible with Opus, use RN2/NR4/DFNR instead

**macOS NSLocalNetworkUsageDescription (#1242)**
- Required for UDP broadcast radio discovery on modern macOS

### Contributors

- **pepefrog1234** — macOS DAX float32 fix + NSLocalNetworkUsageDescription
- **scott-mss** — Antenna Genius bug report with detailed reproduction
- **NF0T** — Oscillator live-update fix
- **jensenpat** — VPN source-path binding (v0.8.10), AGC off level, CMake fix
- **wa2n-code** — K-index rounding report, NR2+Opus investigation

---

## [v0.8.10] — 2026-04-11

### Pop-Out Panadapters, VPN Source-Path Binding, Float32 Fixes

### New Features

**Pop-out panadapters (community: rfoust)**
- SmartSDR-style title bar with pop-out (⬈), maximize (□), and close (×) icons
- Right-click "Pop out" in spectrum context menu to detach into floating window
- Dock back via title bar button; multi-pan mode updates automatically

**VPN source-path binding (#1218, community: jensenpat)**
- Manual connections can select a specific network interface for TCP+UDP
- Auto-follows TCP path for VPN/routed radios on multihomed systems
- Per-target profile persistence in RoutedProfilesJson
- Network Diagnostics dialog shows target IP, source path, TCP/UDP endpoints
- 250ms UDP registration retry for routed connections

**Show Tune Guides (#1198)**
- Visual tune guides in spectrum/waterfall context menu

**Reset settings cleanup flow (#1207)**
- Guided settings reset from Help menu

**Expanded Slice Troubleshooter (#1206)**
- Additional diagnostics in troubleshooter dialog

**Center startup slice (#1204)**
- Slice centered on panadapter at launch

**Center go-to-frequency (#1203)**
- Frequency entry centers pan on target

### Bug Fixes

**AGC off level in VFO widget (#1217, community: jensenpat)**
- AGC slider routes to `agc_off_level` when AGC mode is Off
- Tooltip and accessible name update to match active mode

**AGC off level in RX applet (#1183)**
- Same fix for the RX applet AGC slider

**CW decoder broken by float32 (#1191)**
- Float32→int16 conversion at ggmorse boundary

**DAX audio path**
- DAX emission converted to float32 for TCI/PipeWire clients
- PipeWire bridge reads float32 input correctly (fixes pumping audio)

**CW autotune stop**
- Separated one-shot and loop commands; stop now sends `int=0`

**TX audio broken by float32 (#1175)**
- Restored TX mic path to int16; separate RX/TX audio formats

**Auto-save command (#1195)**
- Uses correct `profile autosave on/off` command; reads from radio status
- Support dialog made modeless

**Multi-Flex UDP port conflict**
- Countdown fallback tries ports 4991, 4990, 4989...

**Band menu reopen (#1209)**
- Fixed band menu not reopening after close

**Stop persisting LCK (#1205)**
- Lock state no longer saved across restarts

**What's New auto-popup removed**
- No longer pops up on launch; available via Help → What's New

### Infrastructure

- Skip this version button in upgrade dialog
- CMake find_package fix (#1210, community: jensenpat)
- CI dependency bumps: actions/cache v5 (#1177), dorny/paths-filter v4 (#1178)

### Contributors

- **rfoust** — Pop-out panadapters (foundation code)
- **jensenpat** — VPN source-path binding, AGC off level in VFO, CMake fix

---

## [v0.8.9] — 2026-04-11

### Float32 Audio, Display Panel Redesign, Community PR Blitz

### New Features

**Float32 audio pipeline**
- End-to-end float32 from radio to speaker — eliminates distortion at high AF slider levels
- All DSP filters (NR2, RN2, NR4, DFNR, BNR) now process in native float32
- Resampler converted to float32 I/O (r8brain double precision preserved internally)
- Removes all unnecessary int16↔float conversion overhead

**Adjustable spectrum trace thickness (#1116)**
- GPU triangle strip expansion for variable-width spectrum line (0.5–5.0 px)
- Line Width slider in Display panel, persisted per-pan
- Setting to "Off" hides trace entirely, showing only fill gradient

**Display panel redesign**
- Compact layout: sliders grouped at top, toggle buttons condensed into labeled row
- Heat Map, Grid, Weighted Average, Cursor Freq as toggle buttons with color state
- Removed separator between FFT and waterfall sections

**Band Stack panel with frequency bookmarks**
- Vertical bookmark strip alongside panadapter with FRStack-style save/restore
- Saves frequency, mode, filter, antennas, AGC, volume, NB/NR/WNB per bookmark
- Color-coded by band plan segment, persisted per-radio

**CPU and memory indicator (#1056)**
- Cross-platform process CPU and RSS in status bar

**Panadapter zoom buttons (#1050)**
- −/+ buttons and macOS trackpad pinch-to-zoom (community: rfoust)

**Collapsible VFO flags (#1166)**
- Click slice letter badge to collapse/expand (community: rfoust)

**VU meter popout (#993)**
- S-Meter can be detached into floating window (community: chibondking)

**Off-screen slice context menu**
- Right-click off-screen indicators: Close, Move Here, Center (community: rfoust)

### Bug Fixes

**Radio-authoritative band changes (#1093)**
- Band buttons use `display pan set band=` — radio manages band stack
- Bandwidth drag no longer snaps pan center to VFO

**PC Audio toggle vs StreamDeck TCI**
- StreamDeck no longer overrides user's explicit PC Audio off toggle

**VFO drag snap (#1120)**
- Passband drag moves VFO relative to grab point, no initial frequency jump

**DFM/FM filter centering (#1122)**
- FM/NFM/DFM filters centered symmetrically instead of USB-style offset

**Multi-pan band change crash (#1146)**
- Null guards for spectrum() during pan reassignment

**HGauge meter colors (#1152)**
- Green/yellow/red zones with white labels for readability
- Yellow caution zones added to all gauges

**Logging checkboxes not persisting**
- Fixed dots in XML keys; default Discovery/Commands/Status to enabled

**Status bar RADIO label (#1113)**
- Renamed STATION to RADIO; station name defaults to OS hostname for multi-flex

**Aurora power meter (#484)**
- AU- models show 0-600W scale instead of 0-120W

**FlexControl stale QSY (#1098)**
- Reset target on external frequency change; added AGC/volume button actions

**macOS CoreAudio crashes (#1059, #1114, #1149)**
- Guard stop() in StoppedState; route device switch to audio thread; restart on silent stop

**PROC turning off on VOX toggle (#1104)**
- Investigated: cannot reproduce on firmware v4.1.5

**Additional fixes**
- Mic bias/boost UI update (#1045)
- DFNR reset button (#1055)
- Support bundle firmware version (#1057)
- GuardedSlider lock bypass (#1060)
- VFO filter BW for DIGU/DIGL (#1066)
- CMake GPU/DFNR diagnostics (#1067)
- TCI PC Audio button state (#1071)
- TGXL connection errors (#1039)
- Manual probe radio identity (#1072)
- Windows QRhi cursor (#1096)
- Grid lines toggle (#1065)
- AGC Threshold tooltip (#1064)

### Infrastructure

**Vendored RADE Opus snapshot**
- Offline builds — no network download needed (community: jensenpat)

**A-index in propagation overlay**
- Shows alongside K-index and SFI (community: jensenpat)

### Contributors

- **rfoust** — Zoom buttons, collapsible VFO flags, off-screen slice menu
- **chibondking** — VU meter popout
- **jensenpat** — Vendored Opus, A-index, spot colors, memory persistence, full frequency display, macOS DAX driver check
- **AetherClaude (pi-claude)** — Multiple bug fixes and features

## [v0.8.8] — 2026-04-10

### Band Stack Panel, Radio-Authoritative Bands, Cross-Platform CPU Monitor

### New Features

**Band Stack panel**
- Vertical bookmark strip alongside the panadapter, toggled by 3-dot icon in status bar
- Click "+" to save current frequency, click a bookmark to recall, right-click to delete
- Saves/restores: frequency, mode, filter, RX/TX antenna, AGC, volume, NB, NR, WNB
- Color-coded by band plan segment (CW=blue, SSB=orange, Data=red)
- Persisted per-radio in BandStack.settings (XML, atomic save)
- Window expands to accommodate panel without affecting waterfall

**CPU and memory indicator (#1056)**
- Process CPU usage and RSS memory in the status bar
- Cross-platform: getrusage (Linux/macOS), GetProcessTimes (Windows)
- Color-coded: blue < 50%, yellow 50-79%, red 80%+

**Panadapter zoom buttons (#1050)**
- −/+ buttons below S/B for stepped bandwidth zoom (1.5× per click)
- macOS trackpad pinch-to-zoom with cursor-anchored zooming
- Per-radio bandwidth limits (FLEX-8600: 14 MHz, 8400: 7 MHz, others: 5.4 MHz)

**Grid lines toggle (#1065)**
- Show/hide grid lines via Display panel toggle
- Persisted per-panadapter

**AGC Threshold tooltip (#1064)**
- Dynamic tooltip showing current AGC threshold value on slider hover

### Bug Fixes

**Radio-authoritative band changes (#1093)**
- Band buttons now use `display pan set band=` — radio manages its own band stack
- Removed ~200 lines of client-side BandStack AppSettings save/restore
- Bandwidth drag no longer snaps pan center to VFO

**FlexControl stale QSY + missing buttons (#1098)**
- Dial no longer jumps back to previous band after external frequency change
- Added ToggleAgc, VolumeUp, VolumeDown to FlexControl button actions

**TCI PC Audio button state (#1071)**
- PC Audio button now reflects TCI-forced stream state
- Button returns to saved preference when TCI disconnects

**TGXL connection errors (#1039)**
- Shows error text in Peripherals tab when TCP connection fails
- Pre-fills radio-discovered TGXL IP as fallback

**Logging checkboxes not persisting**
- Fixed: dots in category IDs were rejected by XML element name validator
- Checkboxes now initialize from saved state on dialog open
- Discovery, Commands, Status default to enabled

**Mic bias/boost UI not updating (#1045)**
- Added missing phoneStateChanged() emit after optimistic update

**DFNR reset button (#1055)**
- Wired onReset callback in both right-click popup and DSP dialog

**Support bundle firmware version (#1057)**
- Reports both protocol version and firmware version

**macOS CoreAudio crash (#1059)**
- Guard stop() calls against StoppedState when switching audio devices

**GuardedSlider lock bypass (#1060)**
- Block mouse drag on sliders when controls are locked (was only blocking wheel)

**VFO filter BW label for DIGU/DIGL (#1066)**
- Show actual bandwidth instead of upper filter edge for digital modes

**CMake diagnostics for GPU/DFNR (#1067)**
- Properly disable GPU rendering when Qt6GuiPrivate not found
- Improved DFNR disabled message pointing to setup-deepfilter.sh

**Windows invisible cursor (#1096)**
- Re-apply cursor in QRhiWidget::initialize() after HWND creation

**Manual radio probe identity (#1072)**
- Read S radio status line for real model/serial/nickname on VPN connections

### Contributors

- rfoust — Panadapter zoom buttons and pinch-to-zoom (#1108)
- AetherClaude (pi-claude) — Multiple bug fixes and features

## [v0.8.7] — 2026-04-09

### TCI Audio Fix, Elgato Stream Deck Plugin, Help Guides

### New Features

**Elgato Stream Deck plugin**
- Native plugin for the official Elgato Stream Deck app (macOS/Windows)
- 43 actions: TX, bands, modes, DSP, audio, slice controls, DVK
- Pre-built distributable — download and double-click to install
- StreamController plugin (Linux) also attached to releases
- Automated workflow attaches both plugins to every release

**Help guides**
- Understanding Noise Cancellation guide (NR, NR2, RN2, NR4, DFNR, BNR comparison)
- Configuring Data Modes guide (CAT, TCI, DAX walkthroughs for WSJT-X, JTDX, fldigi)

**DEXP persistence (#1004)**
- Optimistic updates for DEXP on/off and level (no status echo from radio)
- Saved to AppSettings on change, restored on connect

### Bug Fixes

**TCI audio absent when using Radio Audio (#1014)**
- remote_audio_rx stream always created on connect regardless of PC Audio state
- PC Audio toggle now only controls local playback, never removes the stream
- TCI clients receive audio whether PC Audio is on or off

**Keyboard tuning fixes (#1005)**
- Right arrow tuned wrong direction (stale base frequency)
- Arrow keys now auto-repeat when held for continuous tuning
- Uses tuneAndRecenter() for proper auto-center behavior

**TX Delay not persisting (#996)**
- Wired editingFinished signals for all interlock timing fields (ACC TX, TX Delay, RCA TX1/2/3, Timeout)

**Duplicate Preferences menu entry (#1013)**
- Set PreferencesRole directly on Radio Setup action — one entry on all platforms

**TUN/AMP/AG applet state not persisting (#1042)**
- Save checked state to AppSettings on toggle (was missing for hardware-conditional applets)

**Mic bias/boost not reflecting toggle (#951)**
- Optimistic update for mic bias and mic boost (radio sends no status echo)

**RADE status label not appearing (#1049)**
- Used vfoWidget(sliceId) instead of null vfoWidget() alias
- Label now inline with frequency display instead of separate row

**Finer mouse-wheel steps for Controls sliders (#1026)**
- Use singleStep (1) instead of pageStep (10) for slider wheel events

**DFNR model not found on system install (#1003)**
- Added XDG, /usr/share, /usr/local/share search paths
- CMake install() rule for model file

**Intel Mac GPU rendering**
- Disabled QRhiWidget on Intel Mac DMG builds (rendering issues on older Metal/OpenGL)

### Removed

**Native StreamDeck integration**
- Replaced by TCI-based Elgato and StreamController plugins

### Contributors

- jensenpat — Keyboard tuning fixes, noise cancellation and data modes help guides
- NF0T (Ryan B) — RADE label fix, mic bias/boost fix, applet persistence fix
- AetherClaude (pi-claude) — TX Delay, slider steps, DFNR model paths

## [v0.8.6] — 2026-04-08

### DFNR AI Noise Reduction, Propagation Overlay, Community PR Blitz

### New Features

**DFNR (DeepFilterNet3) AI noise reduction (#970)**
- Fifth client-side NR option using deep neural filtering
- CPU-only, ~10ms latency, excellent in high-noise HF environments
- Configurable attenuation limit and post-filter beta
- Full mutual exclusion with NR2/RN2/NR4/BNR

**HF propagation conditions overlay (#986)**
- K-index and Solar Flux Index displayed on panadapter
- Fetched from hamqsl.com, cached hourly, persists across restarts
- Toggle via View > Propagation Conditions

**Peripherals tab in Radio Setup (#914)**
- Manual IP connect for TGXL, PGXL, and Antenna Genius
- Auto-connect on radio connect, live status indicators
- Pre-fills IP/port from discovered connections

**License Info section in Radio Setup (#979)**
- Shows subscription tier (SmartSDR/SmartSDR+/SmartSDR+ EA)
- Radio ID, expiration date, licensed firmware version
- Parses all 3 license tiers from subscription status messages

**UI scale keyboard shortcuts (#892)**
- Ctrl+=/Ctrl+- zoom in/out, Ctrl+0 reset to 100%
- Restart prompt to apply scale change

**Reset to Defaults button (#981)**
- One-click reset of all display settings
- Clear button restores default logo background

**Autostart immediate toggle (#863)**
- CAT, DAX, rigctld start/stop immediately when toggled

### Bug Fixes

**TX audio always Opus (#932)**
- Radio requires Opus for remote_audio_tx regardless of setting
- Fixed regression where SmartLink TX required DAX ON

**Crash with 2+ panadapters (#895)**
- Per-stream waterfall frame assembly prevents cross-stream corruption

**Crash on VPN connect (#894)**
- UDP stream setup result check with graceful disconnect on failure
- 10-second health check warns if no VITA-49 data received

**Floating applets losing state (#959)**
- Four distinct bugs fixed: toggle off/on, window drift, WM centering race, shutdown race
- P/CW applet '/' in ID was silently corrupting entire settings file

**FlexControl VFO jitter (#693)**
- Track absolute target frequency instead of re-reading slice during coalesce

**Serial PTT/CW non-functional on macOS (#884)**
- QSerialPort parent ownership for proper moveToThread behavior

**Space bar triggering Tune when shortcuts disabled (#928)**
- Space always consumed to prevent accidental button activation

**RADE selecting DIGL on 60m (#875)**
- Uses band definitions for correct sideband (60m is USB)

**FM tone/repeater UI not updating after memory recall (#879)**
- Normalize tone mode/value/offset to match combo box item data

**macOS menu Preferences opened Shortcuts dialog (#883)**
- Explicit QAction::PreferencesRole wiring for Radio Setup

**macOS DMG workflow YAML parse error**
- Indented heredoc XML to fix GitHub Actions YAML parser

**FLEX-6500 capped at 2 panadapters (#953)**
- Added FLEX-6500 to dual-SCU model list for 4 pan support

### Removed

**Native StreamDeck integration**
- Replaced by TCI-based StreamController plugin
- HIDAPI retained for HID encoder support (FlexControl etc.)

### Contributors

- JJ Boyd ~ KG4VCF (boydsoftprez) — DFNR, macOS/Windows testing
- jensenpat — Propagation overlay, Reset to Defaults, network consent fix
- CJ Johnson (chibondking) — Floating applet persistence fixes
- Ryan B / NF0T — License Info section
- AetherClaude (pi-claude) — 12 bug fix PRs

## [v0.8.5] — 2026-04-06

### Poppable Applets, Compression Meter, CW Filter Fixes

### New Features

**Poppable applets (#916)**
- Float/dock applets with persistent geometry across restarts

**StreamController plugin full action set (#838)**
- 40+ TCI actions for Elgato Stream Deck

**DXLab SpotCollector integration (#795)**
- UDP listener for spot forwarding

**Peak hold reset button (#840)**
- Tie hold time to decay presets

### Bug Fixes

- Fix compression meter: pass raw COMPPEAK dBFS to gauges (#877)
- Fix S-Meter oscillating between RX/TX during receive (#910)
- Fix CW filter low-edge drag snapping to zero (#764)
- Fix TCI RX/TX audio for WSJT-X and JTDX (#762)
- Fix SmartLink TX audio artifacts with compression param (#869)
- Fix TX status bar indicator not lighting up (#902)
- Fix crash from setAccessibleName on null ESC widgets (#899)
- Fix DIGU/DIGL filter offset centering (#904)
- Fix PROC speech processor commands (#861)

## [v0.8.4] — 2026-04-06

### NR4 Noise Reduction, Intel Mac, Waterfall Improvements

### New Features

**NR4 (Specbleach) noise reduction (#796)**
- AetherDSP settings dialog with NR4 controls and tooltips
- NR2 gain/NPE/AE filter controls ported from WDSP

**Intel Mac DMG build (#782)**
- Qt built from source for macOS 13 (Ventura) support
- Cached Qt source builds in CI

**Waterfall color scheme selection (#733)**
- Multiple color schemes with persistence

**Offline help system (#797)**
- Operator guides bundled with the app

**Compact DSP overlay (#798)**
- Toggle-only buttons in single row at spectrum top

### Bug Fixes

- Fix VFO flag direction in GPU rendering (#712)
- Fix split TX slice frequency override (#789)
- Auto-disable NR2/RN2/BNR in CW/CWL modes (#784)
- Route TUNE commands through TransmitModel for inhibit (#694)
- Fix background image distortion during resize (#737)
- Fix WNB/RF gain overlay layout (#815)
- Fix Intel Mac DMG crash on macOS 13 (#801)
- Fix mode buttons toggling off on repeated clicks (#813)
- Improve waterfall auto-black with trimmed-mean estimation (#788)

## [v0.8.3] — 2026-04-05

### Hardware Meters, Filter Fixes, CI Hardening

### New Features

**Radio hardware meter applet (#746)**
- PA temperature, supply voltage, fan speed display

**Slice troubleshooting diagnostics (#776)**
- Debug dialog for slice state inspection

**Spot lifetime slider (#758)**
- Non-linear stepped slider for shorter spot lifetimes

### Bug Fixes

- Fix DAX TX external PTT, SpotHub hang, scroll-to-tune step (#780)
- Fix cursor frequency label not persisting (#774)
- Fix macOS crash from rapid setCursor calls (#773)
- Fix TCI server not sending state to clients (#739)
- Fix PROC compression gauge not displaying (#754)
- Fix filter passband widget edge drag (#689, #684)
- Fix background opacity slider in GPU mode (#750)
- Fix 3v/4v pan layouts not restoring on reconnect (#757)

## [v0.8.2] — 2026-04-05

### PGXL Integration, PROC Fix, GPU Polish

### New Features

**PGXL direct connection**
- Direct TCP connection to PowerGeniusXL on port 9008
- Amplifier applet: drain current, mains voltage, MEffA status
- OPERATE/STANDBY button matching TGXL style
- S-Meter power gauge fed from PGXL peakfwd during TX
- TX mode driven by PGXL state (TRANSMIT_A/B)
- 5 Hz poll rate for responsive metering

**UTC date in status bar (#713)**
- Stacked date (yyyy-MM-dd) above time in status bar

**4m band support (#695)**
- 70 MHz band definition for FLEX-6700 XVTR (Region 1)

**Memory spots on panadapter (#704)**
- Saved memories as clickable spots, toggle in SpotHub Display

### Bug Fixes

**PROC/compander fix (#661, #667)**
- PROC button now sends compander commands (was speech_processor)
- NOR/DX/DX+ slider sends compander_level
- Mic profile PROC level restores correctly

**S-Meter reads 0W during TX with PGXL**
- Exciter txMetersChanged skipped when amplifier present
- S-Meter TX mode driven from PGXL state, not moxChanged

**GPU rendering polish (#722, #725, #726, #729, #730, #731)**
- Cursor frequency label restored in GPU mode
- WNB/RF gain indicators update correctly (markOverlayDirty)
- Waterfall syncs on band change
- Fill gradient matches QPainter fallback (bright at line, dark at base)
- Heatmap toggle persisted in AppSettings

**Batch memory spot removal (#708)**
- Signal blocked during loop, single rebuild after

**Dark/Light theme toggle removed (#686)**
- Placeholder menu item was confusing users

### Build System

- macOS: arm64 only (Intel via Rosetta), dropped PKG installer

### Community Contributions

- @boydsoftprez — GPU fill gradient, cursor label, WNB/waterfall sync
- @jensenpat — memory spots on panadapter

## [v0.8.1] — 2026-04-05

### macOS Metal Fix + Memory Spots

### Bug Fixes

**GPU rendering on macOS Metal (#702, #714)**
- Set Metal API explicitly and WA_NativeWindow for QRhiWidget
- QRhiWidget in a RasterSurface hierarchy silently fails without a native NSView
- macOS workflows switched from aqtinstall to Homebrew-only Qt (eliminates version conflicts)

**Memory spots on panadapter (#704)**
- Radio memories displayed as clickable spots on the panadapter
- Click to tune + apply mode/step/filter from saved memory
- Toggle in SpotHub Display settings
- Full sync: add/edit/remove in Memory dialog updates spots immediately

### Build System

- macOS: dropped aqtinstall, Homebrew-only for all dependencies
- macOS: added Homebrew prefix to CMAKE_PREFIX_PATH for linker discovery
- macOS: skip Qt6GuiPrivate find_package on Apple to avoid Homebrew/Qt conflicts

## [v0.8.0] — 2026-04-04

### GPU-Accelerated Rendering — The Performance Release

**This release transforms AetherSDR's rendering pipeline from CPU-bound QPainter
to GPU-accelerated QRhi, reducing main thread CPU usage from 97% to 28%.**

### GPU Rendering (#391)

**GPU-accelerated spectrum/waterfall via QRhi**
- Waterfall rendered as GPU texture with incremental row uploads (~6KB/frame)
- Ring buffer scrolling via fragment shader — zero CPU memmove
- FFT spectrum as GPU vertex buffer with per-vertex colors
- Overlays (grid, band plan, scales, markers) cached as QPainter texture, uploaded only on state change
- Platform-agnostic: OpenGL (Linux), Metal (macOS), D3D11 (Windows)

**Heat map FFT display**
- Intensity-based gradient: blue (weak) → cyan → green → yellow → red (strong)
- Fill gradient fades from full at bottom to transparent at spectrum line
- Toggle between heat map and solid color mode in Display panel
- Fill slider controls opacity in both modes

### New Features

**TCI WebSocket server (#528)**
- Full TCI v2.0 protocol: 72 command handlers over a single WebSocket connection
- CAT control, RX/TX audio streaming with r8brain resampling, IQ streaming
- Sensor telemetry, CW keyer, spot injection
- DIGI applet controls, autostart option
- First FlexRadio client with built-in TCI

**Customizable filter width presets (#675)**
- Right-click any filter button to set custom width in Hz
- Per-mode persistence, synced between VFO widget and RX applet

**Scrollable stepped controls (#673)**
- Step size, TX Low Cut/High Cut, RTTY Mark/Space respond to scroll wheel

**PA temperature unit toggle (#679)**
- Click PA temp label to toggle °F/°C

### Bug Fixes

**RTTY mark/space overlay (#660, #683)**
- RF_frequency in RTTY mode is the mark (radio applies IF shift)
- Green dashed M line, red dashed S line (MMTTY convention)
- VFO flag offset past filter edge, "Shift" → "Space" label

**Combo box scroll wheel regression (#676)**
- Popup visibility check: ignore wheel when closed, allow when open

**Step size not applied on restart (#666)**
- syncStepFromSlice missing emit stepSizeChanged

**Clock 00:00:00z with GPS no lock (#682)**
- GPS time only used when GPSDO installed AND locked

**DAX TX latency on PipeWire (#671)**
- 2KB pipe buffer, 5ms precise timer, drain loop

**AI-assisted issue reporter (#677)**
- Bug report + feature request templates, duplicate check

**RADE DSP guard**
- NR2/RN2/BNR disabled when RADE active

**RADE RX audio fix (#687)**
- Prevents VITA-49 audio fighting RADE decoded speech

**DXCC ADIF parsing (#670)**
- Static regex caching bug, band normalization, mode inference

**Shortcut dedup (#665)**
- Duplicate key bindings resolved on load

### Build System

- GPU rendering enabled by default, auto-disables on Qt < 6.7
- All release workflows updated with qtwebsockets, qtshadertools
- CI Docker image includes Qt6 private headers and shader tools
- Comprehensive dependency list in README for all platforms

### Community Contributions

- @jensenpat — shortcut dedup, sortable memory, macOS hidapi, PA temp toggle
- @Chaosuk97 — DXCC ADIF parsing
- @SA7LAV — DAX TX latency fix
- @tmiw — RADE RX audio fix

## [v0.7.19] — 2026-04-04

### TCI Server, RTTY Fix, Community PRs

### New Features

**TCI WebSocket server (#528)**
- Full TCI v2.0 protocol: 72 command handlers over a single WebSocket connection
- CAT control, RX/TX audio streaming, IQ streaming, CW keying, spot injection
- Per-client audio format negotiation via r8brain resampler (8/12/24/48 kHz)
- Sensor telemetry: S-meter, TX power/SWR/mic
- Auto-broadcast: VFO, mode, filter, TX state, lock, spot clicks
- DIGI applet: TCI enable toggle, port, client count
- Settings menu: Autostart TCI with immediate start/stop
- Validated with WSJT-X Improved and wscat

**Customizable filter width presets (#675)**
- Right-click any filter button to set a custom width in Hz
- Exact value stored, display rounded (e.g. 1234 → 1.2K)
- Per-mode persistence via AppSettings
- VFO widget (8 presets) and RX applet (6 presets) stay in sync
- "Reset to Defaults" reverts to hardcoded presets

**Scrollable stepped controls (#673)**
- Step size, TX Low Cut, TX High Cut respond to mouse scroll wheel
- RTTY Mark and Space labels respond to mouse scroll wheel

### Bug Fixes

**RTTY mark/space overlay at wrong frequency (#660, #683)**
- RF_frequency in RTTY mode is the mark (radio applies IF shift), not the carrier
- Green dashed mark line, red dashed space line (MMTTY convention)
- VFO center line hidden in RTTY/DIGL, replaced by M/S lines
- VFO flag offset past filter edge to not cover passband
- Filter presets center between mark and space tones
- "Shift" label renamed to "Space"

**Combo box scroll wheel regression (#676)**
- GuardedComboBox now checks popup visibility: ignore wheel when closed, allow when open
- Fixes both accidental value changes and broken dropdown scrolling

**Step size not applied to scroll-to-tune on restart (#666)**
- syncStepFromSlice was missing emit stepSizeChanged — SpectrumWidget kept stale value

**Clock showing 00:00:00z when GPS has no lock (#682)**
- GPS time only used when GPSDO is installed AND locked, otherwise system UTC

**AI-assisted issue reporter improvements (#677)**
- Supports both bug reports and feature requests
- Mandatory duplicate check, acceptance criteria section
- Split submit buttons: "Submit Your Idea" / "Report a Bug" with correct templates

**DAX TX latency on Linux/PipeWire (#671)**
- Reduced pipe buffer to 2KB, 5ms precise timer, drain loop

**RADE DSP guard**
- Client-side DSP (NR2/RN2/BNR) disabled when RADE is active

### Community Contributions

- @jensenpat: Normalize duplicate shortcut bindings (#665)
- @jensenpat: Sortable memory dialog columns (#668)
- @jensenpat: Fix hidapi include dirs on macOS (#669)
- @jensenpat: PA temperature unit toggle °F/°C (#679)
- @Chaosuk97: Fix DXCC ADIF parsing and mode inference (#670)
- @SA7LAV: DAX TX latency fix (#671)
- @tmiw: RADE RX audio fix — prevent duplicate samples (#687)

## [v0.7.18.6] — 2026-04-04

### Native StreamDeck, HID Encoders & UX Polish

### New Features

**Native StreamDeck support (#647)**
- Auto-detect all Elgato StreamDeck models (Mini through Plus XL)
- Live key rendering at 10 Hz: frequency, mode, band, TX state, DSP toggles
- Button actions: band select, mode select, SPLIT, LOCK, MUTE, MOX, TUNE, ATU, DSP toggles
- Dial actions (Plus/Plus XL): VFO tune, AF gain, RF power, squelch, RIT, XIT
- Touchscreen: frequency + mode display
- Image caching: only sends changed keys to device
- Configuration dialog (Settings → StreamDeck)
- Brightness control with debounce

**USB HID encoder support (#616)**
- Icom RC-28, Griffin PowerMate, Contour ShuttleXpress/ShuttlePro v2
- Auto-detect, 5ms poll, hotplug reconnect, configurable buttons
- udev rules for non-root access

**Scrollable RIT/XIT controls (#619)**
- Scroll wheel over Hz label adjusts by 10 Hz steps

**Filter passband edge drag (#628)**
- Click near left/right edge to drag independently in FilterPassbandWidget

**GuardedComboBox across all applets (#618)**
- Combo boxes consume scroll wheel events at boundaries

### Bug Fixes

**External 10MHz reference showing as TCXO (#606)**
- Radio sends `state=external`, we only checked `state=ext`

**TGXL power meter zero with PGXL (#600)**
- Re-scan AMP meter routing when TGXL handle arrives

**Squelch disabled in CW mode (#285)**
- Radio locks squelch in CW; controls now dimmed

**AmpApplet double title bar**
- Removed internal title bar, uses AppletPanel drag title bar

**Platform-aware title bar layout**
- Centered on Linux/Windows, left-aligned on macOS

**Feature request dialog modeless (#652)**
- Dialog stays open while using AI in browser
- Clipboard not overwritten on reopen

**Paginated issue fetch in lightbulb prompt (#650)**
- AI assistants now paginate through all open issues

**Step cycle wraps (#647)**
- Dial push step size cycles from max back to min

### Community PRs

**RADE RX audio fix (PR #630 by @tmiw)**
- Match output sample count to input size

**PC mic TX audio (PR #623 by @boydsoftprez)**
- Opus VITA-49 padding fix, mono USB mic, r8brain resampling

**TGXL meters from direct TCP (PR #627 by @boydsoftprez)**
- Parse fwd/swr from port 9010 R responses

**MinGW build fix (PR #622 by @boydsoftprez)**
- TCP_INFO_v0 and _dupenv_s compatibility

**macOS mic TX (PR #642 by @boydsoftprez)**
- Platform-aware sample rate negotiation

**macOS title bar (PR #641 by @jensenpat)**
- Left-align identity cluster on macOS

**Go-to-frequency fix (PR #644 by @jensenpat)**
- Resolve VFO through active slice, defer focus, tuneAndRecenter

### Documentation

**README** streamlined (357 → 130 lines)
**CONTRIBUTING.md** rewritten (517 → 213 lines)
**SECURITY.md** scope updated (SmartLink, WebSocket)
**Wiki** StreamDeck page added

## [v0.7.18.3] — 2026-04-03

### Background Image, Community PRs & Bug Fixes

### New Features

**Background image behind FFT spectrum (#611)**
- Custom background image with adjustable dark overlay (0-100% opacity)
- Bundled AetherSDR logo as default background
- Choose/Clear buttons in Display overlay panel, persisted across sessions

**Dynamic contributors in About dialog**
- Live contributor list fetched from GitHub API with scrollable inset
- Falls back to hardcoded list when offline

**GPG signed commits badge**
- README badge linking to commit history

### Bug Fixes

**External 10MHz reference showing as TCXO (#606)**
- Radio sends `state=external` but we only checked `state=ext`

**TGXL power meter zero with PGXL connected (#600)**
- AMP meter definitions arrived before TGXL handle was known, routing all
  meters to PGXL. Re-scan meter routing when handle arrives.

**TX Band Settings grid lines and dedup (#533)**
- Visible grid lines on all cells, removed duplicate dialog from Radio Setup

**GuardedComboBox across all applets (#618)**
- All combo boxes now consume scroll wheel events at boundaries
- Prevents accidental mode/antenna/profile changes from scroll leaking

### Community PRs

**Fix PC mic TX audio (PR #623 by @boydsoftprez)**
- Opus VITA-49 padding fix — trailing zeros corrupted radio's decoder
- Mono USB mic support with extended fallback chain
- r8brain resampling for TX downsampling (Fixes #363, #387)

**Fix TGXL Fwd Power/SWR gauges (PR #627 by @boydsoftprez)**
- Parse meters from TGXL direct TCP connection (port 9010)
- TGXL reports power/SWR via R responses, not VITA-49 (Fixes #625)

**Fix MinGW build (PR #622 by @boydsoftprez)**
- Add TCP_INFO_v0 and SIO_TCP_INFO definitions for MinGW
- Replace MSVC-only _dupenv_s with std::getenv

### Documentation

**README streamlined** — 357 → 130 lines, condensed feature list, removed shipped roadmap

**CONTRIBUTING.md rewritten** — 517 → 213 lines, added thread architecture table, widget guidelines, optional dependency flags

**SECURITY.md** — added SmartLink credentials and WebSocket endpoints to scope

## [v0.7.18.2] — 2026-04-03

### Quality of Life & Issue Triage

### New Features

**Clickable multiFLEX badge (#599)**
- Click the green "multiFLEX" badge in the title bar to open the dashboard
- Hover highlight and pointing hand cursor

**SAM, NFM, DFM keyboard shortcuts (#601)**
- All radio-supported modes now bindable in View → Configure Shortcuts

**Auto-switch mode on spot click (#424)**
- Clicking a spot with Auto Mode enabled switches slice to match the spot's mode
- Mode extracted from comment text (RBN/POTA/Cluster) or inferred from frequency
- Toggle in SpotHub → Display tab, Mode column added to Spot List

**TX Follows Active Slice (#441)**
- Optional mode in Radio Setup → TX: switching active slice auto-assigns TX
- Disabled during Split to preserve intentional RX/TX separation

**Upgrade check on launch (#486)**
- Checks GitHub releases for newer version, shows What's New with green "Upgrade" button
- Nags on every launch until user upgrades; "Got it — 73!" dismisses temporarily

**Lightbulb hint in What's New (#485)**
- "Found a bug or have an idea? Click the lightbulb button" in dialog header

**Version check before issue submission (#486)**
- Warns if running an old version when clicking the lightbulb button

### Bug Fixes

**Squelch controls disabled in CW mode (#285)**
- Radio locks squelch at fixed level in CW; controls now dimmed in both VFO and RX applet

**RADE audio mute not cleared on quick-mode switch (#590)**
- Quick-mode buttons now properly deactivate RADE before mode change

**Memory dialog now modeless (#591)**
- No longer blocks panadapter interaction while open

**TX Band Settings grid lines and dedup (#533)**
- Grid lines visible on all cells; removed duplicate dialog from Radio Setup

## [v0.7.18.1] — 2026-04-03

### SmartLink Persistence, Scroll Fixes & Quick Wins

### New Features

**SmartLink credential persistence via OS keychain (#572)**
- Auth0 refresh token stored securely in OS keychain (Linux/macOS/Windows)
- Auto-login from stored token on launch — no re-entering credentials
- Email pre-filled (Base64-encoded in settings) on next launch
- Requires `qtkeychain-qt6` package (optional — graceful fallback)

**Cursor frequency label on panadapter (#456)**
- Hover frequency displayed near cursor on both FFT and waterfall
- Toggle via Display panel → "Cursor Freq" button, persisted

**Dynamic RF gain range from radio (#580)**
- Query `display pan rfgain_info` on connect for actual gain range
- Slider adapts to radio model (FLEX-6300 vs 8600 vs 6600M)

**Right-click VFO "Add Spot" (#428)**
- Right-click the VFO frequency display → context menu → Add Spot
- Pre-filled with exact tuned frequency

**macOS TX audio entitlement (PR #592 by @boydsoftprez)**
- Add `com.apple.security.device.audio-input` entitlement for hardened runtime
- Fixes silent mic when launched from Finder (Fixes #543)

### Bug Fixes

**RF gain slider not updating on antenna switch (#557)**
- `PanadapterModel::rfGainChanged` was not wired to overlay slider
- Gain now updates immediately when switching RX antennas

**SpotHub Enter key activating random buttons (#459)**
- Disabled `autoDefault` on all QPushButtons in the SpotHub dialog

**GuardedSlider across all applets (#570)**
- All sliders now consume wheel events at boundaries
- Prevents VFO scroll leak from overlay and title bar sliders

**RADE audio mute not cleared on quick-mode switch (#590)**
- Quick-mode buttons bypassed RADE deactivation, leaving slice muted
- Now properly emits `radeActivated(false)` before mode change

**Memory dialog now modeless (#591)**
- No longer blocks panadapter interaction while open
- Singleton pattern with QPointer (same as SpotHub, Radio Setup)

**Auto-select first discovered radio**
- New users no longer confused by unselected radio in connection panel

**Enter key triggers SmartLink login**
- Press Enter in password field to log in

**What's New dialog updated**
- Regenerated from CHANGELOG.md — was blank since v0.7.16

## [v0.7.18] — 2026-04-02

### Tuner Controls, Band Panel & UI Polish

### New Features

**TGXL 3x1 antenna switch (PR #573 by @boydsoftprez)**
- ANT 1/2/3 buttons in Tuner Applet for TGXL 3x1 models
- Protocol: `activate ant=N` via direct TGXL TCP port 9010
- Only visible on 3x1 models (`one_by_three=1`), hidden on SO2R

**Clickable AMP/TUN status bar indicators (PR #579 by @boydsoftprez)**
- TUN: click cycles OPERATE → BYPASS → STANDBY (green/amber/grey)
- AMP: click toggles OPERATE ↔ STANDBY (green/grey)
- Resolves remote PGXL recovery (#479)

**XVTR bands in Band panel (#571)**
- Configured transverter bands appear as cyan buttons in the Band grid
- XVTR button opens Radio Setup → XVTR tab
- Band panel dynamically rebuilds on XVTR config changes

### Bug Fixes

**Connection panel now a proper dialog (#560, #574)**
- Converted from frameless popup to QDialog with title bar and close button
- No longer disappears on focus loss or lacks window decorations

**Spot click precision (#530)**
- Clicking a spot label now tunes to the exact spot frequency
- Previously the mouseReleaseEvent overrode with a step-snapped position

**Aurora/AU-520 power meter scaling (#484, #501, #582)**
- AU-520 reports `max_internal_pa_power=500` in slice status
- Power gauge now uses the higher of max_power_level and
  max_internal_pa_power for correct 0-600W Aurora scale

**CW decode hint (#392)**
- Added "(requires PC Audio)" hint to CW decode panel header

---

## [v0.7.17.6] — 2026-04-02

### Reconnect & Disconnect Fixes

**Fix disconnect crash on macOS (#561)**
- `PanadapterStream::stop()` was called from the main thread, crashing
  on macOS by closing the socket from the wrong thread
- Route `stop()` through BlockingQueuedConnection to network thread

**Fix VITA-49 not resuming after reconnect (#561)**
- On reconnect, `start()` was skipped because the socket was still bound
  from the previous session. Always call `start()` on connect — it closes
  and rebinds the socket, re-registering the UDP port with the radio

**Clear spectrum and waterfall on disconnect**
- FFT and waterfall display now blanks immediately on disconnect
- Prevents the display from appearing frozen

---

## [v0.7.17.4] — 2026-04-02

### UI Polish & Stability

### Bug Fixes

**Disconnect button unresponsive (#561)**
- Clicking a radio in the list while connected disabled the Disconnect button
- One-character fix: `!m_connected &&` → `m_connected ||`

**PanadapterStream thread affinity (#561)**
- Socket/timer signal connections moved to init() on the network thread
- Fixes "Cannot create children for a parent in a different thread" on macOS
- Prevents duplicate socket notifiers on reconnect

**Scroll wheel 8x on Linux Mint (#556)**
- Added 50ms debounce for desktops that send multiple events per notch
- Combined with existing ±1 clamp for inflated single events

**VFO slider wheel leak (#547 BUG-002)**
- Sliders in VFO panel no longer leak wheel events to frequency scroll
- Scroll over frequency display tunes by step size
- Scroll elsewhere on VFO is consumed (dead zone)

**B/S zoom toggle (#547 ENH-001)**
- B and S buttons now toggle: first press zooms, second press restores

---

## [v0.7.17.3] — 2026-04-02

### Filter Widget, DSP Cleanup & Diagnostics

### New Features

**Visual filter passband widget**
- SmartSDR-style filter control in the RX applet with static trapezoid shape
- Horizontal drag shifts passband, vertical drag adjusts bandwidth
- Numeric labels: lo, hi, bandwidth, and center offset from carrier
- 50 Hz snap grid, minimum 50 Hz bandwidth
- Replaces the DSP toggle buttons in the RX applet

**Frequency reference status bar (#478)**
- Status bar shows actual frequency reference: GPS (with satellite count
  and lock status), Ext 10M, or TCXO
- No more perpetual "Warming" on radios without a GPS antenna
- Uses oscillator status messages from radio for accurate source detection

### Bug Fixes

**Stale process on exit (#527)**
- Suppress reconnect dialog during app shutdown — was keeping the
  process alive on Windows after the main window closed
- Added 3-second timeouts to all thread wait() calls in destructors

**DAX TX level meter (#517)**
- P/CW mic gauge now shows DAX TX input level when DAX is active
- Previously showed nothing because the mic input path returns early
  in DAX mode

**DSP toggles removed from RX applet**
- 9 buttons and 27 sync points eliminated between RxApplet, VfoWidget,
  and spectrum overlay
- DSP controls remain in VFO DSP tab and spectrum overlay menu
- Eliminates triple-state tracking bugs (NR/NR2, RNN/RN2)

---

## [v0.7.17.2] — 2026-04-01

### Bug Fixes & MIDI Improvements

AetherSDR v0.7.17.2 closes 6 user-reported issues, adds MIDI relative knob
support with acceleration, and improves digital mode workflow.

### Bug Fixes

**rigctld cross-band tune (#536)**
- `set_freq` now uses `tuneAndRecenter` so the panadapter follows when
  WSJT-X or other CAT apps change bands (was using `autopan=0`)

**Mouse 8x step on KDE/Cinnamon/Linux Mint (#504)**
- Clamp scroll wheel to ±1 step per event in all scroll handlers:
  SpectrumWidget (VFO tune), RxApplet (freq label), HGauge (TGXL relays)

**Slider labels stale on connect (#544)**
- VFO slider labels (AF Gain, SQL, AGC-T, Pan, ESC, APF) now update
  immediately when the radio pushes status on connect

**S-Meter frozen during TUNE (#491)**
- TUNE bypassed `m_txRequested` so the interlock TRANSMITTING state was
  rejected. S-Meter now switches to forward power during TUNE and MOX.
  Also fixes #499 (power meter no output)

**DAX auto-activate in digital modes (#534)**
- Radio-side DAX flag (`transmit set dax=1`) auto-toggles on mode change
  to/from DIGU/DIGL/RTTY — P/CW DAX button follows automatically
- Client-side DSP (NR2/RN2/BNR) auto-disabled in digital modes to
  prevent data signal corruption
- PipeWire bridge decoupled from DAX flag — use Settings → Auto Start DAX
  for persistent virtual audio devices

**Interlock timing fields not populated (#498)**
- RCA TX1, TX Delay, ACC TX, Timeout now show correct radio values in
  Radio Setup → TX tab (parser existed but was never wired up)

### New Features

**MIDI relative knob mode**
- Relative CC encoding support (1-63 CW, 65-127 CCW)
- 20ms coalesce timer batches rapid steps into single radio commands
- 3-tier acceleration: slow spin = ½ rate (fine tune), fast = 4×
- Snap to step grid (e.g. step=500 → x.x.500, x.x+1.000)
- Radio-authoritative step size — no default until radio sends step=
- "Relative" checkbox in MIDI Mapping dialog, persisted in midi.settings

---

## [v0.7.17.1] — 2026-04-01

### Accurate Network Diagnostics

AetherSDR v0.7.17.1 replaces application-layer RTT measurement with kernel
TCP stack timing, adds per-stream network diagnostics, and fixes DAX
stream lifecycle management.

### Improvements

**Kernel TCP_INFO RTT (#455)**
- RTT now read from the kernel's TCP congestion control (TCP_INFO),
  completely immune to Qt event loop delays
- Linux: `getsockopt(TCP_INFO)` → `tcpi_rtt`
- macOS: `getsockopt(TCP_CONNECTION_INFO)` → `tcpi_srtt`
- Windows: `WSAIoctl(SIO_TCP_INFO)` → `RttUs`
- Falls back to QElapsedTimer stopwatch if kernel call unavailable
- Fixes inflated RTT (40–200ms) that correlated with waterfall load

**Per-stream Network Diagnostics (#455)**
- Network Diagnostics dialog now shows per-stream-type rates:
  Audio, FFT, Waterfall, Meters, DAX
- Per-stream packet loss tracking with sequence error counts
- TX byte counter wired to actual UDP sends
- Total RX from raw socket byte counter
- Matches SmartSDR's reported RX rate (77 kbps on Opus)

**Packet loss accuracy (#455)**
- Sequence tracking moved after ownership filter — no longer counts
  other clients' streams in Multi-Flex mode
- Meter broadcast packets excluded (unreliable sequence counter)
- Eliminates false 28% packet loss reports

**DAX stream cleanup**
- Disabling DAX now sends `stream remove` to the radio and unregisters
  streams from PanadapterStream
- Previously DAX streams persisted until app restart

**Socket write flush**
- `flush()` after `write()` in RadioConnection ensures ping and
  command packets are sent immediately, not buffered by Qt

---

## [v0.7.17] — 2026-04-01

### Network & Input Responsiveness

AetherSDR v0.7.17 moves all network and external controller I/O off the main
thread, fixing ping RTT inflation and establishing a clean 5-thread architecture.

### Improvements

**RadioConnection worker thread (#502)**
- TCP command/status I/O runs on a dedicated worker thread
- Ping RTT now measures actual network latency, not main thread load
- Fixes "Poor" network status despite zero packet drops
- Response callbacks dispatched on main thread via auto-queued signals
- All external `connection()->sendCommand()` calls routed through RadioModel

**External controllers worker thread (#502)**
- FlexControl USB tuning knob, MIDI controllers, and serial port PTT/CW
  keying now run on a shared ExtControllers worker thread
- USB serial I/O, RtMidi callbacks, and poll timers no longer compete
  with GUI rendering for main thread CPU
- MIDI parameter dispatch via signal to main thread (thread-safe)

### Architecture

Five-thread design: Main (GUI + models), Connection (TCP), Audio (DSP),
Network (VITA-49 UDP), ExtControllers (FlexControl/MIDI/SerialPort).
Each worker thread has a single responsibility and communicates via
auto-queued Qt signals. The main thread handles only rendering and
model dispatch.

---

## [v0.7.16] — 2026-03-31

### World-First TGXL Relay Control & Global Band Plans

AetherSDR v0.7.16 brings manual control of the 4O3A Tuner Genius XL Pi
network — a world first for any SDR client — and makes the spectrum overlay
useful for operators worldwide with selectable IARU band plans.

### New Features

**Manual TGXL Pi Network Relay Control (#469)**
- Scroll over the C1, L, or C2 relay bars in the Tuner applet to adjust
  relay positions one step at a time
- AetherSDR auto-connects directly to the TGXL on TCP port 9010
- Real-time relay updates with ~3ms round-trip
- Cursor changes to ↕ when hovering over scrollable relay bars
- Protocol reverse-engineered from the 4O3A TGXL management app
- First SDR client to support manual TGXL relay control

**Selectable IARU Band Plans (#425)**
- View → Band Plan now includes a region selector:
  ARRL (US), IARU Region 1, Region 2, Region 3
- Region 1: Europe, Africa, Middle East (80m stops at 3.800, 40m at 7.200)
- Region 2: Americas (80m to 4.000, 40m to 7.300)
- Region 3: Asia-Pacific, Oceania (80m to 3.900, 40m to 7.200)
- Each plan includes segment allocations and spot frequency markers
- Plan selection persists across restarts
- Band plan data loaded from bundled JSON resources

**multiFLEX Dashboard (#56)**
- Settings → multiFLEX opens a live station dashboard
- Per-client: station name, program, TX antenna, TX frequency
- LOCAL PTT status, enable/disable toggle

### Bug Fixes & Improvements

**Default MTU reduced to 1450 (#470)**
- VITA-49 FFT/waterfall packets are 1436 bytes at MTU 1500, exceeding most
  VPN/SD-WAN tunnel MTUs (WireGuard 1420, OpenVPN 1400)
- Confirmed fix by user on Cisco Meraki SD-WAN
- Adjustable in Radio Setup → Network → Advanced

**FFTW thread safety for NR2 (#467)**
- Added mutex to serialize all FFTW plan creation/destruction
- Prevents potential crashes when switching DSP modes or regenerating wisdom
- `fftw_execute()` left unlocked (thread-safe per FFTW spec)

**Removed broken release.yml workflow**
- GitHub Actions release workflow removed (GITHUB_TOKEN PRs can't trigger CI)
- Ship/release now handled locally via Claude using gh CLI and GPG-signed tags

## [v0.7.15] — 2026-03-30

### Digital-Friendly Minimal Mode

AetherSDR v0.7.15 introduces **Minimal Mode** — a streamlined, ultra-compact
interface designed for digital mode operators who want AetherSDR running
alongside WSJT-X, fldigi, or other companion software without dominating
screen real estate.

### New Features

**Minimal Mode (#208)**
- Ctrl+M or ↙ button in the title bar collapses AetherSDR to a 260px-wide
  applet-only strip — just your VFO, RX controls, TX controls, and meters
- Spectrum, waterfall, and status bar are hidden; title bar goes compact
- ↗ button restores full mode instantly with splitter sizes preserved
- All applets remain available and fully functional
- Perfect for digital mode operation: park AetherSDR on one side of the screen
  while your decoder fills the rest

**multiFLEX Dashboard (#56)**
- Settings → multiFLEX opens a live dashboard showing all connected stations
- Per-client display: station name, program, TX antenna, TX frequency
- LOCAL PTT status with checkmarks and "Enable Local PTT" button
- multiFLEX enable/disable toggle
- Real-time updates via client status subscriptions

### Infrastructure

**CI/CD Pipeline**
- Docker-based CI builds in ~3.5 minutes (down from 3–19 min variable)
- CodeQL analysis runs in parallel without blocking merge
- `git ship` squashes local commits into single auto-merge PR
- `git release` automates version bump, changelog PR, tag, and release

## [v0.7.12] — 2026-03-29

### New Features

**GPG Release Signing (#397, #398)**
- Linux AppImage and source archives are GPG-signed with detached `.asc` signatures
- SHA256SUMS.txt generated and signed for each release
- macOS artifacts signed via Apple codesign + notarization (unchanged)
- Public key published at `docs/RELEASE-SIGNING-KEY.pub.asc` and keys.openpgp.org
- Verification guide at `docs/VERIFYING-RELEASES.md`

**Commit Signing**
- All commits to `main` require GPG signatures (branch protection enforced)
- Contributor setup guide in CONTRIBUTING.md

**Configurable Band Plan Size (#406)**
- View → Band Plan submenu: Off, Small (6pt), Medium (10pt), Large (12pt), Huge (16pt)
- Strip height scales with font size
- Replaces the previous on/off checkbox

**Visual Keyboard Shortcut Manager (#239)**
- View → Configure Shortcuts opens a visual keyboard map dialog
- Full ANSI keyboard layout with keys color-coded by action category
- Click any key to assign/change/clear its binding
- ~45 bindable actions across 12 categories (Frequency, Band, Mode, TX, Audio, Slice, Filter, Tuning, DSP, AGC, Display, RIT/XIT)
- Conflict detection with reassign prompt
- Filterable action table with search and category filter
- Reset to defaults (per-key or all)
- Bindings persist across restarts via AppSettings
- Replaces hardcoded keyboard shortcuts with fully customizable bindings

**Click-and-Drag VFO Tuning (#404)**
- Click inside the filter passband and drag left/right to tune the VFO
- Frequency snaps to step size during drag
- Filter edge drag (resize) takes priority within ±5px grab zone

**Go to Frequency (G key)**
- Press G to open the VFO direct frequency entry field
- Pre-fills with current frequency, selected for easy overtype

**Space PTT Hold-to-Transmit**
- Hold Space to transmit, release to return to RX (true momentary PTT)
- Works regardless of which UI widget has focus
- Properly syncs TX state with TX applet and status bar

### Bug Fixes

**NR2/RN2/BNR Crash on DSP Mode Switch**
- SEGV in SpectralNR::process() when switching from BNR to NR2
- Root cause: enabled flag was set before the DSP object was constructed; audio arriving during the transition called process() on a null object
- Fix: set enabled flag AFTER construction succeeds; clear flag BEFORE destruction

**FlexControl UI Lag (#379)**
- Each encoder step sent a separate TCP command, flooding the radio
- Fix: coalesce rapid encoder steps into a single command every 20ms

**FlexControl Menu Stub (#380)**
- Settings → FlexControl showed "not implemented"
- Fix: wired to open Radio Setup dialog on the Serial tab

**TNF Crash on +TNF Click (#381)**
- SIGBUS on macOS when clicking +TNF after a panadapter layout change
- Root cause: rebuildTnfMarkers lambda captured raw SpectrumWidget pointer that became dangling when the pan was removed
- Fix: capture QPointer instead; also fixed duplicate `tnf create` commands per click

**FlexControl ToggleMox/ToggleTune Stuck in TX (#382)**
- Pressing the FlexControl button a second time didn't toggle TX off
- Root cause: `applyTransmitStatus()` never parsed `mox=` from radio status, so `isMox()` always returned false
- Fix: parse `mox=` key in transmit status updates

**VFO Lock Icon Not Updating (#384)**
- Lock icon on VFO overlay didn't update when toggled via FlexControl or RxApplet
- Root cause: VfoWidget::setSlice() connected 30+ SliceModel signals but was missing `lockedChanged`
- Fix: added the missing signal connection

**Mouse Wheel 8x Step on KDE/Cinnamon (#390, #405)**
- Tuning steps were 8x the selected step size on Linux Mint, Cinnamon, and KDE Plasma
- Root cause: these desktops send high-resolution angleDelta (960 per notch instead of 120)
- Fix: accumulate angleDelta and normalize to 120 units per step

**ESC Gain Slider Black Thumb (#394)**
- ESC gain slider thumb was invisible (black) on macOS and Windows
- Root cause: kSliderStyle only defined horizontal handle rules; ESC gain slider is vertical
- Fix: added vertical groove and handle QSS rules

**RxApplet NR2 Button Not Working (#329)**
- Cycling the RX panel NR button to NR2 didn't enable noise reduction
- Root cause: the `nr2CycleToggled` handler only synced the VFO button visual but never called `enableNr2WithWisdom()`
- Fix: NR2 now actually enables when cycled from the RX panel, matching VFO and overlay buttons

**Split Slice on Wrong Pan (#328)**
- In multi-pan mode, clicking SPLIT could create the TX slice on the wrong panadapter
- Root cause: split used `m_activePanId` (global) instead of the RX slice's actual pan
- Fix: use `rxSlice->panId()` for the `slice create` command
- Bonus: CW split now offsets 1 kHz up (standard convention), other modes 5 kHz

## [v0.7.11] — 2026-03-29


### New Features

**Panadapter Click-to-Spot (#36)**
- Right-click on the panadapter to create a spot marker with callsign, comment, and configurable lifetime
- Optionally forward spots to your connected DX cluster
- Right-click on existing spots: Tune, Copy Callsign, QRZ Lookup, Remove
- Spot frequency snaps to tuning step size

**Per-Slice Record & Play (#164)**
- Record (⏺) and Play (▶) buttons on each VFO flag, matching SmartSDR placement
- Record button pulses red while recording
- Play disabled until a recording exists (radio-managed)
- TX playback: press MOX then Play for a built-in voice keyer

**DAX IQ Streaming (#124)**
- Raw I/Q data from the radio's DDC to external SDR apps (SDR#, GQRX, GNU Radio)
- 4 IQ channels at 24/48/96/192 kHz via PulseAudio virtual capture devices
- DIGI applet: per-channel rate dropdown, level meters, On/Off toggle
- Spectrum overlay: IQ channel routing selector per panadapter
- Dedicated worker thread for byte-swap and pipe I/O at high sample rates

**Applet Panel Collapse (#178)**
- ☰ hamburger icon in the status bar toggles the right applet panel
- Spectrum/waterfall expands to full width when panel is hidden
- Also available via View → Applet Panel checkbox
- Custom painted +PAN spectrum icon replaces text label

**Drag-Reorderable Applets (#335)**
- Drag applets by their ⋮⋮ grip title bars to reorder in the panel
- Order persists across sessions
- View → Reset Applet Order to restore defaults
- Built on QDrag framework (future-proofs for pop-out to floating windows)

**Opus Codec Independent of RADE (#375)**
- SmartLink compressed audio now works without the RADE digital voice module
- System libopus detected via pkg-config when RADE is disabled
- Windows: setup-opus.ps1 builds static opus from source

**Modeless Dialogs**
- SpotHub, Radio Setup, and MIDI Mapping dialogs no longer block the main window
- Interact with the radio while dialogs are open

### Bug Fixes

**BNR Crash Fix (#376)**
- r8brain resampler buffer overflow — BNR output can return up to 9,600 samples at once but the resampler was allocated for 4,096. Increased to 16,384. Found and verified via ASAN.

**AppSettings Corruption Prevention**
- Atomic save: write to .tmp file, validate XML, rename over original
- Backup recovery: auto-recover from .bak if main file is corrupt
- Count guard: refuse to save if settings count dropped below half of loaded count
- Key validation: skip keys with invalid XML element name characters

**RadioModel Shutdown Use-After-Free**
- Disconnect all signals from RadioConnection before member destruction
- Prevents accessing destroyed XVTR map and slice models during teardown
- Found via AddressSanitizer (ASAN) build

**Opus SSE Alignment**
- Copy input data to alignas(16) buffers before opus_encode/opus_decode
- Prevents SEGV on SSE-optimized RADE opus builds

**Spot Label Deconfliction**
- Re-scan all placed labels after each nudge to properly stack across all levels
- Spots no longer overlap when multiple labels are close in frequency

**Other Fixes**
- Duplicate spot-to-cluster sends fixed (disconnect before reconnect in wirePanadapter)
- DIGI applet section headers use distinct label style (not confused with draggable title bars)
- DIGI applet layout: CAT Control and DAX Enable rows moved below their channel status for cleaner visual flow

### Platform Notes

- DAX IQ pipe output is Linux/macOS only. Windows DAX IQ support tracked in #87.
- Opus standalone requires system libopus on Linux (`libopus-dev`) or macOS (`brew install opus`). Windows builds from source via setup-opus.ps1.

---

73, Jeremy KK7GWY & Claude (AI dev partner)

---

## [v0.7.10] — 2026-03-29


### TX Audio
- **PR #353 merged** — DAX/SSB TX edge sync fix from @pepefrog1234. Immediate SSB TX audio gating (no delay on key-down), DAX backlog stabilization, Low-Latency DAX route option for FreeDV (Settings menu toggle)
- **MIDI CW keying** — straight key (`cw.key`), iambic dit/dah paddles (`cw.dit`/`cw.dah`), and PTT hold (`cw.ptt`) via new Gate param type. CW speed also mappable. (#295)

### Spots
- **Hover tooltips** — hover over any spot label to see callsign, frequency, source, spotter, comment, and spotted time
- **Spot trigger on click** — clicking a spot sends `spot trigger <index>` to the radio so external logging software sees the click (#341)
- **Lines stop at label** — spot dotted lines draw from bottom of spectrum to the label only, no clutter above (#337)
- **Cross-band tuning** — clicking a spot on a different band in SpotHub now tunes AND recenters the panadapter (#352)

### Panadapter
- **3v / 4v layouts** — 3 and 4 vertical pan stacking options in the layout chooser (#312)
- **Single-click-to-tune** — configurable in View menu (off by default). When enabled, single left-click on spectrum/waterfall tunes immediately (#342)

### Bug Fixes
- **Line out volume** — master volume slider controls radio line out when PC Audio is disabled (#244, #351)
- **Audio preserved on profile change** — profile load no longer switches audio to PC when listening through radio speakers (#336)
- **CW decode after profile change** — deferred re-check restarts decoder when mode status arrives after profile load (#305)
- **XVTR band stack** — added 2m, 1.25m, 440, 33cm, 23cm to band frequency lookup. Band stack save/restore now works for all XVTR bands (#346)
- **Full model reset on disconnect** — all radio-specific state (APD, tuner, amplifier, XVTR, power levels) resets when switching between different radio models (#359)
- **Bundled RtMidi** — MIDI controller support works on all platforms (Linux/macOS/Windows) without external package dependency

### Issues Closed
#25, #40, #54, #74, #105, #115, #157, #158, #190, #217, #219, #224, #226, #231, #244, #252, #262, #263, #270, #273, #295, #296, #300, #303, #305, #306, #308, #310, #312, #314, #319, #337, #338, #339, #340, #341, #342, #349, #351, #352, #355, #357, #359

Full changelog: https://github.com/ten9876/AetherSDR/compare/v0.7.9...v0.7.10

---

## [v0.7.9] — 2026-03-29


### Highlights

- **MIDI controller mapping** — Map any class-compliant USB MIDI controller to 50+ AetherSDR parameters. MIDI Learn mode: select a parameter, move a knob, binding created. Dedicated Settings → MIDI Mapping dialog with device selector, binding table, activity indicator, and named profiles. Supports CC (knobs/faders), Note On/Off (buttons/pads), and Pitch Bend (high-res tuning). CW straight key, iambic dit/dah paddle, and PTT via MIDI Gate parameters. Bindings stored in dedicated `~/.config/AetherSDR/midi.settings` XML file. Optional dependency (RtMidi). (#355, #295)

- **FlexControl USB tuning knob** — Auto-detect VID 0x2192 / PID 0x0010, rotary tuning with acceleration (1–6x), 3 configurable buttons (tap/double/hold). Radio Setup → Serial tab config. (#25)

- **USB Cable management** — Radio Setup → USB Cables tab for configuring USB-serial adapters on the radio's rear USB ports. CAT, BCD (band decoder), Bit (8-bit per-band switching), and Passthrough cable types. (#40)

- **FreeDV Reporter spots** — Real-time FreeDV station spots via Socket.IO WebSocket to qso.freedv.org. New FreeDV tab in SpotHub. (#349)

- **BNR streaming mode** — Documented container streaming vs transactional mode. The Maxine BNR container must be started with the streaming entrypoint for real-time audio processing. Wiki updated with correct docker run command.

### Other Changes

- Closed 8 TX audio / VOX issues (#54, #74, #115, #157, #158, #270, #306, #310) — all addressed by v0.7.6–v0.7.8 TX audio fixes
- RTTY Operation wiki page added
- Issue templates updated to encourage AI-assisted reports
- Radio Setup dialog widened to show all tabs without scroll arrows

### Dependencies

New optional dependencies (feature hidden when not available):
- `rtmidi` — MIDI controller support (`sudo pacman -S rtmidi`)
- `qt6-websockets` — FreeDV Reporter spots (`sudo pacman -S qt6-websockets`)

### Build

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j$(nproc)
```

Full changelog: https://github.com/ten9876/AetherSDR/compare/v0.7.6.1...v0.7.9

---

## [v0.7.8] — 2026-03-28

## What's New

### PC TX Audio
- PC microphone TX audio now works via Opus encoding on the `remote_audio_tx` stream
- Client-side mic gain control (P/CW slider, 0-100%, persisted in AppSettings)
- Client-side mic level metering with VU-style ballistics (fast attack, slow decay)
- VOX support with PC mic input
- DAX TX path (WSJT-X, VARA, etc.) remains uncompressed float32 on `dax_tx`

### P/CW Applet
- Mic level meter works correctly for both PC and hardware mic inputs
- Radio CODEC meters suppressed when mic_selection=PC (prevents noise floor flicker)
- Compression meter disabled pending gain reduction calculation fix (#345)

### Windows
- Application icon now displays in taskbar, Start Menu, and Alt-Tab

### Documentation
- Full TX audio signal path documented with all 15 radio meters (`docs/tx-audio-signal-path.md`)

## Bug Fixes
- Fixed `std::log10f` build failure on CI (not in C++ standard)

## Upgrade Notes
Drop-in replacement for v0.7.7. PC mic gain (`PcMicGain`) saved client-side in AppSettings (default 100%).

---

## [v0.7.7] — 2026-03-28


**AetherSDR v0.7.7 introduces SpotHub** — a completely new, unified spot management system that brings DX spotting to Linux ham radio like never before. Four spot sources. One panadapter. Zero external tools required.

---

## Introducing SpotHub

Gone are the days of running separate Python scripts, telnet clients, and browser tabs just to see who's on the air. SpotHub brings it all together in a single dialog (**Settings → SpotHub**) with six tabs:

### DX Cluster
Connect to any DX Spider, AR-Cluster, or CC Cluster node via telnet. Full interactive console with command input. See every spot the moment it hits the cluster — right on your panadapter.

### Reverse Beacon Network (RBN)
Automated CW/RTTY/FT8 skimmer spots from the worldwide RBN network. Built-in rate limiting prevents command flooding during contests. Connect to `telnet.reversebeacon.net` and let the skimmers do the work.

### WSJT-X Decode Spotter
**This is the big one.** AetherSDR listens for WSJT-X decode messages via UDP multicast and spots every station you can hear — directly on the panadapter. Features:
- **Smart filters**: Show only CQ calls, CQ POTA, or stations calling *you*
- **Color-coded by category**: Green for CQ, Cyan for CQ POTA, Red for stations calling you
- **SNR-based transparency**: Strong signals pop with full opacity, weak ones fade — you see propagation at a glance
- **Configurable lifetime**: 30-300 second spot decay, tuned for the fast pace of digital modes

### POTA (Parks on the Air)
Real-time POTA activation feed polling `api.pota.app`. See every active park activation on your panadapter with park reference, park name, and mode. Spot lifetime synced to the POTA API's own expiry timer. Never miss an activator again.

### Spot List
All spots from all sources in one sortable, filterable table. Band filter checkboxes (160m-6m). Source column tells you where each spot came from. Double-click any row to tune. Smart auto-scroll that pauses when you're reading and resumes when you scroll back.

### Display Controls
Full control over how spots render on the panadapter: label levels, position, font size, colors, background opacity, and spot lifetime.

---

## Per-Source Color Coding

Every spot source has its own configurable color:

| Source | Default Color | What You See |
|--------|--------------|--------------|
| DX Cluster | Tan | Classic cluster spots |
| RBN | Blue | Skimmer detections |
| WSJT-X | Green/Cyan/Red/White | Per-filter category colors |
| POTA | Yellow | Park activations |

Colors are sent to the radio via the FlexRadio spot API — they render natively on the panadapter with no client-side override needed.

## Spot Density Badges

When spots pile up (and they will), overlapping labels collapse into amber **+N** badges. Click a badge to expand a popup showing every collapsed callsign with its frequency. Click any entry to tune. No more unreadable label soup.

## Deduplication

Cross-source dedup ensures the same callsign on the same frequency doesn't flood your panadapter. If a station QSYs, the old spot is replaced. Each source uses its own appropriate lifetime for dedup timing.

## Worker Thread Architecture

All four spot sources run on a dedicated worker thread. Network I/O, protocol parsing, and log file writing happen completely off the main GUI thread. Spots are batched and forwarded to the radio once per second — smooth, efficient, and invisible.

---

## Other Fixes in This Release

- **SmartLink reconnect fix** (#224) — Signal connection leak that prevented switching between SmartLink radios
- **Version in title bar** (#315) — Window title now shows "AetherSDR vX.X.X"
- **Spot label hover cursor** — Pointing hand cursor on spot labels and density badges

---

## Upgrade Notes

Drop-in replacement for v0.7.6.x. No new required dependencies. SpotHub settings are saved to `~/.config/AetherSDR/AetherSDR.settings` on first use.

---

Built with [Claude Code](https://claude.ai/claude-code). 73 de KK7GWY.

---

## [v0.7.6.1] — 2026-03-28

## Bug Fix

- **Fix SmartLink reconnect signal leak** — `WanConnection` signal connections in `RadioModel::connectViaWan()` were never disconnected on teardown, causing duplicate signal delivery on each reconnect cycle. This prevented users from switching between SmartLink radios or cleanly disconnecting and reconnecting. (#224)

## Upgrade

Drop-in replacement for v0.7.6. No new dependencies or settings changes.

---

## [v0.7.6] — 2026-03-27

## Highlights

### NVIDIA NIM BNR — GPU-Accelerated AI Noise Removal (#288)

**A world first for SDR clients.** AetherSDR v0.7.6 introduces real-time GPU-accelerated neural noise removal powered by NVIDIA Maxine BNR, running on your local RTX GPU via a self-hosted Docker container.

- **Neural denoising** trained on massive audio datasets — superior to classical spectral (NR2) and RNNoise (RN2) approaches
- **Real-time gRPC streaming** — 48kHz mono float32, 10ms processing chunks, ~15ms total added latency
- **Intensity control** — adjustable denoising strength (0–100%) via slider in DSP panel
- **Jitter buffer** — 50ms priming for smooth, uninterrupted playback
- **Container management** — Start/Stop/Status in Radio Setup → Audio, optional autostart on app launch
- **Zero-config audio** — BNR button in VFO DSP tab and spectrum overlay, 3-way mutual exclusion with NR2/RN2

**Requirements:** NVIDIA RTX 4000+ GPU (Ada Lovelace or newer), Docker + NVIDIA Container Toolkit, NGC API key (free).

```bash
docker login nvcr.io  # Username: $oauthtoken, Password: <NGC API key>
docker run -d --gpus all --shm-size=8gb \
  -p 8001:8001 -p 8000:8000 \
  -e NGC_API_KEY=$NGC_API_KEY \
  -e STREAMING=true \
  --restart unless-stopped \
  --name maxine-bnr \
  nvcr.io/nim/nvidia/maxine-bnr:latest

cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_BNR=ON
cmake --build build -j$(nproc)
```

### ESC Diversity Beamforming (#20, #38)

- **DIV toggle** in VFO audio tab enables dual-SCU diversity reception
- **ESC controls** — polar display (phase=angle, gain=radius), horizontal phase slider (0–360° in 5° steps), vertical gain slider (0.0–2.0)
- **Real-time ESC meter** — signal strength bar after ESC processing
- Protocol verified against SmartSDR pcap: `esc=on/off`, phase in radians, DiversityChild guard per FlexLib
- Requires DIV_ESC license (SmartSDR+) and dual-SCU radio

### Band Zoom Buttons

- **S** (Segment) and **B** (Band) buttons at bottom-left of waterfall
- Uses radio-native `segment_zoom=1` / `band_zoom=1` commands (SmartSDR protocol)

## Bug Fixes

- **XVTR band switch crash** — QPointer prevents SEGV when panadapter destroyed during band change
- **CW decoder not working on first pan** — initial applet wasn't wired through setActivePanApplet()
- **Band change panadapter not scrolling** — use tuneAndRecenter() instead of autopan=0
- **VFO widget not shrinking** — fixed setFixedWidth constraining height when ESC panel hidden
- **Null pointer guards** — m_panApplet checked before setCwPanelVisible() calls

## Build

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j$(nproc)

cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_BNR=ON
cmake --build build -j$(nproc)
```

73, Jeremy KK7GWY & Claude (AI dev partner)

---

## [v0.7.5] — 2026-03-27

## What's New

### ESC (Enhanced Signal Clarity) — Diversity Beamforming (#20, #38)
- **DIV toggle** in VFO audio tab enables dual-SCU diversity reception
- **ESC controls**: toggle button, horizontal phase slider (0–360° in 5° steps), vertical gain slider (0.0–2.0)
- **Polar display**: crosshair circle with dot positioned by phase (angle) and gain (radius from center)
- **Real-time ESC meter**: signal strength bar after ESC processing (SLC/ESC meter, 10 fps)
- Protocol matches SmartSDR pcap: `esc=on/off`, phase in radians, DiversityChild guard per FlexLib
- ESC panel visible only on diversity parent slice — child slices show DIV but not ESC controls
- Requires DIV_ESC feature license (SmartSDR+ or higher)
- Dual-SCU radios only: FLEX-8600(M), FLEX-6700(R), FLEX-6600(M), AU-520(M)

### Bug Fixes
- **Band change panadapter scroll**: switching bands now recenters the panadapter on the new frequency (was sending `autopan=0` which prevented scrolling)
- **VFO widget resize**: panel properly shrinks when DIV is disabled (fixed `setFixedWidth` constraining height)

## Install
Download the appropriate package for your platform from the assets below, or build from source:
```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j$(nproc)
./build/AetherSDR
```

73, Jeremy KK7GWY & Claude (AI dev partner)

---

## [v0.7.4] — 2026-03-26

## What's New in v0.7.4

### VOX Support — #253
AetherSDR now creates a `remote_audio_tx` stream on connect and continuously streams mic audio to the radio during RX. This enables the radio's VOX (Voice-Operated Transmit) detection via `met_in_rx=1`.

Previously, only `dax_tx` streams were created and mic audio was only sent during active transmit — so VOX had no audio to monitor and could never trigger.

The DAX TX path is unchanged (only sends during active transmit to prevent mic bleed into digital modes). VOX audio flows on a separate `remote_audio_tx` stream with its own accumulator.

### Profile Load Fix — #289
Applying a global profile no longer causes the FFT spectrum to disappear. The radio resets `x_pixels`/`y_pixels` to defaults during profile application — AetherSDR now detects this and automatically re-pushes correct widget dimensions.

### Band Stack Alignment Fix — #279, #291
Removed bandwidth and center from band stack save/restore. Both are radio-authoritative per FlexLib API — pushing stale saved values caused FFT/waterfall horizontal misalignment on band changes. Only dBm scale (a client display preference) is persisted.

---

**Full Changelog**: https://github.com/ten9876/AetherSDR/compare/v0.7.3.1...v0.7.4

---

## [v0.7.3.1] — 2026-03-26

## v0.7.3.1 Hotfix

Fixes two regressions introduced in v0.7.3:

### Profile Load Blank Spectrum — #289
Applying a global profile caused the FFT spectrum to disappear. The radio resets `x_pixels`/`y_pixels` to defaults (50/20) during profile application, overwriting our correct widget dimensions. Now automatically detects when the radio reports small pixel values in status and re-pushes the actual widget dimensions.

### FFT/Waterfall Alignment on Band Change — #279, #291
Switching bands caused horizontal misalignment between FFT spectrum and waterfall. The band stack was saving and restoring **bandwidth** (a radio-authoritative setting per FlexLib API), which overrode the radio's bandwidth and created a timing mismatch. Removed bandwidth and center from band stack persistence — only dBm scale (a client-side display preference) is saved/restored.

### Also included from v0.7.3 post-release
- Title bar speaker mute now controls local PC audio engine (#259)
- VFO tab bar speaker icon toggles muted/unmuted on right-click (#283)

---

**Full Changelog**: https://github.com/ten9876/AetherSDR/compare/v0.7.3...v0.7.3.1

---

## [v0.7.3] — 2026-03-26

## What's New in v0.7.3

### DVK (Digital Voice Keyer) — #19
- **12-slot recording panel** with F1-F12 hotkeys, REC/STOP/PLAY/PREV buttons
- **Elapsed timer** with 100ms resolution and per-slot progress bars (red/green/blue by operation)
- **Right-click context menu**: Rename, Clear, Delete, Import WAV, Export WAV
- **Inline name editing** via double-click, with forbidden character stripping
- **WAV export** (download): reverse TCP transfer from radio to local file
- **WAV import** (upload): validates format (2-ch, 32-bit float, 48 kHz, max 5 MB), TCP streaming to radio
- **Mode-aware availability**: DVK enabled only in voice modes (USB/LSB/AM/SAM/FM/NFM/DFM)
- **Empty slot guards**: prevents TX on empty slots, disables Clear/Delete/Export for empty slots

### CWX/DVK Panel Management
- CWX enabled only in CW/CWL modes, DVK only in voice modes
- Three indicator states: active (cyan), available (dim), unavailable (dark grey)
- Auto-close on mode switch, mutual exclusion between panels
- 4-pane splitter layout fix (CWX + DVK + PanStack + AppletPanel) — #281

### FFT/Waterfall Alignment Fix — #279
- Removed hardcoded xpixels=1024 in RadioModel::configurePan() that overwrote correct widget dimensions
- FFT spectrum and native waterfall tiles now align horizontally at all window sizes

### FFT dBm Calibration Fix
- Bin conversion now uses actual y_pixels from radio status (bins are pixel Y positions, not 0-65535)
- Tracks y_pixels from radio status updates for correct dBm scaling across different display sizes

### Mute Controls — #259, #283
- **Title bar speaker button** now mutes local PC audio engine (was only muting radio line out)
- **VFO tab bar speaker icon** toggles between muted/unmuted to show mute state at a glance
- **Right-click speaker icon** on VFO tab bar toggles mute directly (matches SmartSDR)

---

**Full Changelog**: https://github.com/ten9876/AetherSDR/compare/v0.7.2...v0.7.3

---

## [v0.7.2] — 2026-03-25

## Bug Fixes

- **Per-slice step size from radio** (#274, #241) — Step size and step list are now driven entirely by the radio's per-slice, per-mode status. The RX applet stepper dynamically rebuilds when switching slices or modes. Removed client-side step overwrite that was fighting the radio's mode-specific defaults.

- **Antenna Genius discovery retry** — If the UDP port 9007 bind fails at startup (e.g. another process holds the port), AetherSDR now retries every 5 seconds until it succeeds. Once detected, the AG persists for the app lifetime. Previously a failed bind meant the AG was never detected.

## Policy

- **Radio-authoritative settings** — Documented and enforced the policy that the radio is always authoritative for any setting it stores. AetherSDR only persists client-side settings the radio doesn't know about. This prevents the radio and client from fighting on reconnect.

---

🤖 Generated with [Claude Code](https://claude.com/claude-code)

73, Jeremy KK7GWY & Claude (AI dev partner)

---

## [v0.7.1] — 2026-03-25


Bug fixes, multi-pan stability improvements, and new CW/spot controls.

### Multi-Pan Improvements
- **Radio-authoritative pan restore** — radio restores pans from GUIClientID session, client arranges layout (no more creating/removing pans on startup)
- **Close pan via X button** — click the X on any pan's title bar to close it and auto-rearrange remaining pans
- **Defensive xpixels re-push** — 500ms delayed re-push ensures all pans get proper FFT bin resolution after connect
- **TGXL/PGXL meter separation** — meters routed by amplifier handle so TGXL and PGXL gauges don't cross-contaminate

### CW Decoder Controls
- **Lock Pitch** button — prevents decoder from wandering to noise after finding the CW tone
- **Lock Speed** button — locks WPM to current detected value
- **Pitch Range sliders** (Lo/Hi) — constrain decoder frequency search window (300-1200 Hz)
- **Numeric Hz labels** on pitch range sliders with dynamic tooltips

### Spot Settings Enhancements (#260)
- **Color picker** for spot text override color
- **Color picker** for spot background override color
- **Background Opacity** slider (0-100)
- **Live preview** — all changes apply immediately to the spectrum

### Bug Fixes
- **macOS trackpad scroll-to-tune** (#266) — handle pixelDelta for trackpad, ignore momentum scrolling
- **FWDPWR/SWR meter source** (#233) — match "TX-" not just "TX" for exciter power
- **S-Meter TX power with PGXL** — shows amplifier output power when PGXL is connected
- **Title bar mute toggles** (#259) — click speaker/headphone icons to mute/unmute line out and headphones
- **Filter polarity normalization** — DIGU/USB modes correct negative filter offsets from radio session restore
- **Band stack filter recall** — only recalls filter when saved mode matches recalled mode

### Known Issues
- CW sidetone not yet available through PC Audio (radio sends sidetone to physical outputs only, client-side generation deferred)
- `sub codec all` subscription rejected on firmware v4.1.5 (harmless)

---

🤖 Generated with [Claude Code](https://claude.com/claude-code)

73, Jeremy KK7GWY & Claude (AI dev partner)

---

## [v0.7.0] — 2026-03-25


## CWX Keyer Panel

Built-in CW keyboard keyer — toggle via the **CWX** label in the status bar.

- **Send mode**: type your message, hit Enter to send with perfect timing and spacing
- **Live mode**: each keystroke sends immediately for real-time CW keying
- **F1-F12 macro editor**: store and recall 12 CW macros via the Setup view
- **F-key hotkeys**: press F1-F12 to send macros when the CWX panel is visible (independent of global keyboard shortcuts toggle)
- **Chat bubble history**: sent messages displayed as rounded bubbles with timestamps, scrolling from bottom up
- **Speed control**: 5-100 WPM stepper synced with radio
- **Prosign support**: = (BT), + (AR), ( (KN), & (BK), $ (SK)
- **QSK toggle** and **break-in delay** in Setup view

## AMP Applet — PGXL Amplifier Display

New **AMP** button in the applet panel, auto-shows when a Power Genius XL is detected.

- Forward Power gauge (0-2000W)
- SWR gauge (1-3, converted from Return Loss)
- Temperature gauge (30-100°C)
- Matches TGXL gauge styling (HGauge widgets)
- Meter wiring confirmed from live PGXL log data

## CW Decode Sensitivity Slider

New **Sens** slider in the CW decode bar filters out low-confidence decoded characters.

- Slide right = higher sensitivity (fewer garbage characters)
- Slide left = show everything
- Reduces noise in the CW decode display on weak signals

## Other Improvements

- Improved log file handling and diagnostics
- Additional issue triage and community engagement

---

🤖 Generated with [Claude Code](https://claude.com/claude-code)

73, Jeremy KK7GWY & Claude (AI dev partner)

---

## [v0.6.2] — 2026-03-24


## Band Stacking Registers

Per-band save/recall is here. When you switch bands using the spectrum overlay, AetherSDR remembers your last settings and restores them when you come back.

**Saved per band:** frequency, mode, AGC mode + threshold, RX/TX antenna, squelch, step size, all DSP flags (NR, NB, ANF, NRL, NRS, RNN, NRF, ANFL, ANFT, APF, NR2, RN2), pan bandwidth (zoom), and dBm scale.

First visit to a band uses the static default. Subsequent visits recall your last settings. Settings persist across restarts.

## Multi-Pan Improvements

- **Per-pan band change** — each pan's band buttons now target the correct slice, not the global active slice
- **Pan recenters on band change** — FFT/waterfall follows the VFO when switching bands across pans
- **Correct slice labels** — each pan shows the right slice letter (A, B, C, D)
- **Cross-band save guard** — prevents frequency contamination between bands in multi-pan mode
- **Widget cleanup on pan removal** — prevents crashes when reducing pan layouts

## Per-Slice Step Size

The radio sends per-slice step sizes and step lists (different for CW vs SSB vs digital). AetherSDR now parses these and syncs the RX applet stepper when switching between slices. The correct step list is shown per mode.

## New Features

- **Frequency auto-calibration** — Radio Setup → RX tab: Cal Frequency field + Start button + Offset display
- **Keyboard shortcuts** — View → Keyboard Shortcuts (off by default): arrow keys, step cycle, PTT, mute, tune lock
- **DX spot display** — spots from external tools (FlexSpots, N1MM, etc.) appear as clickable labels on the panadapter
- **Spot click-to-tune** — click a spot label to tune to that frequency
- **Spot settings** — Settings → Spots: enable/disable, font size, max stacking levels, color overrides

## Bug Fixes

- Fix GCC 13 internal compiler error on Ubuntu 24.04 (#254)
- Fix filter polarity on session restore (wrong-side passband for DIGU/USB)
- Fix FWDPWR/SWR meter on all radios — source matching was too strict (#233)
- Fix TGXL handle: accept initial 0x00000000 and upgrade to real handle
- Fix SmartLink: use UPnP ports when manual port forwards are absent (#230)
- Fix preamp sync: shared antenna hardware state propagated to all pans (#232)
- Fix crash in mhzToX during band transitions (divide by zero with spots visible)
- Fix Windows support bundle: resolve symlink to actual log file (#243)
- Fix memory recall to use per-pan commands (#236)
- Improved log file handling and diagnostics

---

🤖 Generated with [Claude Code](https://claude.com/claude-code)

73, Jeremy KK7GWY & Claude (AI dev partner)

---

## [v0.6.1] — 2026-03-24


Patch release with critical bug fixes, new features, and multi-pan stability improvements.

## New Features

### DX Spot Display (#228)
- **Spot markers on panadapter** — colored callsign labels at spotted frequencies with dotted tick lines
- **Click-to-tune** — click any spot label to tune to that frequency
- **Settings → Spots** — full control: enable/disable, font size, max stacking levels, vertical position, color override, clear all
- Works with FlexSpots, N1MM+, DXLabs, or any tool that sends `spot add` to the radio

### Keyboard Shortcuts (#234)
- **View → Keyboard Shortcuts** to enable (off by default for safety)
- Left/Right: nudge frequency by step size
- Shift+Left/Right: nudge by 10× step
- Up/Down: AF gain ±5
- T: toggle MOX | M: toggle mute | L: toggle tune lock
- [ / ]: cycle step size

### Frequency Calibration (#201)
- Radio Setup → RX tab: Cal Frequency field, Start button, Freq Offset (ppb) display
- Pre-populated from radio's `cal_freq` on connect

## Bug Fixes

- **Fix FWDPWR/SWR meter reading zero on all radios** (#233) — meter source is `TX-` not `TX`, broke in v0.5.8 PGXL fix
- **Fix SmartLink connection on UPnP radios** (#230) — now parses `public_upnp_tls_port`/`public_upnp_udp_port` (was connecting to port 65535)
- **Fix TGXL buttons not working** — handle was stuck at 0x00000000 from initial status, now upgrades to real handle
- **Fix preamp sync in Multi-Flex** (#232) — antenna preamp is shared hardware, now applied to all pans regardless of client
- **Fix VFO NR2 button not working** (#227) — connections now permanent per-VFO, not re-wired on focus switch
- **Fix scroll-to-tune only going one direction** — same-pan tuning uses immediate model update path
- **Fix crash on pan removal with active spots** — spot rebuild lambda now guarded with QPointer
- **Fix Windows support bundle** (#243) — resolves .lnk symlink to actual log file

## Multi-Pan Improvements

- Per-pan bandwidth, center, and dBm range drag commands (#236) — drags on one pan no longer affect other pans
- Layout restore on reconnect uses `applyPanLayout` to handle pan count mismatch from split mode

---

🤖 Generated with [Claude Code](https://claude.com/claude-code)

73, Jeremy KK7GWY & Claude (AI dev partner)

---

## [v0.6.0] — 2026-03-24


**Multi-pan is here!** This release introduces experimental support for multiple simultaneous panadapters on FlexRadio transceivers.

> ⚠️ **Experimental Feature** — Multi-panadapter support is functional but still under active development. Some interactions between pans may behave unexpectedly. We need your help testing and finding bugs. Please report any issues you encounter on the [Issues](https://github.com/ten9876/AetherSDR/issues) page.

## Multi-Panadapter Highlights

- **Layout Picker** — click +PAN in the status bar to choose from available layout options:
  - A / B (2 vertical), A | B (2 horizontal)
  - A|B / C (2 top + 1 bottom), A / B|C (1 top + 2 bottom)
  - A|B / C|D (2×2 grid), Single (1 pan)
- **Independent FFT + waterfall** per pan with native VITA-49 tile routing
- **Independent display settings** per pan (AVG, FPS, fill, gain, black level)
- **Click-to-tune** uses SmartSDR protocol (`slice m <freq> pan=<panId>`)
- **Layout persistence** — saved across restarts
- **Pan reduction** — switch from more pans to fewer, excess pans close automatically
- Dual-SCU radios (FLEX-6600/6700/8600, AU-520): up to 4 pans
- Single-SCU radios (FLEX-6400/8400, AU-510): up to 2 pans

## Other New Features

- **Heartbeat indicator** — title bar circle flashes green on each TCP ping, blinks red on missed beats
- **Keepalive ping** — sends `keepalive enable` + 1s ping timer (matches FlexLib protocol)
- **Reconnect dialog** — "Radio disconnected — Waiting for reconnect" on unexpected disconnect with auto-reconnect
- **Fast disconnect detection** — 5s discovery stale timeout with TCP ping fallback for routed/SmartLink
- **Low Bandwidth Connect** — connection panel checkbox for VPN/LTE/metered connections
- **PA inhibit during TUNE** — opt-in safety feature disables ACC TX output before tune, restores after
- **VFO TX badge toggle** — click to assign OR unassign TX from any slice
- **TGXL OPERATE disables TUNE/ATU/MEM** — TX applet buttons dimmed when external tuner is in OPERATE mode
- **VFO slider value labels** — AF gain, SQL, AGC-T show numeric values
- **RIT/XIT offset lines** — dashed lines on panadapter showing actual RX/TX frequencies
- **RTTY mark/space lines** — dashed M/S frequency lines on panadapter in RTTY mode
- **Multi-Flex indicator** — green "multiFLEX" badge in title bar with hover tooltip
- **Step size persistence** — tuning step saved across restarts
- **RTTY mark default from radio** — reads radio's value on connect
- **Show TX in Waterfall** — waterfall freezes during TX when disabled, multi-pan aware
- **Network MTU setting** — Radio Setup → Network → Advanced
- **XVTR panel** — 2×4 grid with auto-grow for configured bands
- **Fill slider label fix** — display panel Fill slider updates label
- **Additional protocol subscriptions** — cwx, dax, daxiq, radio, codec, dvk, usb_cable, spot, license

## Bug Fixes

- Fix volume slider sync: parse `audio_level` not `audio_gain` (#161)
- Fix exciter forward power disappearing when PGXL connected (#181)
- Fix NR2/RN2 button sync between spectrum overlay, VFO, and RX applet
- Fix NR2 freeze: spectrum overlay bypassed FFTW wisdom generation (#214)
- Fix crash on exit: proper MainWindow destructor stops DSP before teardown (#167)
- Fix SQL button showing enabled in digital modes (#192)
- Fix TNF toggle sending no command (#184)
- Fix FDX toggle: optimistic update since radio doesn't echo status (#188)
- Fix DAX channel not persisting (#180)
- Fix profile save overwrite UX (#177)

## Protocol Discoveries (SmartSDR pcap analysis)

- SmartSDR **never sends `slice set <id> active=1`** — active slice is client-side only
- Click-to-tune uses `slice m <freq> pan=<panId>` with radio-side slice routing
- Per-pan `xpixels/ypixels` must be pushed after creation (radio defaults to 50×20)
- Waterfall tiles use stream ID 0x42xxxxxx (distinct from FFT's 0x40xxxxxx)
- `keepalive enable` + `ping ms_timestamp=` every 1 second

## Known Multi-Pan Issues

- Bandwidth drag may affect rates on other pans (#236)
- Split mode interaction with multi-pan needs further testing
- Per-pan display settings are not pushed on reconnect (second pan gets radio defaults)
- Visual stepping when adding pans (pans appear one at a time before rearranging)

---

🤖 Generated with [Claude Code](https://claude.com/claude-code)

73, Jeremy KK7GWY & Claude (AI dev partner)

---

## [v0.5.8] — 2026-03-23

## v0.5.8 — Heartbeat, Reconnect, PA Safety & Bug Fixes

### New Features

- **Heartbeat indicator** — Small circle in title bar flashes green on each radio ping, blinks red when connection is lost
- **Reconnect on power cycle** — Dialog appears on unexpected disconnect with auto-reconnect via 5s retry timer and discovery detection (#209)
- **Fast disconnect detection** — Radio loss detected in ~5s via discovery timeout instead of 30-60s TCP timeout (#209)
- **PA inhibit during TUNE** — Safety feature: opt-in setting temporarily disables ACC TX output during tune to protect external amplifiers (#156)
- **Low Bandwidth Connect** — Checkbox in connection panel sends `client low_bw_connect` for VPN/LTE/metered links
- **Keepalive ping** — Sends `keepalive enable` + 1s ping (matches FlexLib), drives heartbeat for all connection types
- **VFO slider value labels** — AF gain, SQL, AGC-T sliders show numeric values in VFO widget (#198)
- **Step size persistence** — Tuning step saved/restored across restarts (#211)
- **TGXL OPERATE disables TUNE/ATU/MEM** — TX applet buttons dimmed when external tuner is in OPERATE mode (#197)

### Bug Fixes

- **NR2 freeze on first enable** — Spectrum overlay DSP panel bypassed FFTW wisdom generation, freezing UI. Now all paths use background thread with progress dialog (#214)
- **NR2/RN2 persistence** — Client-side DSP state saved on exit, restored on launch (#167)
- **Exciter power disappearing with PGXL** — FWDPWR meter index overwritten by amplifier's meter. Now filtered by source "TX" (#181)
- **VFO TX badge** — Now toggles (assign AND unassign), matching RX applet behavior (#213)
- **Volume slider sync** — Fixed status key `audio_level` (was `audio_gain`), sliders track Maestro/SmartControl changes (#161)
- **TGXL state detection** — BYPASS mode (`operate=1 bypass=1`) no longer disables TX applet buttons (#197)

### Closed Issues

#101, #116, #119, #132, #137, #149, #161, #173, #177, #180, #181, #185, #189, #198, #199, #213, #214, #218, #222 and more

### Wiki

- New page: [Low Bandwidth Connections](https://github.com/ten9876/AetherSDR/wiki/Low-Bandwidth-Connections)

---

🤖 Generated with [Claude Code](https://claude.com/claude-code)

---

## [v0.5.7] — 2026-03-23


Major bug fix and feature release — 25+ issues addressed across two marathon sessions.

### Bug Fixes
- **Crash on exit** (#167): double-free of VfoWidget buttons during Qt teardown; proper MainWindow destructor stops NR2/RN2/RADE before destruction
- **Volume sliders not syncing** (#161): wrong status key (`audio_gain` → `audio_level`); sliders now track Maestro/SmartControl and profile changes
- **FDX toggle not working** (#188): radio accepts but doesn't echo status — added optimistic update
- **TNF toggle not sending command** (#184): `setGlobalEnabled()` now emits `commandReady`
- **SQL showing green in digital modes** (#192): disabled button with distinct dimmed style in DIGU/DIGL; squelch saved/restored on mode switch
- **Fill slider label not updating** (#206): missing `setText()` in `valueChanged` handler
- **NR2/RN2 not syncing** between spectrum overlay, VFO widget, and RX applet
- **Show TX in Waterfall had no effect** (#207): waterfall now freezes during TX when disabled (multi-pan aware)
- **RTTY mark default hardcoded** (#200): now reads from radio status on connect

### New Features
- **Multi-Flex indicator** (#185): green "multiFLEX" badge in title bar with connected client tooltip
- **Configurable quick-mode buttons** (#191): right-click USB/CW/AM to assign any mode; SSB toggles USB↔LSB, DIG toggles DIGU↔DIGL
- **RTTY mark/space lines** (#189): dashed M/S frequency lines on panadapter in RTTY mode
- **RIT/XIT offset lines** (#199): dashed lines showing actual RX (slice color) and TX (red) frequencies
- **UI scaling** (#194): View → UI Scale (75%–200%) via `QT_SCALE_FACTOR`
- **Band plan overlay toggle** (#193): View menu checkbox to show/hide ARRL overlay
- **Network MTU** (#202): Radio Setup → Network → Advanced spin box
- **Station name** (#182): configurable in Radio Setup → Radio tab
- **VFO slider value labels** (#198): AF gain, SQL, AGC-T show live numeric values
- **XVTR panel** (#204): 2×4 grid with auto-grow for configured transverter bands
- **DAX channel persistence** (#180): saved/restored across restarts
- **Profile manager UX** (#177): selecting a profile populates the name field
- **Client-side DSP persistence**: NR2/RN2 state restored on launch
- **PA temp/voltage precision** (#195): XX.X°C and XX.XX V

### Improvements
- **macOS mic permission** (#157): proper `AVAuthorizationStatus` check with diagnostic logging
- **APD visibility**: hidden on radios that don't support it
- **FDX indicator**: matches TNF/CWX/DVK font size (24px)
- **Disabled button style**: reusable `kDisabledBtn` for dimmed non-clickable buttons

### Protocol
- Added `sub client all` subscription for real-time Multi-Flex client tracking
- Client disconnect status parsing (`client <handle> disconnected`)
- Network MTU sent on connect (`client set enforce_network_mtu=1 network_mtu=N`)

---

73, Jeremy KK7GWY & Claude (AI dev partner)

---

## [v0.5.6] — 2026-03-23



---

## [v0.5.5] — 2026-03-22



---

## [v0.5.4] — 2026-03-22



---

## [v0.5.3] — 2026-03-22



---

## [v0.5.2] — 2026-03-21

## What's Changed

- **Log file rotation**: timestamped filenames, keeps last 5 sessions
- **Multi-Flex discovery**: show connected client station name (e.g. "Available (Multi-Flex: Maestro)")
- **Default diagnostic logging**: Discovery, Commands, and Status logging enabled by default (pre-1.0)
- **Antenna Genius applet layout fix**: buttons and labels no longer clip in the 260px sidebar

Full changelog: https://github.com/ten9876/AetherSDR/compare/v0.5.1...v0.5.2

---

## [v0.5.1] — 2026-03-21



---

## [v0.5.0] — 2026-03-21



---

## [v0.4.17] — 2026-03-21



---

## [v0.4.16] — 2026-03-20



---

## [v0.4.15] — 2026-03-20

## SmartLink Remote Operation — Now Working!

Operate your FlexRadio over the internet from Linux. Connect via SmartLink and get full spectrum, waterfall, audio, and meters — just like being on the local network.

### What's new

- **SmartLink VITA-49 UDP streaming** — FFT spectrum, waterfall tiles, RX audio (float32 stereo), and meter data now stream over WAN. Previously only the TLS command channel worked; now the full receive path is functional.
- **WAN UDP registration protocol** — Implements FlexLib's `client udp_register handle=0x<HANDLE>` sent via UDP datagram (not TCP `client udpport`, which the radio rejects on WAN connections with error `0x500000AA`).
- **NAT pinhole keepalive** — Sends `client ping handle=0x<HANDLE>` via UDP every 5 seconds to maintain the NAT mapping for sustained remote sessions.
- **Pre-bound UDP socket** — Binds the VITA-49 UDP port before requesting SmartLink connection, passing `hole_punch_port` to the relay server so the radio knows where to send packets.
- **SmartLink debug logging** — Comprehensive logging across the full WAN connection flow (Auth0, SmartLink server, TLS handshake, UDP registration, VITA-49 packet arrival) for troubleshooting.
- **Fixed log file path** — Logs now write to `~/.config/AetherSDR/aethersdr.log` (was double-nested).
- **macOS DAX virtual audio bridge** (PR #93) — CoreAudio HAL plugin for DAX audio devices on macOS.
- **Fixed low SSB voice TX level** (PR #94) — +24 dB gain compensation for USB mic input.

### SmartLink requirements

- FlexRadio with SmartSDR+ subscription
- UPnP enabled on the radio's router (or manual UDP port forwarding for port 4993)
- NAT hole-punching for radios without UPnP is not yet implemented

### Known limitations

- Audio may be jittery on high-latency connections (jitter buffer not yet implemented)
- Opus compressed audio (PCC 0x8005) not yet supported — uses uncompressed float32 (~384 kbps)
- No WAN auto-reconnect on connection loss

### Tested

FLEX-8600 fw v4.1.5 via SmartLink with UPnP — all four VITA-49 stream types confirmed working (FFT 0x8003, waterfall 0x8004, audio 0x03E3, meters 0x8002) at ~330 KB/s sustained.

---

**Full changelog:** https://github.com/ten9876/AetherSDR/compare/v0.4.14...v0.4.15

---

## [v0.4.14] — 2026-03-19



---

## [v0.4.13] — 2026-03-19



---

## [v0.4.12] — 2026-03-19



---

## [v0.4.11] — 2026-03-19


### PC Audio Toggle
New **PC** button on the VFO widget audio tab controls whether audio plays through your PC speakers or the radio's physical outputs (line out, headphone, front speaker).

- **PC ON** (green, default) — audio streams to your PC via `remote_audio_rx`
- **PC OFF** (grey) — audio plays through the radio's line out / headphone / front speaker

This fixes the issue where users with powered speakers connected to the radio's line out jack couldn't hear audio (#71). The radio mutes its physical outputs when a `remote_audio_rx` stream is active — toggling PC Audio OFF removes that stream.

The setting persists across sessions.

### Also in this release
- 48kHz audio fallback for devices that don't support 24kHz
- AppImage rebuilt with Qt 6.7 via aqtinstall (fixes Ubuntu audio)
- Windows build bundles libfftw3-3.dll (#81)
- XVTR band sub-menu and context-aware frequency entry
- Direct frequency entry (double-click frequency display)
- NR2 UX improvements (3-state NR button, wisdom dialog)
- 4O3A Antenna Genius support (@EI6JGB)

### Downloads
| Platform | File |
|----------|------|
| Linux x86_64 | `AetherSDR-v0.4.11-x86_64.AppImage` |
| Linux ARM | `AetherSDR-v0.4.11-aarch64.AppImage` |
| macOS | `AetherSDR-v0.4.11-macOS-universal.dmg` |
| Windows | `AetherSDR-v0.4.11-Windows-x64.zip` |

### Full Changelog
https://github.com/ten9876/AetherSDR/compare/v0.4.10...v0.4.11

---

## [v0.4.10] — 2026-03-18


### Critical: Audio Fix for Ubuntu/Debian
- **AppImage rebuilt with Qt 6.7 via aqtinstall on Ubuntu 22.04 base** — fixes empty audio device dropdowns and no audio output on Ubuntu 24.04, Debian 12+, and other modern distros
- Root cause: Ubuntu 22.04's Qt 6.2.4 had no separate multimedia plugin — linuxdeploy couldn't bundle it. Now using Qt 6.7.3 which ships the multimedia backend as a proper loadable plugin
- Bundles GStreamer audio plugins (pulseaudio, alsa, pipewire) directly in the AppImage
- **48kHz fallback**: AudioEngine now automatically upsamples RX (24k→48k) and downsamples TX (48k→24k) for devices that don't natively support 24kHz
- Diagnosed with help from a second Claude instance running inside an Ubuntu 24.04 VM

### XVTR Transverter Support
- XVTR button in band grid opens transverter sub-panel with configured bands
- "HF" button returns to regular band grid
- Context-aware frequency entry: on XVTR bands, bare integers get decimal after 3rd digit (1446 → 144.6 MHz)

### Direct Frequency Entry
- Double-click the frequency display (VFO widget or RX applet) to type a frequency
- Accepts: 14.225 (MHz), 14225 (kHz), 14225000 (Hz), 14.225.000 (dotted)

### NR2 UX Improvements
- 3-state NR button: Off → NR → NR2 → Off
- FFTW wisdom dialog: progress after each plan, breathing animation, auto-close messaging

### Windows Fix
- Bundle `libfftw3-3.dll` in Windows ZIP (fixes #81)

### 4O3A Antenna Genius Support (contributed by @EI6JGB)
- UDP discovery, TCP control, per-port antenna grid, band→antenna memory

### Downloads
| Platform | File |
|----------|------|
| Linux x86_64 | `AetherSDR-v0.4.10-x86_64.AppImage` |
| Linux ARM | `AetherSDR-v0.4.10-aarch64.AppImage` |
| macOS | `AetherSDR-v0.4.10-macOS-universal.dmg` |
| Windows | `AetherSDR-v0.4.10-Windows-x64.zip` |

### Full Changelog
https://github.com/ten9876/AetherSDR/compare/v0.4.9...v0.4.10

---

## [v0.4.9] — 2026-03-18


### AppImage Audio Fix (Ubuntu/Debian)
The AppImage now bundles GStreamer and PipeWire multimedia plugins. This fixes the no-audio issue reported on Ubuntu 24.04 LTS (#75) where the PC audio device list was empty despite system audio working normally.

### Firmware Update Staging
New staged firmware update workflow in Settings → Radio Setup → Radio tab:
- **Check for Update** queries FlexRadio's website and compares versions
- **Download** fetches the SmartSDR installer, verifies MD5 against published hash
- **Extract** carves the .ssdr firmware file from the installer binary
- **Validate** checks the extracted file header before enabling upload
- **Browse .ssdr** still available for manual file selection
- Experimental feature — disclaimer included

### NR2 UX Polish
- 3-state NR button cycle in RX Applet: Off → NR → NR2 → Off
- NR2 toggle added to DSP side panel
- All NR2 buttons sync bidirectionally
- Buttons only show "on" after wisdom generation completes
- FFTW wisdom dialog: progress reports after each plan, breathing animation, auto-close messaging

### Platform Firmware Mapping
All consumer FlexRadios (Microburst/DeepEddy/BigBend/Aurora) use FLEX-6x00 firmware. Only DragonFire (FLEX-9600) uses separate firmware.

### Downloads
| Platform | File |
|----------|------|
| Linux x86_64 | `AetherSDR-v0.4.9-x86_64.AppImage` |
| Linux ARM | `AetherSDR-v0.4.9-aarch64.AppImage` |
| macOS | `AetherSDR-v0.4.9-macOS-universal.dmg` |
| Windows | `AetherSDR-v0.4.9-Windows-x64.zip` |

### Full Changelog
https://github.com/ten9876/AetherSDR/compare/v0.4.7a...v0.4.9

---

## [v0.4.7a] — 2026-03-18


Minor UX improvements to the NR2 spectral noise reduction feature introduced in v0.4.7.

### NR Button 3-State Cycle
The NR button in the RX Applet now cycles through three states:
- **Off** → click → **NR** (radio-side noise reduction)
- **NR** → click → **NR2** (client-side spectral noise reduction)
- **NR2** → click → **Off**

The button label changes to show which mode is active. NR and NR2 are never on simultaneously — the radio's NR is automatically disabled when switching to NR2.

### Wisdom Dialog Improvements
- Progress bar now advances **after** each FFT plan completes, not before — no more false 100% while the last plan is still computing
- Description label shows which plan is being computed during slow phases ("Computing COMPLEX-TO-REAL FFT size 262144...")
- Breathing opacity animation on the dialog when progress >= 90% (the slow phase) so users know it's still working
- "Wisdom generation complete!" confirmation before auto-close
- Light text on progress bar readable at all fill levels

### NR2 Button Sync
All three NR2 controls (VFO widget, DSP side panel, RxApplet) now stay in sync and only show "on" after wisdom generation completes and NR2 is actually processing audio.

### DSP Side Panel
- NR2 toggle button added to the left-side DSP overlay panel
- DSP panel top-aligned with button menu

### Downloads
Pre-built binaries auto-attach when CI completes (~5 min).

### Full Changelog
https://github.com/ten9876/AetherSDR/compare/v0.4.7...v0.4.7a

---

## [v0.4.7] — 2026-03-18


### NR2 Spectral Noise Reduction (contributed by @EI6JGB)

Client-side spectral noise reduction using the Ephraim-Malah MMSE Log-Spectral Amplitude estimator with OSMS noise floor tracking. This is a sophisticated DSP algorithm that complements the radio's built-in NR — particularly effective on weak signals where the radio's NR alone isn't enough.

**Features:**
- Toggle NR2 in the VFO widget DSP panel
- FFTW3 optimized FFTs with automatic radix-2 fallback when FFTW3 isn't installed
- Background FFTW wisdom generation — audio continues playing during optimization
- 1-second startup ramp for smooth transition as the noise estimator converges
- Anti-musical-noise temporal gain smoothing
- Decision-directed a priori SNR estimation with speech presence probability
- CPU cost: ~1-2% of one core at 24kHz mono processing

**Dependencies (optional):**
- Linux: `libfftw3-dev` (falls back to built-in FFT without it)
- macOS: `brew install fftw`
- Windows: bundled in `third_party/fftw3/`

Thank you @EI6JGB for this excellent contribution — real DSP engineering with proper overlap-add processing, Hann windowing, and numerical stability.

### Firmware Uploader

Update your radio's firmware directly from Linux — no Windows machine required.

- Settings → Radio Setup → Radio tab → Firmware Update section
- Browse for .ssdr firmware files
- Progress bar with chunked TCP upload
- Confirmation dialog with model verification
- Automatic port fallback (4995 → 42607)

The .ssdr files can be obtained from an existing SmartSDR installation (`C:\ProgramData\FlexRadio Systems\SmartSDR\Updates\`).

### Downloads

Pre-built binaries auto-attach when CI completes (~5 min):

| Platform | File |
|----------|------|
| Linux x86_64 | `AetherSDR-v0.4.7-x86_64.AppImage` |
| Linux ARM | `AetherSDR-v0.4.7-aarch64.AppImage` |
| macOS | `AetherSDR-v0.4.7-macOS-universal.dmg` |
| Windows | `AetherSDR-v0.4.7-Windows-x64.zip` |

### Full Changelog
https://github.com/ten9876/AetherSDR/compare/v0.4.6...v0.4.7

---

## [v0.4.6] — 2026-03-18


### ARRL Band Plan Overlay
- Color-coded sub-band segments on the bottom of the FFT display
- CW (blue), DATA (red), PHONE (orange), Beacon (cyan), Satellite (purple)
- License class shading: Extra (dim), General (medium), Tech (bright)
- White dot markers for spot frequencies (QRP calling, beacons, SSTV, AM calling, etc.)
- Hover over any dot for a tooltip with the frequency and description
- Source: ARRL band chart (rev. 1/16/2026) + Considerate Operator's Frequency Guide

### Waterfall Time Scale
- Time strip on the right edge of the waterfall (0s at top, increasing down)
- Auto-measured row interval — adapts when rate slider changes
- 5-second tick marks, always displayed in seconds

### TX Audio (PC Mic → Radio)
- PC microphone audio now streams to the radio via DAX when mic source = PC
- Auto-starts when selecting PC, stops when switching to BAL/MIC/LINE/ACC
- No longer forces mic_selection=PC on connect — respects radio's saved state

### Waterfall TX Transition
- Immediately falls back to FFT-derived waterfall rows during TX
- Immediately resumes native tiles on RX — no more 2-3 second pause

### Mic Metering
- `met_in_rx=1` enabled on connect for RX mic monitoring
- Gauge suppressed when monitoring is disabled

### Display Settings Persistence
- WNB on/off, WNB level, RF gain saved to settings and restored on connect

### UI Polish
- All overlays consume mouse/wheel events — no accidental VFO tuning
- Black level slider range fixed for proper noise floor suppression
- Rate slider range adjusted (sends 71-100 to radio)

### Linux ARM Support
- aarch64 AppImage built alongside x86_64 on every release

### Downloads

Pre-built binaries auto-attach when CI completes (~5 min):

| Platform | File |
|----------|------|
| Linux x86_64 | `AetherSDR-v0.4.6-x86_64.AppImage` |
| Linux ARM | `AetherSDR-v0.4.6-aarch64.AppImage` |
| macOS | `AetherSDR-v0.4.6-macOS-universal.dmg` |
| Windows | `AetherSDR-v0.4.6-Windows-x64.zip` |

### Full Changelog
https://github.com/ten9876/AetherSDR/compare/v0.4.5...v0.4.6

---

## [v0.4.5] — 2026-03-18


### Multi-Slice Operation
- Color-coded slice markers on spectrum (A=cyan, B=magenta, C=green, D=yellow)
- Click slice badge or VFO line to switch focus
- Independent TX assignment — click grey TX badge in VFO overlay to move TX
- Close (✕) and lock (🔒) buttons floating beside the VFO overlay
- +RX button and right-click "Close Slice" for slice management
- Off-screen slice indicators with frequency readout
- Clean single-slice startup (no race conditions with multiple slices)

### Tracking Notch Filters
- +TNF button creates a notch at the center of the active filter passband
- Right-click spectrum or waterfall to add/remove/configure TNFs
- Drag TNF markers to reposition
- Color-coded: yellow = temporary, green = permanent
- Width (50-500 Hz) and depth (Normal/Deep/Very Deep) via context menu
- Fixed race condition that could crash the radio (#69)

### Audio Controls
- New Audio tab in Radio Setup: line out, headphone, front speaker controls
- PC audio input/output device selection with live switching
- Mic source selector fully wired (MIC/BAL/LINE/ACC/PC)

### Other Improvements
- Dynamic mode list from radio (FDVU, FDVM, future modes auto-detected)
- FreeDV-aware DSP controls
- Saved routed radios auto-probe and reconnect on launch
- Security hardened logging (redacted credentials, restricted permissions)
- AI-assisted feature request guide for non-developer contributors
- Right-click context menu on spectrum and waterfall

### Downloads

Pre-built binaries auto-attach when CI completes (~5 min):

| Platform | File |
|----------|------|
| Linux | `AetherSDR-v0.4.5-x86_64.AppImage` |
| macOS | `AetherSDR-v0.4.5-macOS-universal.dmg` |
| Windows | `AetherSDR-v0.4.5-Windows-x64.zip` |

### Full Changelog
https://github.com/ten9876/AetherSDR/compare/v0.4.0...v0.4.5

---

## [v0.4.0] — 2026-03-17


### New Features

**Tracking Notch Filters (TNF)**
- Right-click spectrum or waterfall to add/remove TNFs
- Drag markers to reposition, adjust width and depth via context menu
- Color-coded: yellow = temporary, green = permanent (survives power cycles)

**SmartLink Remote Operation (beta)**
- Log in with your FlexRadio SmartSDR+ account
- Radio auto-discovered via SmartLink relay server
- Full command channel over TLS — tune, change modes, all controls work remotely
- UDP streaming (FFT, waterfall, audio) in progress — testers welcome

**Manual (Routed) Connection**
- Connect to radios on different subnets/VLANs where UDP broadcast doesn't reach
- Enter IP address, AetherSDR probes the radio and adds it to the list

**Audio Settings Tab**
- Line out gain/mute, headphone gain/mute, front speaker mute
- PC audio input/output device selection (live-switching)

**4-Channel CAT Control**
- Independent rigctld TCP server per slice (ports 4532-4535)
- PTY symlinks per channel (/tmp/AetherSDR-CAT-A through -D)
- PTT auto-switches TX to the keyed channel's slice

**Cross-Platform Builds**
- Linux AppImage, macOS universal DMG (Intel + Apple Silicon), Windows ZIP
- All auto-built via GitHub Actions on tagged releases

**Other Improvements**
- Dynamic mode list from radio (supports FDVU, FDVM, and future modes)
- Mode-aware DSP controls for FreeDV digital voice modes
- Security hardening: redacted credentials in logs, restricted log file permissions
- Right-click context menu on spectrum and waterfall
- Qt 6.2 compatibility fixes for Ubuntu 22.04

### Downloads

Pre-built binaries will be attached automatically when CI completes (~5 min).

| Platform | File |
|----------|------|
| Linux | `AetherSDR-v0.4.0-x86_64.AppImage` |
| macOS | `AetherSDR-v0.4.0-macOS-universal.dmg` |
| Windows | `AetherSDR-v0.4.0-Windows-x64.zip` |

### Full Changelog
https://github.com/ten9876/AetherSDR/compare/v0.3.90-beta...v0.4.0

---

## [v0.3.90-beta] — 2026-03-17


**This is a beta release.** SmartLink TCP command channel is fully working. UDP streaming (FFT, waterfall, audio) needs community testing.

### What's New: SmartLink

Operate your FlexRadio remotely over the internet — no VPN required.

**Working:**
- Log in with your FlexRadio SmartSDR+ account (email/password)
- Radio auto-discovered via SmartLink server
- Full TCP command channel: tune, change modes, adjust filters, all controls work
- Unified radio list shows both local (LAN) and remote (SmartLink) radios
- Stable TLS connection with automatic keepalive

**Needs Testing (call for testers!):**
- UDP streaming (FFT spectrum, waterfall, audio) over WAN
- We confirmed TCP works but UDP packets aren't arriving through carrier NAT (T-Mobile)
- **We need testers with home WiFi remote access** (not mobile hotspot) to verify UDP works with proper port forwarding
- Port forwarding required: TCP 4994 + UDP 4993 to radio's LAN IP

### How to Test SmartLink

1. **At home:** Enable SmartLink in SmartSDR → Settings → SmartLink Setup
2. **Forward ports:** TCP 4994 and UDP 4993 to your radio's LAN IP
3. **Test ports:** Click "Test" in SmartSDR's SmartLink Setup — both should be green
4. **Remote:** Build AetherSDR on a remote machine, log in with your FlexRadio account
5. **Report:** Share results in [Discussions](https://github.com/ten9876/AetherSDR/discussions) — especially if FFT/waterfall/audio works!

### Requirements
- SmartSDR+ subscription (for SmartLink access)
- Port forwarding on home router (TCP 4994, UDP 4993)
- FlexRadio with SmartLink enabled and registered

### Full Changelog
https://github.com/ten9876/AetherSDR/compare/v0.3.0...v0.3.90-beta

---

## [v0.3.0] — 2026-03-17


### Profile Management
Full profile management matching SmartSDR's Profiles menu:

**Profiles menu** (menu bar):
- Dynamic list of global profiles loaded from radio
- Active profile marked with checkmark
- Click to instantly load a global profile

**Profile Manager dialog** (Profiles → Profile Manager...):
- **Global tab**: Load, Save, Delete global profiles with name entry field
- **Transmit tab**: Manage TX profiles
- **Microphone tab**: Manage mic profiles
- **Auto-Save tab**: Toggle auto-save for TX/mic profile changes
- Lists refresh in real-time as the radio processes changes
- Delete confirmation dialog
- Double-click to load

### Critical Bug Fixes

**Profile load crash (SEGV)**: Loading a global profile that changes the slice caused a segfault in `AppletPanel::setSlice()` — the old SliceModel was deleted but its pointer wasn't cleared before disconnect. Fixed by nulling all slice pointers in `onSliceRemoved` before re-wiring.

**No audio after profile load**: After loading a profile, the radio destroys and recreates the slice, invalidating the old `remote_audio_rx` stream. The radio rejected duplicate stream creation with `5000008e`. Fixed by removing the old stream before creating a new one, chained via callback.

**No FFT after profile load**: The `m_panResized` flag wasn't reset when a slice was removed, preventing the panadapter from re-syncing with the radio's new display settings.

### Full Changelog
https://github.com/ten9876/AetherSDR/compare/v0.2.9...v0.3.0

---

## [v0.2.9] — 2026-03-17


### DAX Audio Channels UI
- DAX applet with Enable button, 4 RX level meters, 1 TX level meter
- Slice assignment labels update dynamically from radio state
- TX status shows active slice name during transmit, "Ready" otherwise
- UI ready for PipeWire integration (issue #15)

### rigctld Improvements
- Protocol v1 dump_state with all fields required by WSJT-X/Hamlib
- TCP_NODELAY on client connections for immediate responses
- Command-level debug logging for troubleshooting
- Fixed RX audio loss caused by stale `dax=1` on connect

### Applet Panel Cleanup
- Renamed buttons: VU, RX, TUN, TX, PHNE, P/CW, EQ, DIGI
- Tighter spacing to fit all 8 buttons without clipping

### Bug Fixes
- Reverted DAX PipeWire integration (not ready — TX audio format issues, RX routing conflicts)
- Removed automatic `transmit set dax=1` on connect that was silencing RX audio

### Full Changelog
https://github.com/ten9876/AetherSDR/compare/v0.2.8...v0.2.9

---

## [v0.2.8] — 2026-03-16


### Hamlib rigctld CAT Control
- TCP server on port 4532 — works with WSJT-X, fldigi, N1MM, and any Hamlib NET rigctl client
- Virtual serial port (PTY) at `/tmp/AetherSDR-CAT` for apps that need a serial device
- Autostart options in Settings menu for both rigctld and TTY
- Protocol v1 dump_state with full Hamlib field set
- New CAT Control applet in sidebar with slice selector, enable toggles, port/path configuration
- Credit: rigctld implementation contributed by @pepefrog1234

### Native Waterfall Tiles
- VITA-49 waterfall tiles (PCC 0x8004) now rendered natively with full frame assembly
- FFT spectrum and waterfall are fully decoupled — changing AVG, FPS, or dBm range affects only the FFT
- Waterfall appearance controlled independently via Gain, Black Level, and Rate

### Display Sub-Menu
- New Display panel on left overlay: AVG, FPS, Fill (opacity + color picker), Weighted Average
- Waterfall controls: Gain, Black Level (+ Auto from tile headers), Rate
- All 9 display settings persisted in XML settings file

### Radio Setup Dialog (Complete)
- All 8 tabs implemented: Radio, Network, GPS, TX, Phone/CW, RX, Filters, XVTR
- TX Band Settings popup with per-band power, interlock, and PTT routing
- Transverter management with create/remove/edit

### Other Improvements
- TX stuck fix: MOX unkeys immediately instead of 7-15s delay on fw v1.4.0.0
- Multi-Flex: independent operation alongside SmartSDR/Maestro clients
- XML settings persistence (SSDR-compatible format)
- Network diagnostics, memory channels, spot settings dialogs
- Desktop integration (`.desktop` file, icon, `cmake --install`)
- PC audio TX via DAX stream
- macOS build support

### Full Changelog
https://github.com/ten9876/AetherSDR/compare/v0.2.7...v0.2.8

---

## [v0.2.7] — 2026-03-16


### Native Waterfall Tiles
The waterfall display is now fully decoupled from the FFT spectrum:

- **Native VITA-49 waterfall tiles** (PCC 0x8004) re-enabled with full frame assembly from fragmented packets (2460 bins per frame across ~4 UDP packets)
- **Frequency-based pixel mapping** using tile FrameLowFreq + BinBandwidth with linear interpolation between bins
- **Independent intensity color mapping** via `intensityToRgb()` — waterfall appearance no longer affected by FFT dBm range, averaging, or FPS changes
- **Time-based row interpolation** between consecutive tile rows for smooth scrolling
- **2-second fallback** to FFT-derived waterfall if native tiles stop arriving
- **AutoBlackLevel** from tile headers piped to Display sub-panel Auto button

### Display Settings Persistence
All 9 display controls now save to `~/.config/AetherSDR/AetherSDR.settings` in real-time on every slider, button, or color change:

- `DisplayFftAverage`, `DisplayFftFps`, `DisplayFftFillAlpha`, `DisplayFftFillColor`, `DisplayFftWeightedAvg`
- `DisplayWfColorGain`, `DisplayWfBlackLevel`, `DisplayWfAutoBlack`, `DisplayWfLineDuration`

Settings are restored on app launch with overlay menu sliders synced to saved values.

### Display Control Tuning
- Auto Black Level defaults to **on**
- Black slider range 0–100 (internally scaled to 0–150)
- Gain range expanded to 120 dB at minimum for better dim/contrast control
- Rate slider 50–500ms
- FFT fill color picker changes both spectrum line and gradient fill

### Full Changelog
https://github.com/ten9876/AetherSDR/compare/v0.2.6...v0.2.7

---

## [v0.2.6] — 2026-03-16


### Display Sub-Menu
New Display panel on the left overlay menu with independent FFT and waterfall controls:

**FFT controls:**
- AVG slider (0-100) — radio-side FFT averaging depth
- FPS slider (5-30) — FFT frame rate
- Fill slider (0-100) + color picker — FFT gradient fill opacity and color
- Weighted Average toggle

**Waterfall controls:**
- Gain slider (0-100) — waterfall color intensity/contrast
- Black slider (0-125) + Auto — waterfall black level threshold
- Rate slider (25-500ms) — waterfall scroll speed

All controls send `display pan set` / `display panafall set` commands to the radio and update the local display simultaneously.

### XML Settings System
Migrated all client-side settings from Qt's QSettings INI format to an XML file at `~/.config/AetherSDR/AetherSDR.settings`:
- SSDR-compatible key names (PascalCase, True/False booleans)
- Auto-migration from old QSettings on first launch
- Per-station settings support
- Human-readable, hand-editable XML

### Settings Menu Dialogs
- **Network Diagnostics** — RTT, max RTT, RX/TX rates, packet drop stats
- **Memory Channels** — create/select/remove radio-side memory channels
- **Spot Settings** — spot display preferences (levels, font size, colors)

### Other Improvements
- Unified combo box styling via shared `ComboStyle.h` — all dropdowns now have consistent dark-themed appearance with painted down-arrow
- Auto-connect fix: disconnect now clears saved radio serial, preventing unwanted reconnect on app restart
- Updated STYLEGUIDE.md and CONTRIBUTING.md

### Full Changelog
https://github.com/ten9876/AetherSDR/compare/v0.2.5...v0.2.6

---

## [v0.2.5] — 2026-03-16


### Complete Radio Setup Dialog
All 8 tabs of the Settings → Radio Setup dialog are now fully implemented, matching SmartSDR's configuration interface:

**Radio** — Serial number, callsign, nickname, region, HW version, options, Remote On/multiFLEX toggles

**Network** — IP/mask/MAC display, DHCP/Static IP configuration, Enforce Private IP

**GPS** — Installed status, coordinates, grid square, altitude, satellite tracking, speed, freq error, UTC time

**TX** — Timing delays, TX profile selector, interlock polarity, max power, tune mode, show TX in waterfall. New TX Band Settings popup with per-band RF/Tune power, PTT inhibit, and RCA/ACC interlock checkboxes

**Phone/CW** — Mic BIAS/+20dB boost, PC audio input device selector, CW iambic A/B, paddle swap, CWU/CWL sideband, CWX sync, RTTY mark default

**RX** — GPSDO detection with auto/manual frequency calibration, 10 MHz reference source selector (Auto/TCXO/GPSDO/External), oscillator lock status, binaural audio, mute local when remote

**Filters** — Voice/CW/Digital filter sharpness sliders (Low Latency ↔ Sharp), Auto buttons, low latency digital modes checkbox

**XVTR** — Transverter management with sub-tabs per entry, create/remove, all fields editable (RF/IF/LO freq, LO error, RX gain, max power, RX only)

### Full Changelog
https://github.com/ten9876/AetherSDR/compare/v0.2.4...v0.2.5

---

## [v0.2.4] — 2026-03-16


### Bug Fix: Band Memory Deprecated
Per-band settings persistence has been temporarily disabled pending a full redesign (#9). The automatic band-crossing detection was incorrectly saving state during connection, causing 80m/40m band buttons to tune to 20m instead (#8). 

Band switching still works — clicking a band button tunes to the correct default frequency and mode. Per-band memory (remembering antenna, zoom, filter settings per band) will return in a future release with a more robust design.

### TX Settings Tab
New TX tab in the Radio Setup dialog:
- ACC TX, RCA TX1/TX2/TX3 timing delays
- TX Profile dropdown (populated from radio)
- RCA/Accessory interlock polarity settings
- Max Power limit (editable, 0-100%)
- Tune Mode selector (Single Tone / Two Tone)
- Show TX in Waterfall toggle

### Full Changelog
https://github.com/ten9876/AetherSDR/compare/v0.2.3...v0.2.4

---

## [v0.2.3] — 2026-03-16


### Bug Fix
- **Band menu tuning to wrong frequency** — Clicking 80m or 40m would sometimes tune to 20m instead. The band-crossing detection was incorrectly saving state during initial connection. Fixed by guarding with `m_updatingFromModel` flag. Stale band memory is automatically cleared on first launch of v0.2.3. (#8)

### Settings Dialog
- **Settings menu** added between File and View (Radio Setup, Network, FlexControl, multiFLEX, etc.)
- **Radio Setup** — Radio tab with serial number, callsign, nickname (editable), region, HW version, options, Remote On/multiFLEX toggles
- **Network tab** — IP address, subnet mask, MAC address, DHCP/Static IP configuration with Apply button, Enforce Private IP toggle
- **GPS tab** — Installed status, latitude/longitude, grid square, altitude, satellite tracked/visible, speed, freq error, UTC time

### Other
- macOS build support (Homebrew portaudio link path fix)
- Desktop integration (.desktop file + cmake install rules)
- System UTC clock fallback when no GPS installed
- DAX/Hamlib added to roadmap (#7)

### Full Changelog
https://github.com/ten9876/AetherSDR/compare/v0.2.2...v0.2.3

---

## [v0.2.2] — 2026-03-16


### Bug Fix: Multi-Client (Multi-Flex) Support

AetherSDR now operates correctly when another client (SmartSDR, Maestro) is already connected to the radio. Previously, connecting AetherSDR as a second client would cause:

- Waterfall displaying all red (processing the other client's FFT data)
- Zoom/scale changes replicating between clients
- Both clients tuning in sync (sharing the same slice)

**Fix:** Three-layer filtering by `client_handle`:
1. **Slice ownership** — only tracks slices belonging to our client
2. **Display status** — only processes our panadapter/waterfall status updates
3. **VITA-49 packets** — only decodes FFT/waterfall data from our stream IDs

AetherSDR now creates its own independent slice and panadapter when connecting to a radio with existing clients, enabling true Multi-Flex operation.

### Full Changelog
https://github.com/ten9876/AetherSDR/compare/v0.2.1...v0.2.2

---

