# AetherSDR Project Governance

This document describes how AetherSDR is governed — who has authority over
what, how decisions are made, and how contributors can earn expanded roles.

See [CONTRIBUTING.md](CONTRIBUTING.md) for the mechanics of submitting code.

---

## Project Direction

AetherSDR's goal is a **cross-platform, natively-compiled** FlexRadio client that
matches SmartSDR feature-for-feature. Every technical and design decision
should be evaluated against that goal.

**Cross-platform first.** Platform-specific code is acceptable where it solves
a real problem, but it must be isolated (platform guards, separate files) and
cannot be the primary motivation for touching shared code. A macOS cosmetic
preference is not a sufficient reason to modify `MainWindow` or `TitleBar`.

**The project maintainer (Jeremy Fielder / KK7GWY) holds final authority on:**

- Visual design — colors, fonts, layout, theme
- UX behavior — how controls work, what clicks and shortcuts do by default
- Architecture — threading model, signal routing, new dependencies
- Feature scope — what is and isn't in scope for the project

This is not a committee. When there is disagreement about direction, the
maintainer's decision is final.

---

## Roles

### Contributor

Anyone who opens a PR. No special permissions. PRs are reviewed by the
maintainer or a Domain Maintainer with authority over the affected area.

### Triager

Can label, close, and comment on issues. Cannot merge PRs.

**How to earn it:** Demonstrated pattern of helpful, accurate issue responses.
Nominate yourself or ask in an issue — the maintainer will grant it.

### Domain Maintainer

Can review and merge PRs **within their designated area** without waiting for
maintainer review, subject to the RFC and CODEOWNERS requirements below.

Current domain areas:

| Area | Path(s) | Notes |
|------|---------|-------|
| Documentation | `resources/help/`, `docs/`, `*.md` | Help text, wiki, guides. Spans two CODEOWNERS tiers: `docs/` and `resources/help/` are Tier 3, bare `*.md` is Tier 2 |
| Build / CI | `CMakeLists.txt`, `.github/` | Build system, CI pipelines |
| Plugins | `plugins/` | Stream Deck, TCI plugins |
| Platform: macOS | `src/platform/macos/` | macOS-specific code only |
| Platform: Windows | `src/platform/windows/` | Windows-specific code only |

Domain Maintainers cannot merge PRs that touch files outside their area.
Changes that touch `src/gui/`, `src/core/`, or `src/models/` always require
maintainer review regardless of domain.

**How to earn it:** Three or more merged PRs in the area, demonstrated
understanding of the cross-platform requirements, and explicit agreement to
the project direction in this document. Open an issue titled
`[Governance] Domain Maintainer request: <area>` to start the conversation.

### Core Maintainer

Can review and merge PRs across most of the codebase, excluding areas
protected by CODEOWNERS. Expected to understand the full architecture and
the SmartSDR protocol.

This role does not currently exist outside the project maintainer. It will be
established when the project has contributors who have demonstrated sustained,
high-quality work across multiple areas over an extended period.

### Project Maintainer

Jeremy Fielder ([@ten9876](https://github.com/ten9876)). Final authority on
all decisions. Assisted by Claude (AI development partner) for implementation,
review, and issue triage — see the AI Contributors section below.

---

## RFC Process

Some changes require a written proposal — an RFC (Request for Comments) — to
be approved before implementation begins. This prevents the "implement first,
discuss never" pattern and ensures cross-platform implications are considered
up front.

### What requires an RFC

- Any change to **visual design** — colors, fonts, spacing, theme, icons
- Any change to **default UX behavior** — what a click, shortcut, or gesture
  does out of the box
- **New default keyboard bindings**
- **New external dependencies** (libraries, frameworks, system packages)
- **Architecture changes** — new threads, new signal routing patterns,
  changes to the audio pipeline
- **Platform-specific native integration** that touches shared code
  (e.g., embedding AppKit chrome into `MainWindow`)
- **New feature areas** substantially beyond the current scope

When in doubt, open an RFC issue first and ask.

### What does NOT require an RFC

- Bug fixes with a clear root cause
- Protocol compliance fixes matching SmartSDR behavior
- New shortcuts that are unassigned by default and additive
- Documentation additions and corrections
- Build / CI fixes
- New applets or dialogs that don't change existing UX
- Performance improvements that don't change behavior

### How to write an RFC

Open a GitHub issue with the label `rfc` and the title prefix `[RFC]`.
Describe:

1. **Problem** — what is broken or missing
2. **Proposal** — what you want to change
3. **Cross-platform impact** — how this affects Linux, macOS, and Windows
4. **Alternatives considered** — what else you looked at

The maintainer will comment with approval, rejection, or requested changes.
Do not open a PR until the RFC issue is approved.

---

## CODEOWNERS

PR review is gated by [`.github/CODEOWNERS`](.github/CODEOWNERS), which is the
authoritative source of who must approve what (last-match-wins). It defines
three tiers, broadest → most restrictive:

- **Tier 3 — source code, tests, and documentation** (`@aethersdr/reviewers`):
  all of `src/` — including the whole of `MainWindow` — plus `tests/`,
  `docs/`, and `resources/`, markdown included (so `resources/help/` is here
  too), plus anything not enumerated below. The broad reviewer roster;
  routine review of source, its tests, and its documentation all benefit from
  more eyes. `tests/` is here because it *is* source — ~90k lines of C++ —
  and because most code changes touch `src/` and `tests/` together. Two files
  under `docs/` are carved back to Tier 1 below, so "all of `docs/`" is the
  rule and not quite the whole story.
- **Tier 2 — infrastructure** (`@aethersdr/infrastructure`): `*.md` outside
  those directories (`README.md`, `CHANGELOG.md`, `ROADMAP.md`,
  `plugins/*/README.md`, …), `CMakeLists.txt`, the routine CI
  workflows under `.github/workflows/`, and the AI-instruction files
  (`AGENTS.md`, `CLAUDE.md`, `GEMINI.md`,
  `.github/copilot-instructions.md`, `.claude/commands/`).
- **Tier 1 — governance / security** (`@aethersdr/maintainers`): the governance
  docs (the Constitution — **both** its canonical copy at
  `.specify/memory/constitution.md` and the root `CONSTITUTION.md` mirror —
  plus `GOVERNANCE.md`, `CONTRIBUTING.md`, `CODE_OF_CONDUCT.md`, `LICENSE`),
  the security/compliance paths (`SECURITY*`, `.github/CODEOWNERS` itself, the
  CodeQL config, the release signing key and the `docs/VERIFYING-RELEASES.md`
  that publishes its fingerprint), and the workflows that hold release
  secrets or feed bytes into a signed artifact — which includes the CI-image
  build, since the CodeQL scan runs inside that image.

The lists above are a summary. `.github/CODEOWNERS` is the enforced artifact
and wins on any disagreement; the path-level breakdown for contributors is the
table in [`CONTRIBUTING.md`](CONTRIBUTING.md#reviews-and-merging). Deliberately
not restated here: the exact workflow filenames, which change as workflows are
added and renamed.

One subtlety worth knowing before editing `.github/CODEOWNERS`: a pattern with
no slash (`*.md`, `CMakeLists.txt`) — **or with only a trailing one**
(`docs/`, `tests/`) — matches at **any depth**. Only a *leading* slash
(`/docs/`) anchors a pattern to the repository root.

Two consequences. The `*.md` glob reaches markdown inside every directory, so
the `docs/`, `resources/`, and `tests/` lines have to come *after* it in the
file for those directories' markdown to stay at Tier 3 — CODEOWNERS resolves by
last match, not by specificity. And those three lines are themselves
unanchored, so they also claim `third_party/crdv/docs/` and
`third_party/crdv/tests/`. That is intended: a vendored tree's own docs and
tests belong with its code, which is already Tier 3.

Tier 1 is deliberately narrow: a path belongs there only if a wrong change to
it would alter **who decides things** or **what gets signed**. Documentation
that merely describes how to build the software — including the AI-instruction
files, which carry architecture, build steps, and style conventions rather than
policy — sits at Tier 2. Where an instruction file appears to conflict with
this document or the Constitution, those win; the conflict is a defect in the
instruction file, not a governance change, so gating it at Tier 1 bought
friction rather than control.

This reallocation does not touch **project direction**, which §Project
Direction reserves to the Project Maintainer regardless of who can approve an
edit to `ROADMAP.md`.

Self-approval is blocked by GitHub on every tier — your own PR always needs a
review from someone else. The Tier-1 paths are hard gates: no merge without
maintainer sign-off (an admin override is the only bypass). Team rosters live
in the GitHub org; `.github/CODEOWNERS` carries the exact patterns.

---

## AI Contributors

AetherSDR has two categories of AI involvement:

### AetherClaude (automated agent)

AetherClaude is an official automated contributor that monitors the issue
tracker and opens PRs for issues labeled `aetherclaude`.

**The following limits are normative and defined here.** They apply to every
automated agent that opens PRs against this repository, not only AetherClaude:

- **May autonomously fix:** bugs with a clear root cause, protocol compliance
  issues confirmed against FlexLib or a pcap, build/CI failures.
- **May not autonomously change:** visual design (colours, fonts, layout,
  theme), UX behaviour (what controls do, keyboard shortcuts), architecture
  (new threads, signal routing, new dependencies), feature scope beyond what
  the issue describes, or default values affecting all users.

When in doubt, the agent implements the fix and notes in the PR that a design
decision needs maintainer review. The Project Maintainer is the sole authority
on visual design and UX direction.

`AGENTS.md` §"Autonomous Agent Boundaries" elaborates these limits with
worked examples for agent consumption. That file is Tier 2 and **may not
widen them** — where it and this section differ, this section governs, and
the difference is a defect in `AGENTS.md`. Widening an automated agent's
autonomy is an amendment to this document and follows §Amendments.

Bot-opened PRs are reviewed under the same CODEOWNERS tiers as any other PR.
@AetherClaude is deliberately **not** a member of any code-owner team, so a
bot PR always requires approval from a human code owner of every tier it
touches, and the bot can never approve its own work or another agent's.

### AI-assisted human contributions

Contributions generated with AI tools (GPT, Claude, Copilot, etc.) are
welcome and held to **the same standards as any other PR**. The human
submitting the PR is responsible for understanding the change, testing it,
and ensuring it meets the project's guidelines. "Generated by AI" is not a
reason to relax review standards — if anything, it warrants closer scrutiny
of cross-platform correctness.

AI-assisted PRs that touch protected areas still require RFC approval first.

---

## Decision Making

For ordinary PRs, the process is simple: a Triager or Domain Maintainer can
comment, the maintainer reviews and merges or requests changes.

For significant decisions (RFC-required changes, new maintainer roles,
changes to this document), the maintainer may open a discussion period of
at least 5 days before deciding. Community input is welcome and considered,
but the maintainer's decision is final.

---

## Amendments

This document may be updated by the project maintainer at any time. Significant
changes will be announced in the GitHub Discussions or via a PR with the label
`governance` so the community can comment before the change lands.

---

*73 de KK7GWY*
