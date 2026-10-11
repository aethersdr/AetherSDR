---
title: "AI-Assisted Development"
slug: "/ai-assisted-development"
description: "AetherSDR is developed through an AI-augmented open-source workflow."
---

AetherSDR is developed through an AI-augmented open-source workflow. The project lead (Jeremy, KK7GWY) and the core contributors work mainly through [Claude Code](https://claude.com/claude-code); other contributors use Codex, Copilot, Cursor, Gemini, Aider and more; and the [AetherClaude](https://github.com/aethersdr/aetherclaude) bot triages issues, drafts implementation plans and opens PRs for issues labelled `aetherclaude-eligible`.

Every change passes the same gate whichever tool (or person) produced it: signed commits, green CI and human code-owner review before anything reaches `main`.

You don't need to be a C++ developer to contribute.

## The guide every AI tool reads

[**AGENTS.md**](https://github.com/aethersdr/AetherSDR/blob/main/AGENTS.md) is the canonical project guide: architecture, conventions, the must-knows, and path-scoped sub-guides under `docs/agents/` (settings, GUI, backends, Flex protocol, tests and CI). Each tool's own instruction file (`CLAUDE.md`, `GEMINI.md`, `.github/copilot-instructions.md`) is just a pointer to it.

The [Constitution](https://github.com/aethersdr/AetherSDR/blob/main/CONSTITUTION.md) and [GOVERNANCE.md](https://github.com/aethersdr/AetherSDR/blob/main/GOVERNANCE.md) outrank AGENTS.md. GOVERNANCE.md also sets the limits on what an AI agent may change without a person deciding.

## Setup

1. Install an AI coding tool. The project recommends **Claude Code** for consistency with the core team.
2. Fork the repository: https://github.com/aethersdr/AetherSDR/fork
3. Clone your fork: `git clone https://github.com/YOUR_USERNAME/AetherSDR.git`
4. Install Qt 6.12 with `scripts/setup/setup-qt.sh` and build. See [Building from Source](./building-from-source.md).
5. Set up commit signing. Tell your assistant *"read `docs/COMMIT-SIGNING.md` and help me set up commit signing"*; the top of that file is written for AI assistants to follow.

## Starting a feature

Start with a prompt like:

```
Read AGENTS.md, CONTRIBUTING.md and CONSTITUTION.md. I want to implement [feature] for issue #[number].
Plan the implementation before writing code.
```

### Tips

- **Be specific.** "Add a button that does X" works better than "improve the UI".
- **Test incrementally.** Build and run after each change.
- **Point at existing code**, for example "look at how RxApplet handles filters".
- **One issue per PR.**
- **Ask for an RFC** before any visual, UX or architecture change. AI agents must not decide those on their own.

## Rules your assistant must follow

AGENTS.md has the full list. The ones that catch people most often:

1. Use `AppSettings`, never `QSettings`.
2. Every colour is a ThemeManager token, with no hard-coded hex.
3. A control the radio cannot use is dimmed with a reason, never hidden.
4. Use `QSignalBlocker` when updating the UI from model signals.
5. Filter radio data by `client_handle` for Multi-Flex safety.
6. Sign every commit, and end the commit subject with the principle it honours (`Principle <N>.`).
7. The core RX path (discovery → connect → FFT → audio) must still work.

## Letting an AI drive AetherSDR

AetherSDR has a built-in **agent automation bridge** with an MCP server, so an AI assistant can observe and operate a running AetherSDR. This is useful for reproducing a bug and proving a fix. It is off by default; turn it on under **Radio Setup → Network → Advanced → Agent Automation (MCP)**. Transmit control is a separate opt-in. See [Automation Bridge and MCP](./automation-bridge-and-mcp.md).

## AI-to-AI debugging

If your agent needs to coordinate with the maintainers:

1. Open a GitHub issue with your AI's analysis, code references and proposed fix.
2. Include relevant log output. See [Support and Logging](./support-and-logging.md) for where logs live and which categories to enable.
3. The maintainers' agents monitor issues and will respond.

## See also

- [Contributing Guide](./contributing-guide.md)
- [Building from Source](./building-from-source.md)
- [Automation Bridge and MCP](./automation-bridge-and-mcp.md)
- [Docs Style Guide](./docs-style-guide.md)
- [CONTRIBUTING.md](https://github.com/aethersdr/AetherSDR/blob/main/CONTRIBUTING.md)
- [AGENTS.md](https://github.com/aethersdr/AetherSDR/blob/main/AGENTS.md)
