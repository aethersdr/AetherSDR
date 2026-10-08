---
title: "Docs Style Guide"
slug: "/docs-style-guide"
description: "How AetherSDR documentation pages are laid out and written."
---

How AetherSDR documentation pages are laid out and written. Every page follows the same skeleton, so readers know where to look, and contributors (human or AI) know what to write. The approach borrows from the [ArchWiki style guide](https://wiki.archlinux.org/title/Help:Style), the [Home Assistant documentation templates](https://developers.home-assistant.io/docs/documenting/create-page) and the [Diátaxis](https://diataxis.fr/) framework.

## Page Types

Each page is mainly one of four kinds. Don't mix them on one page; link instead.

| Type | Answers | Examples |
|------|---------|----------|
| **Tutorial** | "Teach me" — a guided first experience with a guaranteed result | [Your First Session](./your-first-session.md), [Before You Transmit](./before-you-transmit.md) |
| **How-to / feature guide** | "Help me do X" — setup and use of one feature | [CAT Control](./cat-control.md), [Split Operation](./split-operation.md), [Hermes-Lite 2](./hermes-lite-2.md) |
| **Reference** | "Look it up" — complete, dry, accurate tables | [Menu Reference](./menu-reference.md), [Keyboard Shortcuts](./keyboard-shortcuts.md) |
| **Explanation** | "Help me understand" — background and trade-offs | [DSP Noise Mitigation](./dsp-noise-mitigation.md), [GPU Rendering](./gpu-rendering.md) |

**Landing pages** (for example [FlexRadio](./flexradio.md), [Linux](./linux.md), [macOS](./macos.md), [Windows](./windows.md), [Supported Radios](./supported-radios.md)) are a second way in: they are organized by the reader's situation and link across all four types.

## Page Skeleton

Each page is one Markdown file in [`docs/user/docs/`](https://github.com/aethersdr/AetherSDR/tree/main/docs/user/docs), named after its slug (`cat-control.md` is served at `/cat-control`). Sections appear in this order. Omit a section that has nothing in it; never leave an empty heading.

```markdown
---
title: "Page Title"
slug: "/page-title"
description: "One sentence: what this is. Search results and link previews show it."
status: "Experimental"
applies_to: ["Hermes-Lite 2"]
platforms: ["Linux", "macOS", "Windows"]
---

:::info[Status]

**Status:** Experimental · **Applies to:** Hermes-Lite 2 · **Platforms:** Linux, macOS, Windows

:::

One or two untitled introductory paragraphs: what this is and why you'd use it.

## Requirements
## Setup
## Using <the feature>        (one or more task-oriented sections)
## Reference                   (tables of controls, settings, ranges, defaults)
## Known issues
## Troubleshooting
## Making a change

These docs are Markdown in the AetherSDR repository and change through an ordinary pull request:

1. Edit or add the page under `docs/user/docs/` (and `docs/user/sidebars.js` for a new page). The **Edit this page** link at the bottom of every page opens the file on GitHub.
2. Preview it with `npm start` in `docs/user`, or run `npm run build` to get the full link check. [`docs/user/README.md`](https://github.com/aethersdr/AetherSDR/blob/main/docs/user/README.md) has the details.
3. Open a pull request against `main`. A change to user-visible behaviour updates the matching page in the same pull request as the code.

The pages under `generated/` are written by `tools/docs/gen_reference.py` from the source; change the source and re-run the script rather than editing them.

## See also
```

The page title comes from the `title` front matter; don't repeat it as a `#` heading. Every new page also needs an entry in [`docs/user/sidebars.js`](https://github.com/aethersdr/AetherSDR/blob/main/docs/user/sidebars.js), or readers can only reach it through links.

### Status

Include the status fields and the matching `:::info[Status]` callout when support differs by radio or platform, or when the feature is not fully supported. Leave both out of pages that apply everywhere. The front matter is the machine-readable copy and the callout is what readers see, so keep them in step.

- **`status`** is one of: **Supported** · **Early** · **Experimental** · **Experimental, receive-only** · **Off by default**. These match the README's hardware tiers.
- **Mixed support:** when a page covers families at different tiers, **`status`** gives the tier of the best-supported family (usually FlexRadio), and **`applies_to`** marks every other family with its own tier in brackets, for example `["FlexRadio", "Hermes-Lite 2 (experimental)", "Networked Icom (early; IC-7300MK2 supported)"]`.
- **`applies_to`** lists the radio families the page covers: FlexRadio, Hermes-Lite 2, Networked Icom, ANAN-G2, RTL-SDR, KiwiSDR/Web-888, Demo. Use `["All radios"]` when it has no limits.
- **`platforms`** is included only when something is platform-limited. For example, MNR is macOS only, and BNR is Linux and Windows.

### Known issues vs Troubleshooting

These are separate sections and must not be merged.

- **Known issues** lists unsolved problems and current limitations. **Every entry links its open GitHub issue** in this form: `Short description of the symptom ([#1234](https://github.com/aethersdr/AetherSDR/issues/1234)).` When the issue closes, the entry is deleted. Don't write dates or version numbers into an entry; the linked issue carries that history.
- **Troubleshooting** covers problems that have a fix. Write each one as a `###` heading naming the **symptom** as the user experiences it, followed by the likely cause and numbered steps to fix it:

```markdown
### No audio from the speaker

The PC Audio path is off, or the wrong output device is selected.

1. Check the title-bar speaker icon: ...
2. Open **Settings → Radio Setup... → Audio** and ...
```

### See also

`See also` comes last. It holds links to other pages of these docs first, then external links. Never call it "External links" or "More resources".

## Writing Rules

- **Describe current behaviour.** No "previously", no "as of v26.x", no history. A one-time behaviour change that existing users will notice goes in a callout that starts `> **Upgrading from an older version:**`.
- **Use exact UI names in bold**, with menu paths written as **Tools → PSK Reporter...** and settings as **Settings → Radio Setup... → Peripherals**. Check them against `src/gui/MainWindow_Menus.cpp`.
- **Links between pages** are relative file links with the page title as the text: `[CAT Control](./cat-control.md)`, or `[pinning](./smartlink-setup.md#certificate-pinning)` for a heading. The site build fails on a link to a page or heading that does not exist. Link source docs with full GitHub URLs, such as `https://github.com/aethersdr/AetherSDR/blob/main/docs/HERMES.md`.
- **Placeholders** go in code: `` `<name>` ``. A bare `<name>` in prose is read as an HTML tag and disappears.
- **Spelling and voice:** British spelling (colour, behaviour), second person, short sentences.
- **Never invent a fact.** If you can't verify it against the source, leave it out.
- **Settings** live in `AetherSDR.db`. See [Settings and Backups](./settings-and-backups.md).

## See also

- [Contributing Guide](./contributing-guide.md)
- [AI-Assisted Development](./ai-assisted-development.md)
- [Translating the Docs](./translating-the-docs.md)
- [Diátaxis](https://diataxis.fr/)
- [ArchWiki: Help:Style](https://wiki.archlinux.org/title/Help:Style)
