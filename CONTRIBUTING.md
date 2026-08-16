# Contributing to AetherSDR

Thanks for your interest in AetherSDR! We're building a native SmartSDR
client for FlexRadio on Linux, macOS, and Windows. Community contributions
are welcome.

This file is the **policy** side of contributing: what we accept, who
reviews what, and the ground rules. The **operational** side — architecture,
protocol, coding conventions, commit signing — lives in
[`docs/DEVELOPER-GUIDE.md`](docs/DEVELOPER-GUIDE.md).

See [GOVERNANCE.md](GOVERNANCE.md) for project roles, decision-making, and
the RFC process for significant changes.

---

## Quick Start

1. Browse [open issues](https://github.com/aethersdr/AetherSDR/issues) —
   issues labeled `good first issue` are great starting points.
2. Fork the repo and create a feature branch from `main`.
3. Implement the fix or feature (one issue per PR).
4. Open a pull request referencing the issue number (`Fixes #42`).

---

## Reporting Bugs

- Use the **lightbulb button** in AetherSDR's title bar for AI-assisted bug
  reports, or open a [GitHub issue](https://github.com/aethersdr/AetherSDR/issues/new) directly.
- Include: OS/distro, AetherSDR version, radio model, firmware version.
- Attach logs (`~/.config/AetherSDR/aethersdr.log`) or use Help → Support → Send to Support.
- Check existing issues first to avoid duplicates.

## Suggesting Features

- Open a GitHub issue or use the lightbulb button for an AI-assisted feature request.
- Describe the problem you're solving, not just the solution.
- Reference SmartSDR behavior where applicable — screenshots help.
- One feature per issue.

---

## Submitting Code

**Development tool:** AetherSDR is developed using [Claude Code](https://claude.com/claude-code)
as the primary development environment. We **strongly encourage all contributors to use
Claude Code** — it has full codebase context via `AGENTS.md` and naturally produces code
that matches our conventions.

1. **Fork the repo** and create a feature branch from `main`.
2. **Read the [AetherSDR Constitution](CONSTITUTION.md).** (Canonical
   source: [`.specify/memory/constitution.md`](.specify/memory/constitution.md);
   the root [`CONSTITUTION.md`](CONSTITUTION.md) is a byte-identical
   mirror.) **14 principles total** (constitution v2.0.0): 7
   AetherSDR-specific (FlexLib authority, radio-authoritative live
   state, radio-persistable settings, clean-room contributions,
   per-feature config ownership, transmit-on-intent, boundary input
   validation) + 7 defensive engineering principles adopted from
   Cisco's
   [Foundry Constitution](https://github.com/CiscoDevNet/foundry-security-spec/blob/main/constitution.md)
   (Evidence Over Assertion, Surface Only What Survives, Atomic Claims,
   Demonstrated Fixes, Infra Sandbox, Operator Outranks Agents, Atomic
   Persistence). Your PR's commit message will cite the principle it
   honors (e.g. `Principle V.` for nested-JSON persistence or
   `Principle X.` for verified-base patch generation).
3. **One issue per PR.** Keep changes focused and reviewable.
4. **Follow the coding conventions** in
   [`docs/DEVELOPER-GUIDE.md`](docs/DEVELOPER-GUIDE.md#coding-conventions).
5. **Test your changes** against a real FlexRadio if possible.
6. **Sign your commits** (required by branch protection — SSH or GPG; see
   [Commit Signing](docs/DEVELOPER-GUIDE.md#commit-signing)).
7. **Open a pull request** against `main` with a clear description.

---

## Developer documentation

Everything you need to actually write the code:

| Document | Covers |
|---|---|
| [`docs/DEVELOPER-GUIDE.md`](docs/DEVELOPER-GUIDE.md) | Project architecture, thread model, `MainWindow` rules, SmartSDR protocol reference, C++/Qt conventions, optional-dependency flags, commit signing |
| [`docs/PR-WORKFLOW.md`](docs/PR-WORKFLOW.md) | Draft-PR conventions, stale-branch policy, recovering from a red `main` |
| [`AGENTS.md`](AGENTS.md) | The canonical, exhaustive project guide — the source every AI tool reads |
| [`docs/COMMIT-SIGNING.md`](docs/COMMIT-SIGNING.md) | Full cross-platform signing setup (SSH and GPG) |
| [`docs/first-contribution-cheatsheet.md`](docs/first-contribution-cheatsheet.md) | Beginner's on-ramp |

---

## Reviews and merging

PR review responsibility is divided into three tiers via
[`.github/CODEOWNERS`](.github/CODEOWNERS). Self-approval is blocked by
GitHub on every tier — your own PR always needs review from someone else.

| Tier | Paths | Who can approve |
|---|---|---|
| **Source, tests & documentation (Tier 3)** | Everything not listed below — all of `src/`, **including the whole of `MainWindow`** — plus `tests/`, `docs/`, and `resources/`, **markdown included** (so `docs/DEVELOPER-GUIDE.md` and the in-app help text under `resources/help/` are both here). Two files under `docs/` are carved back to Tier 1 below | `@aethersdr/reviewers` (@ten9876, @jensenpat, @NF0T, @rfoust, @chibondking) |
| **Infrastructure (Tier 2)** | `*.md` *outside* `docs/`, `resources/`, and `tests/` (`README.md`, `CHANGELOG.md`, `ROADMAP.md`, `plugins/*/README.md`, and the AI-instruction files `AGENTS.md` / `CLAUDE.md` / `GEMINI.md` / `.github/copilot-instructions.md`), `.claude/commands/`, `CMakeLists.txt`, `THIRD_PARTY_LICENSES`, the routine `.github/workflows/`, `.github/dependabot.yml`, `.github/docker/`, `.github/ISSUE_TEMPLATE/` | `@aethersdr/infrastructure` (@ten9876, @jensenpat, @rfoust) |
| **Maintainer-only (Tier 1)** | Governance docs (`CONSTITUTION.md` **and its canonical copy `.specify/memory/constitution.md`**, `GOVERNANCE.md`, `CONTRIBUTING.md`, `CODE_OF_CONDUCT.md`, `LICENSE`), security/compliance (`SECURITY*`, `.github/CODEOWNERS`, `.github/codeql/`, `docs/RELEASE-SIGNING-KEY.pub.asc` and the `docs/VERIFYING-RELEASES.md` that publishes its fingerprint), and the workflows that hold release secrets, feed bytes into a signed artifact, or form part of the CodeQL scanner's trust chain (`sign-release.yml`, `codeql.yml`, `macos-dmg.yml`, `windows-installer.yml`, `appimage.yml`, `docker-ci-image.yml`, `streamdeck-plugins.yml`) | `@aethersdr/maintainers` (@ten9876) |

The maintainer-only tier is deliberately narrow: it covers the rules of the
project and the paths that can compromise a signed release. Everything that
is *documentation about how to build the thing* — including the AI-instruction
files — sits at Tier 2, so day-to-day iteration isn't maintainer-gated.

Tests and documentation sit at Tier 3 alongside the code they belong to. A
guide under `docs/`, a help topic under `resources/help/`, or a unit test under
`tests/` is best reviewed by the same people who review the code it covers —
and `tests/` is source code in its own right: ~90k lines of C++ against three
files of harness glue. Most code changes touch `src/` and `tests/` together, so
splitting them across tiers taxed nearly every well-tested PR for no gain.

Note the ordering rule behind this, if you ever edit `.github/CODEOWNERS`:
`*.md` has no slash, so it matches markdown at **any** depth, and CODEOWNERS
resolves by *last match*, not by specificity. The `docs/`, `resources/`, and
`tests/` lines only work because they sit below the `*.md` glob — move them
above it and every `.md` under those directories quietly falls back to Tier 2.
`CMakeLists.txt` has the same no-slash behaviour, which is why build config
still reaches Tier 2 wherever it lives.

A trailing slash does not anchor a pattern either: only a *leading* slash
(`/docs/`) pins one to the repository root. So `docs/` and `tests/` also claim
`third_party/crdv/docs/` and `third_party/crdv/tests/` — intended, since a
vendored tree's own docs and tests belong with its code, which is already
Tier 3.

`AGENTS.md`, `CLAUDE.md`, and `GEMINI.md` are Tier 2 because their content is
operational: architecture, build steps, style guide, protocol notes. The policy
those files must not contradict lives in [`CONSTITUTION.md`](CONSTITUTION.md)
and [`GOVERNANCE.md`](GOVERNANCE.md), which remain Tier 1 and outrank them.

The routine CI workflows and build config sit at Tier 2 as well; only the
security-sensitive workflows — the ones that hold release secrets or feed bytes
into a signed artifact — are carved back to Tier 1.

`MainWindow` is **not** maintainer-gated. With the #3351 decomposition
complete, the core **`MainWindow.{h,cpp}`** and its extracted **`MainWindow_*.cpp`
sibling TUs** (`MainWindow_DigitalModes.cpp`, `MainWindow_Wiring.cpp`,
`MainWindow_Controllers.cpp`, etc.) all sit at the Source (Tier 3) reviewer
tier, so the whole MainWindow surface shares the broad reviewer roster — a
primary goal of the decomposition was widening review of that code to the team.

Bot-opened PRs (e.g. @AetherClaude's) still require a human reviewer regardless
of tier — the bot is intentionally **not** a code owner. The Infrastructure
tier simply means changes to the repo's own scaffolding (dependency bumps,
issue templates, CI config, build config) need an infrastructure owner rather
than a maintainer.

For the mechanics around this gate — draft-PR conventions, the stale-branch
policy, and how to recover a red `main` — see
[`docs/PR-WORKFLOW.md`](docs/PR-WORKFLOW.md).

---

## What We Will Not Accept

- **Wine/Crossover workarounds.** The goal is fully native.
- **Copied proprietary code.** Clean-room implementations from observed
  protocol behavior and FlexLib source are fine.
- **Changes that break the core RX path.** Test: discovery → connect →
  FFT display → audio output.
- **Large reformatting PRs.** Fix style only in files you're modifying.
- **UX, visual, or architecture changes without an approved RFC.** Open
  a `[RFC]` issue first — see [GOVERNANCE.md](GOVERNANCE.md).

---

## AI-Assisted Feature Requests

**You don't need to be a developer to contribute.** Click the lightbulb
button in AetherSDR's title bar — it copies a structured prompt to your
clipboard and opens your choice of AI assistant. Describe your idea in
plain English, and the AI generates a well-structured GitHub issue.

### What makes a good request

- **Be specific.** "Add a noise gate with adjustable threshold" not "better audio."
- **Describe the problem.** Tell us *why*, not just *what*.
- **Reference SmartSDR.** Screenshots of the Windows client are very helpful.
- **One feature per issue.**

---

## Notes for AI Agents

Read [`AGENTS.md`](AGENTS.md) first — it is the authoritative project context
for every AI tool. The task-to-file quick reference and the AI-to-AI
coordination protocol are in
[`docs/DEVELOPER-GUIDE.md`](docs/DEVELOPER-GUIDE.md#notes-for-ai-agents).

`AGENTS.md` is documentation, not policy: where it appears to conflict with
[`CONSTITUTION.md`](CONSTITUTION.md) or [`GOVERNANCE.md`](GOVERNANCE.md),
those files win, and the conflict is a bug in `AGENTS.md` worth reporting.

The limits on what an automated agent may change without a human deciding are
defined in [`GOVERNANCE.md`](GOVERNANCE.md#ai-contributors) §AI Contributors,
which is Tier 1. `AGENTS.md` §"Autonomous Agent Boundaries" elaborates them
but may not widen them.

---

## Code of Conduct

Be respectful, constructive, and patient. Ham radio has a long tradition
of helping each other learn — bring that spirit here.

73 de KK7GWY

## License

By contributing to AetherSDR, you agree that your contributions will be
licensed under the [GNU General Public License v3.0](LICENSE).
