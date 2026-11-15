---
title: "Translating the Docs"
slug: "/translating-the-docs"
description: "How to add and maintain a translation of the AetherSDR documentation."
---

How to add and maintain a translation of the AetherSDR documentation. The site is built with [Docusaurus](https://docusaurus.io/), which keeps each language in its own folder next to the English pages, so a translation is a set of Markdown and JSON files that change through ordinary pull requests.

English is the source. Today it is the only language; the site is set up so that adding one is a pull request, not a rebuild.

:::note

Only these docs can be translated. The AetherSDR application itself is not translated yet: its menus, dialogs and messages are English only.

:::

## Requirements

- **At least two contributors per language.** A new language starts only when two people who read and write it commit to it: one translates, the other reviews, and they swap roles. A language with one contributor cannot be reviewed, so it is not added.
- **Nobody approves their own translation.** Every translation pull request is reviewed and approved by someone other than its author, and for the wording, by another speaker of that language.
- **Normal pull requests.** Translations follow the same rules as any other change: a fork, a branch, signed commits and review. See the [Contributing Guide](./contributing-guide.md).
- Node.js 20 or newer, to preview the site (see [`docs/user/README.md`](https://github.com/aethersdr/AetherSDR/blob/main/docs/user/README.md)).

Open an issue proposing the language first, naming both contributors. The pull request that adds it can follow once a maintainer agrees.

## Adding a language

The examples use French (`fr`); use your language's [locale code](https://en.wikipedia.org/wiki/IETF_language_tag).

1. Add the locale to `i18n.locales` in `docs/user/docusaurus.config.js`, for example `locales: ['en', 'fr']`. The commented example there shows the optional `localeConfigs` entry for the language's name in the menu.
2. Generate the files for the site's own text (navbar, footer, sidebar category names, version labels):

   ```sh
   cd docs/user
   npm ci
   npm run write-translations -- --locale fr
   ```

   This writes JSON files under `i18n/fr/`. Translate the `message` values and leave the keys alone.
3. Copy the pages you are translating into the locale's docs folder, keeping the file names, and translate them:

   ```sh
   mkdir -p i18n/fr/docusaurus-plugin-content-docs/current
   cp docs/first-connection.md i18n/fr/docusaurus-plugin-content-docs/current/
   ```

4. Preview the language: `npm start -- --locale fr`. Run `npm run build` before opening the pull request; it builds every locale and fails on a broken link or heading in any of them.

A page you have not translated yet falls back to the English one, so a language can start with a few pages and grow.

## The `i18n/` layout

```text
docs/user/i18n/fr/
├── code.json                                     # text in the site's own components
├── docusaurus-theme-classic/
│   ├── navbar.json
│   └── footer.json
├── docusaurus-plugin-content-docs/
│   ├── current.json                              # sidebar labels, Next (main)
│   ├── current/                                  # translated pages, Next (main)
│   │   └── first-connection.md
│   ├── version-stable.json                       # sidebar labels, Stable
│   └── version-stable/                           # translated pages, Stable
└── ...
```

Translate in `current/`, which follows `main`, the same as the English pages in `docs/user/docs/`. The `version-stable/` folder is the translation of the stable snapshot; it is refreshed from `current/` when a release is prepared, the same way the English snapshot is.

Keep each translated file's front matter `slug` and every heading's meaning: links from other pages point at the slug and at the heading anchors. To keep an anchor stable when the heading text changes in translation, give the heading an explicit id, for example `## Première connexion {#first-connection}`.

## Keeping a translation in sync

The English pages change with the application, and a translation does not change with them. To find what has moved on since you last updated a page, list the English page's history after the date of your last translation commit:

```sh
git log --oneline --since=2026-10-01 -- docs/user/docs/first-connection.md
git diff <commit>..main -- docs/user/docs/first-connection.md
```

Bring the translation up to date in a pull request that names the English commit it now matches. When an English page is renamed or removed, rename or remove the translated file in the same way, or the build fails on its links. Run `npm run write-translations -- --locale fr` again after sidebar or navbar changes: it adds new strings and keeps the ones you have already translated.

A translated page that is badly out of date is worse than the English fallback. If a language has no one keeping it current, its maintainers may remove the stale pages so readers get the English ones.

## See also

- [Contributing Guide](./contributing-guide.md)
- [Docs Style Guide](./docs-style-guide.md)
- [Docusaurus: i18n tutorial](https://docusaurus.io/docs/i18n/tutorial)
- [Kubernetes: Localizing Kubernetes documentation](https://kubernetes.io/docs/contribute/localization/)
