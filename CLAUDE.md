# Claude Code Project Guide for AetherSDR

> **Claude Code: read [`AGENTS.md`](AGENTS.md) for the full project guide.**
> This file exists only because Claude Code auto-loads `CLAUDE.md`.
> Everything project-wide — architecture, conventions, the constitution,
> commit signing, branch protection rules, the must-knows — lives in
> `AGENTS.md` so every AI tool reads the same source of truth.

## Why this is a thin file

AetherSDR's contribution surface is multi-agent in practice (six+
distinct AI tools touch the codebase). Each tool has its own
well-known file at a different path:

| Tool | File |
|---|---|
| Claude Code | `CLAUDE.md` (you are here) |
| OpenAI Codex CLI / spec-kit / Foundry | `AGENTS.md` (the canonical) |
| GitHub Copilot | `.github/copilot-instructions.md` |
| Gemini Code Assist | `GEMINI.md` |
| Aider | `CONVENTIONS.md` |

All of those are pointers to `AGENTS.md`. The duplication-by-pointer
pattern keeps each tool reading from its native discovery path without
forcing a single canonical file to be copied N times.

## Claude-Code-specific notes

These tidbits are about how Claude Code as a tool interacts with this
codebase. Everything else is in `AGENTS.md`.

- **`git ship` alias** — the project has a `git ship` shell alias
  documented in `AGENTS.md` that squashes local commits ahead of
  `origin/main`, creates a branch, pushes, opens a PR with
  auto-squash-merge enabled. Use it for batch shipping rather than
  one-commit-per-PR.
- **Skills directory** — Claude Code's skill loader picks up skills
  from `~/.claude/skills/`. The AetherClaude bot keeps its own copies
  under `~/build/aetherclaude/skills/`. Those are the authoritative
  source for any agent-orchestration logic.
- **Plans directory** — implementation plans live at
  `~/.claude/plans/`. They are conversation-scoped scratch files,
  not project-canon; `AGENTS.md`, `CONSTITUTION.md`, and
  `CONTRIBUTING.md` are.
- **Memory directory** — Claude Code's auto-memory lives at
  `~/.claude/projects/-home-jeremy-build-AetherSDR/memory/`. Index
  is `MEMORY.md`. Use it for cross-session continuity; everything in
  it is operator-scoped (per Principle XIII), not project-canon.

That's everything Claude-Code-specific. Read `AGENTS.md` for the rest.
