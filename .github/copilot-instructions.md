# GitHub Copilot Instructions for AetherSDR

**Canonical project guide: [`/AGENTS.md`](../AGENTS.md).** Read it
before suggesting non-trivial code. This file holds only the highest-
priority must-knows that fit in Copilot's chat context window.

## Five must-knows before suggesting code

1. **Signed commits required on `main`.** Branch protection enforces
   it. See `docs/COMMIT-SIGNING.md` for setup; the doc has explicit AI
   Assistant Instructions at the top.

2. **The AetherSDR Constitution governs every contribution.** See
   `CONSTITUTION.md` (canonical: `.specify/memory/constitution.md`).
   14 principles, structured per Cisco's
   [Foundry Constitution](https://github.com/CiscoDevNet/foundry-security-spec/blob/main/constitution.md)
   spec. Commit-message format: `Short description (#NNNN). Principle <N>.`
   when the change is principle-relevant.

3. **`bin/validate-diff.sh` is the merge gate, enforced by
   infrastructure** (Principle XII: Sandbox By Infrastructure, Not By
   Prompt). Don't suggest patterns that try to bypass it; the gate
   rejects PRs that touch protected paths regardless of prompt-level
   intent.

4. **Use `AppSettings`, NOT `QSettings`.** Persist as **nested JSON
   under one root key** per feature (Principle V). Example: a feature
   stores `{"enabled": true, "mode": "auto"}` under `AppSettings["MyFeature"]`,
   not `MyFeatureEnabled` + `MyFeatureMode` as flat keys.

5. **All meter UI uses `MeterSmoother`** (AGENTS.md convention). Do not
   suggest envelope followers, `std::pow`/`exp` smoothing, or
   asymmetric `kAlpha` blenders for new meter widgets.

6. **Assign yourself to an issue or PR before posting a review,
   comment, or merge action** (`gh issue edit NNNN --add-assignee @me`
   or `gh pr edit NNNN --add-assignee @me`). The assignees list is
   the visible multi-agent claim signal — Principle X.
   **AetherClaude (`@aethersdr-agent`) auto-triages everything, so
   adding yourself alongside it is expected**; only avoid
   double-assigning when another non-AetherClaude agent is already
   engaged — coordinate via comment in that case.

## C++ / Qt6 style highlights (full guide in AGENTS.md)

- C++20, Qt6, no `QSettings`, no naked `new`/`delete`, no `goto`,
  braces on all control flow.
- Platform guards prefer `Q_OS_WIN` / `Q_OS_MAC` / `Q_OS_LINUX`.
- For new files: classes `PascalCase`, methods `camelCase`, member
  vars `m_camelCase`, constants `kPascalCase`.

## Multi-agent context

AetherSDR is touched by ≥6 distinct AI tools. PRs from different
agents must verify their base is current before producing patches
(Principle X: Claims Are Atomic And Mortal — PR #2780 was the
canonical stale-snapshot revert incident this principle exists to
prevent).

For everything else, read `/AGENTS.md`.
