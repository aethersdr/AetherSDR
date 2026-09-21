# CHANGELOG house style — the anatomy of a release section

The topical style dates from v26.7.3 (#4301); every prep since keeps it.
Before that, sections were Keep-a-Changelog `Added / Changed / Fixed`. Do not
revert to that shape, and do not touch the older sections that still use it.

## The section, top to bottom

```markdown
## [vX.Y.Z] — YYYY-MM-DD

### <operator-facing clause> · <second clause> · <structural clause last>

N merged changes from M human contributors, AetherClaude and K Dependabot updates within that total. <Two or three sentences: what the release is, operator-facing half first, structural half last.>

### <Topical section — what an operator sees first>

- **<Headline feature> (#NNNN).** <A paragraph: what it does, what changed for
  the operator, what is deliberately not included.>
- <One bullet per change, or one per theme with several citations> (#NNNN, #NNNN).

### <Per-radio-family section: Hermes-Lite 2>
### <Per-radio-family section: Flex, Icom and ANAN>
### <Persistence, audio, devices>
### Backend seam
### Headless engine (aetherd)
### Automation and bridge
### Project and packaging

- CI: … (#NNNN); … (#NNNN).
- `actions/<x>` moves to <ver> (#NNNN).            ← Dependabot bumps: one line

### Contributors

Thanks to **@login** (N commits — <what they did>), **@login** (N commits — maintainer; <what they did>), …, **@aethersdr-agent** (N commits — AetherClaude orchestrator; <what it did>). Dependabot contributed <K> dependency update<s>. Counts cover primary commit authors; co-author credit remains in the commit history.

Welcome to first-time contributors **@a**, **@b**!

73, <Name> <CALLSIGN> & <Tool> (AI dev partner)
```

Rules the reviews enforced:

- **The headline** is one line, clauses joined with ` · `, operator-facing half
  first and structural half last. Its **first clause becomes the GitHub release
  title** (`AetherSDR vX.Y.Z — <first clause>`) and the first line of the tag
  message, so it has to stand alone.
- **The intro** opens with the exact count sentence. The numbers come from the
  commit-to-PR mapping (`scripts/release_contents.py`), never from memory or
  from a `git shortlog` by display name. "Merged changes" is the number of
  PR-or-direct-commit units in the range; with squash merges that equals the
  commit count.
- **Section order is review-weight order**: what an operator sees first
  (menus, chains, maps, modes), then the per-radio-family sections, then
  persistence / audio / devices, then structural (backend seam, aetherd), then
  Automation, then Project and packaging last.
- **Every PR in the range is cited at least once**, headline features as a bold
  lead sentence with the PR number and a paragraph, everything else as one
  bullet per change or per theme. A commit with no PR is cited by short SHA.
- **Numbers come from the PR body.** Where a body disagrees with itself (the
  #4582 token counts in v26.8.1 read 25 in the headline and 24 in the body of
  the same entry), pick one and say which in the PR body.
- **State what ships**, not what was reverted, removed or tried.
- **Contributors** is one paragraph, descending by count, humans first and
  bots after them; the maintainer's entry carries `maintainer;` so it does not
  read as a drive-by contributor (a #4872 review nit); Dependabot is a sentence,
  not an entry. Handles come from the API login, never from a display name or a
  callsign — v26.8.3 credited a display name to a stranger's handle and listed
  a callsign as a second person, and both are still on `main`.
- **The welcome line appears only when there are first-time contributors.**
- **The sign-off** is `73, <name> <callsign> & <tool> (AI dev partner)` in the
  form of whoever is cutting: `73, Jeremy KK7GWY & Claude (AI dev partner)` or
  `73, Pat KI6BCJ & Codex (AI dev partner)`.

## Excerpt — v26.9.4 (#5861), first section only

```markdown
## [v26.9.4] — 2026-09-20

### AetherRX and AetherTX as one window each, Neural Noise Reduction and global precipitation · the Hermes-Lite 2 transmits through WDSP and reads its meters honestly

97 merged changes from 13 human contributors, AetherClaude and two Dependabot updates within that total. The operator-facing half rebuilds the receive and transmit chains into one window each, brings WDSP 2.10 in with Neural Noise Reduction as a seventh client-side NR method, puts worldwide precipitation on the PSK Reporter map and gives the TGXL and PGXL front-panel presentations. The Hermes-Lite 2 gets the largest share of the rest: an ALC that only reduces, a WDSP TXA modulator keyed on the air and made the default, a derived dBm reference, a wideband bandscope and an S-meter that no longer reads the noise floor from a peak-hold. Underneath, aetherd gains credential-bound transmit grants, and the recorder, DVK, PMS and settings store all stop lying about failed writes.

### Receive and transmit chains

- **AetherRX — the receive chain in one window (#5805).** Every page now reads the same way: switches along the top, the display under them, every knob in one row at the foot. The tab column down the left *is* the chain — each row carries an enable checkbox and a grip, and dragging a row rewrites the RX chain order. […]
- **AetherTX — the transmit chain in one window (#5819).** The same shape for the transmit side […]
- A checked `QPushButton` under the modem chrome now has a visible state, so BYPASS, Record and Play show when they are pressed (#5846).
```

and its Contributors block:

```markdown
### Contributors

Thanks to **@on8st** (38 commits — the Hermes-Lite 2 TX level stack, TXA, bandscope, dBm reference, meters and shared raw-IQ DSP), **@ten9876** (24 commits — maintainer; AetherRX and AetherTX, WDSP 2.10 and NNR, the 4O3A front panels and test repairs), **@rfoust** (13 commits — global precipitation, DeepFist, aetherd TX grants, recorder, DVK, PMS and settings persistence), […], **@WA8PAM** (1 commit — WSJT-X calling-me spots), **@aethersdr-agent** (2 commits — AetherClaude orchestrator; MQTT drive and the Tools menu). Dependabot contributed two dependency updates. Counts cover primary commit authors; co-author credit remains in the commit history.

73, Jeremy KK7GWY & Claude (AI dev partner)
```

(v26.9.4 had no first-time contributors, so it carries no welcome line.
v26.9.3's read `Welcome to first-time contributors **@crypticpy**, **@kgbvax**,
**@WA8PAM**, **@Chipensaw**!`.)

## The release-notes footer, verbatim

The GitHub release body is the CHANGELOG section minus its `## [vX.Y.Z]` line
and its `### <headline>` line, followed by this footer (from the v26.9.3
release; substitute the versions):

```markdown
### Downloads

Tag-triggered CI publishes Linux AppImages (x86-64 and aarch64), macOS DMGs (Apple Silicon and Intel), and Windows installers and portable ZIPs as jobs finish. Assets may still be building when this release first appears. macOS signing and notarization, and release checksum and signature generation, run in their respective workflows.

**Full diff:** [v26.9.2...v26.9.3](https://github.com/aethersdr/AetherSDR/compare/v26.9.2...v26.9.3). See [verification instructions](https://github.com/aethersdr/AetherSDR/blob/v26.9.3/docs/VERIFYING-RELEASES.md).

73, Jeremy KK7GWY & Claude (AI dev partner)
```

The sign-off already ends the Contributors block, so the release body carries
it once — at the very end, after the footer — not twice.

## The tag message, verbatim shape (v26.9.3)

```
AetherSDR v26.9.3 — A Tools-first menu bar, live map overlays and APRS digipeating

The menu bar is reorganized around what operators actually reach for, with a
first-class Tools menu ahead of View. PSK Reporter gains NOAA weather radar and
NASA city lights, AetherModem gains a WIDE1-1 fill-in digipeater, and the
waterfall gains clock-aligned time markers. Underneath, slice, capability and
transmit paths move onto the IRadioBackend seam, aetherd reaches Stages 3 and 4,
and the audio engine becomes rate-aware.

86 merged changes from 16 human contributors, AetherClaude and Dependabot.
Cut by Jeremy KK7GWY.
```

First line: `AetherSDR vX.Y.Z — <first clause of the headline>`. Then one
paragraph. Then the count line (no Dependabot number here) and `Cut by <name>
<callsign>.`
