# MasterSDR Project Governance

This document describes how MasterSDR is governed — who has authority over
what, how decisions are made, and how contributors can earn expanded roles.

See [CONTRIBUTING.md](CONTRIBUTING.md) for the mechanics of submitting code.

---

## Project Direction

MasterSDR's goal is a **Linux-native, cross-platform** FlexRadio client that
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
| Documentation | `resources/help/`, `docs/`, `*.md` | Help text, wiki, guides |
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

The following paths require maintainer review on every PR regardless of who
else has approved:

```
src/gui/          @ten9876
src/core/         @ten9876
src/models/       @ten9876
CMakeLists.txt    @ten9876
```

Domain Maintainers can approve and merge PRs in their own areas. The paths
above are hard gates — no merge without maintainer sign-off.

---

## AI Contributors

MasterSDR has two categories of AI involvement:

### MasterClaude (automated agent)

MasterClaude is an official automated contributor that monitors the issue
tracker and opens PRs for issues labeled `MasterClaude`. It operates within
strict boundaries defined in `CLAUDE.md`:

- **May autonomously fix:** bugs with clear root cause, protocol compliance
  issues, build/CI failures
- **May not autonomously change:** visual design, UX behavior, architecture,
  feature scope, default values

MasterClaude PRs are reviewed by the maintainer before merge.

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
