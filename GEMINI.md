# Gemini Code Assist Instructions for AetherSDR

**Canonical project guide: [`AGENTS.md`](AGENTS.md).** Read its core,
plus the `docs/agents/` sub-doc for the paths you touch, before
writing or recommending code. This file holds only
the must-knows that fit in Gemini's chat context efficiently.

## Must-knows before suggesting code

1. **Signed commits required on `main`.** Branch protection enforces
   it. See [`docs/COMMIT-SIGNING.md`](docs/COMMIT-SIGNING.md); the
   doc has explicit AI Assistant Instructions at the top — point
   yourself or the contributor at that doc for setup.

2. **The AetherSDR Constitution governs every contribution.** See
   [`CONSTITUTION.md`](CONSTITUTION.md), structured
   per Cisco's
   [Foundry Constitution](https://github.com/CiscoDevNet/foundry-security-spec/blob/main/constitution.md)
   spec. Commit-message format: `Short description (#NNNN). Principle <N>.`
   when the change is principle-relevant.

3. **Enforcement is by infrastructure, not prompt** (Principle XII):
   branch protection (required CI checks, CODEOWNERS review, signed
   commits) for every contributor, plus `bin/validate-diff.sh` — a
   path allow-list in the AetherClaude orchestrator's own repo that
   gates that bot's PRs. Don't suggest patches that try to work
   around either.

4. **Use `AppSettings`, NOT `QSettings`.** Persist as nested JSON
   under one root key per feature (Principle V). Example pattern:
   `AppSettings["MyFeature"] = {"enabled": true, "mode": "auto"}`.
   The store is SQLite (RFC #4603); credentials go to QtKeychain,
   never the settings store.

5. **All meter UI uses `MeterSmoother`** — never roll your own
   envelope follower. (`docs/agents/gui.md`.)

6. **Assign yourself to an issue or PR before posting a review,
   comment, or merge action** (`gh issue edit NNNN --add-assignee @me`
   or `gh pr edit NNNN --add-assignee @me`). The assignees list is
   the visible multi-agent claim signal — Principle X.
   **AetherClaude (`@aethersdr-agent`) auto-triages everything, so
   adding yourself alongside it is expected**; only avoid
   double-assigning when another non-AetherClaude agent is already
   engaged — coordinate via comment in that case.

7. **New or reworked UI follows the visual canon**
   (`docs/style/aethersdr-style-guide.md`, RFC #6226): `color.canon.*`
   tokens, `CanonWindow`, the spark's gold only for recognising people
   (warning amber and meter zones are unaffected). Apply it to UI the issue
   already changes; never restyle other screens unasked.

## C++ / Qt6 style essentials (full guide in AGENTS.md)

- C++20, Qt6, no `QSettings`, no naked `new`/`delete`, no `goto`,
  braces on all control flow.
- Platform guards prefer `Q_OS_WIN` / `Q_OS_MACOS` / `Q_OS_LINUX`.
- Naming: classes `PascalCase`, methods `camelCase`, member vars
  `m_camelCase`, constants `kPascalCase`.

## Multi-agent context

AetherSDR is touched by ≥6 distinct AI tools (Claude Code, OpenAI
Codex, Cursor, GitHub Copilot, Gemini, Aider, plus the AetherClaude
orchestrator bot). PR base must be verified current before producing
patches (Principle X: Claims Are Atomic And Mortal). Stale-snapshot
overwrites that silently revert prior merged work are the canonical
multi-agent failure mode this principle exists to prevent.

For everything else, read `AGENTS.md`.
