---
title: "Contributing Guide"
slug: "/contributing-guide"
description: "AetherSDR welcomes bug reports, feature ideas, documentation and code."
---

AetherSDR welcomes bug reports, feature ideas, documentation and code. The rules live in the repository; this page is a short orientation.

## Requirements

Read these before you contribute:

| Document | What it is for |
|---|---|
| [CONTRIBUTING.md](https://github.com/aethersdr/AetherSDR/blob/main/CONTRIBUTING.md) | Contribution policy: what is accepted, who reviews what |
| [CONSTITUTION.md](https://github.com/aethersdr/AetherSDR/blob/main/CONSTITUTION.md) | The project's governing principles. Read it before writing or reviewing code. |
| [GOVERNANCE.md](https://github.com/aethersdr/AetherSDR/blob/main/GOVERNANCE.md) | Roles, decisions and the RFC process |
| [AGENTS.md](https://github.com/aethersdr/AetherSDR/blob/main/AGENTS.md) | The canonical project guide: architecture, conventions and must-knows, read by every contributor and AI tool |
| [docs/DEVELOPER-GUIDE.md](https://github.com/aethersdr/AetherSDR/blob/main/docs/DEVELOPER-GUIDE.md) | Architecture, threading, protocol reference, coding conventions |
| [docs/first-contribution-cheatsheet.md](https://github.com/aethersdr/AetherSDR/blob/main/docs/first-contribution-cheatsheet.md) | A beginner's on-ramp |
| [docs/COMMIT-SIGNING.md](https://github.com/aethersdr/AetherSDR/blob/main/docs/COMMIT-SIGNING.md) | Setting up commit signing on every platform |

Where AGENTS.md or the developer guide appears to conflict with CONSTITUTION.md or GOVERNANCE.md, the constitution and governance win.

## Contributing code

1. Browse [open issues](https://github.com/aethersdr/AetherSDR/issues). Issues labelled `good first issue` are good starting points.
2. Fork the repository and create a feature branch from `main`.
3. Build it. **Qt 6.12 is required**; `scripts/setup/setup-qt.sh` installs it. See [Building from Source](./building-from-source.md).
4. Implement the fix or feature (one issue per PR).
5. **Sign your commits.** `main` requires signed commits. SSH signing is the simple path; see [docs/COMMIT-SIGNING.md](https://github.com/aethersdr/AetherSDR/blob/main/docs/COMMIT-SIGNING.md).
6. Open a pull request against `main` referencing the issue (`Fixes #42`).

Every PR needs green CI and review from a code owner other than yourself.

Most AetherSDR development is done with AI coding tools. See [AI-Assisted Development](./ai-assisted-development.md).

### Key rules

- **Use `AppSettings`, never `QSettings`.** Settings live in the SQLite store; credentials go to the OS keychain, never into settings.
- **Every colour is a theme token.** Take colours from ThemeManager tokens, never hard-coded hex values, so the Default Light theme and user themes work. See `docs/theming/` and `docs/style/` in the repository.
- **Dim, don't hide.** A control the connected radio cannot use stays visible but dimmed, with the reason in its tooltip.
- **Accessible names.** New widgets need accessible names so screen readers can use them (see `docs/a11y.md`).
- **No feedback loops.** Use `QSignalBlocker` when updating the UI from model signals.
- **Multi-Flex safety.** Filter radio data by `client_handle`.
- **The radio is authoritative** for its own live state; FlexLib is the reference for protocol behaviour.
- **Cite the principle** your change honours at the end of the commit subject, for example `Principle V.`
- **One PR per issue.** Keep changes focused, and don't reformat code you are not changing.
- **Test the RX chain.** Discovery → connect → FFT → audio must still work.
- **Open an `[RFC]` issue first** for any UX, visual or architecture change.
- **Native only.** No Wine or CrossOver workarounds.

## Writing documentation

These docs live in the AetherSDR repository under [`docs/user/docs/`](https://github.com/aethersdr/AetherSDR/tree/main/docs/user/docs), one Markdown file per page, and change through an ordinary pull request. Every page follows a standard skeleton, status front matter and set of writing rules, described in the [Docs Style Guide](./docs-style-guide.md). Read it before adding or restructuring a page.

- **Fixing a page:** use the **Edit this page** link at the bottom of it, or edit the file in your fork, and open a pull request against `main`.
- **Changing behaviour:** a pull request that changes what users see or do updates the matching page in the same pull request.
- **Checking your change:** `npm run build` in `docs/user` fails on any link to a page or heading that does not exist. [`docs/user/README.md`](https://github.com/aethersdr/AetherSDR/blob/main/docs/user/README.md) covers previewing, screenshots and the generated reference pages.
- **Translating:** see [Translating the Docs](./translating-the-docs.md).

## Reporting bugs

The fastest path is **Help → File an Issue...** inside AetherSDR. It builds a support bundle, copies an AI prompt to your clipboard, and opens GitHub's bug-report form pre-filled with your system details and a redacted log tail. See [Support and Logging](./support-and-logging.md) for log categories and privacy notes, and [Troubleshooting](./troubleshooting.md) for how to attach a crash report.

If you'd rather file by hand, use the [bug report template](https://github.com/aethersdr/AetherSDR/issues/new?template=bug_report.yml) and include:

- what happened vs. what you expected
- radio model and firmware version
- OS and AetherSDR version (**Help → About AetherSDR** shows both and lets you copy the build details)
- the log: in `~/.config/AetherSDR/logs/` on Linux, `~/Library/Preferences/AetherSDR/logs/` on macOS, `%LOCALAPPDATA%\AetherSDR\logs\` on Windows

## Requesting features

Use **Help → Submit your Idea... 💡**. It copies a structured prompt into Claude, ChatGPT, Gemini, Grok or Perplexity, so you can describe your idea in plain English and have the AI write the issue body, then opens the right GitHub template.

To file by hand, use the [feature request template](https://github.com/aethersdr/AetherSDR/issues/new?template=feature_request.yml). Describe the problem you are solving, reference SmartSDR behaviour where it helps (screenshots are welcome), and keep to one feature per issue.

## See also

- [AI-Assisted Development](./ai-assisted-development.md)
- [Building from Source](./building-from-source.md)
- [Docs Style Guide](./docs-style-guide.md)
- [Translating the Docs](./translating-the-docs.md)
- [Support and Logging](./support-and-logging.md)
- [CONTRIBUTING.md](https://github.com/aethersdr/AetherSDR/blob/main/CONTRIBUTING.md)
