# AetherSDR user documentation

The source for **https://docs.aethersdr.com**, the AetherSDR user guide. It is
a [Docusaurus](https://docusaurus.io/) site: the pages are Markdown files in
`docs/`, and the sidebar is `sidebars.js`. `docs/` is the source of truth;
change it with a pull request.

## Preview locally

You need Node.js 20 or newer.

```sh
cd docs/user
npm ci
npm start
```

`npm start` opens a live-reloading preview at http://localhost:3000. Your
edits show under **Next (main)** (`/next/`; see [Versions](#versions)). Search
only works in a production build, so to try it:

```sh
npm run build
npm run serve
```

## The build is the link check

`docusaurus.config.js` sets `onBrokenLinks`, `onBrokenAnchors` and
`markdown.hooks.onBrokenMarkdownLinks` to `throw`. A link to a page or heading
that does not exist fails `npm run build`, locally and in CI. Link to other
pages by relative file path, for example `[CAT Control](./cat-control.md)` or
`[pinning](./smartlink-setup.md#certificate-pinning)`.

Pages are `.md` and are parsed as CommonMark (`markdown.format: 'detect'`), so
`<`, `>`, `{` and `}` in prose are ordinary text. Write a placeholder as
`` `<name>` `` in code, or as `&lt;name>`; a bare `<name>` is read as an HTML
tag and disappears. Admonitions (`:::info`, `:::tip`, `:::warning` and so on)
work in `.md` files.

The **Docs** workflow (`.github/workflows/docs.yml`) builds the site and the
PDF on every pull request that touches `docs/user/`, `tools/docs/` or the
workflow itself, and deploys both to Cloudflare Pages on a push to `main`.

## Writing pages

Follow the [Docs Style Guide](docs/docs-style-guide.md): page types, the page
skeleton, the status front matter, and the writing rules. A new page also
needs an entry in `sidebars.js`.

## Versions

| Version | Source | Served at |
|---|---|---|
| **Stable**, labelled with the release in `stable-version.json` | `versioned_docs/version-stable/`, `versioned_sidebars/` | `/`, the default |
| **Next (main)** | `docs/`, `sidebars.js` | `/next/`, with an "unreleased" banner |

There is one snapshot. Edit `docs/` only: the snapshot is replaced, never
edited, by `tools/docs/snapshot_stable.py`, which `/release-prep` runs for
every release. It copies `docs/` and the evaluated `sidebars.js` into the
snapshot, sets the label, and points the Log Analyzer's `/next/` rule links
at Stable for every page the snapshot now has:

```sh
python3 tools/docs/snapshot_stable.py 26.10.2
```

The PDF manual is built from `docs/`: Next on `main`, the tag's docs on a
release.

## Generated reference pages

The pages in `docs/generated/` are tables parsed from the C++ source
(shortcuts, controller actions, log categories, TCI commands) by
`tools/docs/gen_reference.py`. Don't edit them; change the source and run:

```sh
python3 tools/docs/gen_reference.py           # rewrite the pages
python3 tools/docs/gen_reference.py --check   # what CI runs
```

Their sidebar category is in `sidebar-extra.json`, which `sidebars.js`
appends, so a snapshot carries it too.

## Screenshots

`static/img/screens/` holds screenshots captured by
`tools/docs/capture_screenshots.py` from the steps in `screens.json`, each in
a fresh settings profile with the default dark theme. Most are taken on a real
FlexRadio on 20 m, so they show real signals; the pages that teach the demo
simulator (`your-first-session`, `demo-mode`) are shot against the demo. Popup
menus need a focused window on Wayland, so they are a separate offscreen
pass. Three passes, from a GPU build on a HiDPI display (Linux):

```sh
# Real radio (shots without "radio": "demo"); --no-netns so it can reach the LAN.
python3 tools/docs/capture_screenshots.py --build-dir build --allow-gpu-build \
    --platform wayland --no-netns --radio-serial <serial>
# The same radio, popup menus ("offscreen": true).
python3 tools/docs/capture_screenshots.py --build-dir build --allow-gpu-build \
    --platform offscreen --dpr 1.6667 --no-netns --radio-serial <serial>
# The demo pages ("radio": "demo"), in a private network namespace.
python3 tools/docs/capture_screenshots.py --build-dir build --allow-gpu-build --platform wayland
python3 tools/docs/capture_screenshots.py --build-dir build --allow-gpu-build \
    --platform offscreen --dpr 1.6667
python3 tools/docs/embed_screenshots.py
```

`--only id1,id2` re-shoots a subset. The script never transmits, connects only
to the serial it is given, and blacks out anything that identifies the station
in every image: IP and MAC addresses, the serial number, tailnet names, GPS
positions, the bridge token and the scratch paths, found from the widget tree
and by OCR (`tesseract`), plus any `redact` boxes in the manifest. It then
OCRs the result again and reports anything that still looks like one; look at
every image before committing it all the same.

`tools/docs/embed_screenshots.py` puts each shot on its `page` under its
`anchor_heading` (`intro` is the end of the introduction) as an `<img>` sized
to its logical width, so the HiDPI PNG stays sharp, with its alt text and
caption. It is idempotent, so re-shooting never duplicates an image; `--check`
reports a page that is out of step.

## PDF manual

`tools/docs/build_pdf.py` typesets the pages, in sidebar order, into one PDF
with Pandoc and Typst (templates in `tools/docs/pdf/`, the vendored symbol
font in `tools/docs/pdf/fonts/`). It needs `node`, `pandoc` 3.6 or newer and
`typst` 0.13 or newer:

```sh
python3 tools/docs/build_pdf.py --output AetherSDR-Manual.pdf
```

CI deploys it with the site as `/AetherSDR-Manual.pdf`, and attaches it to
every release with a detached signature made with the release key.

## Log Analyzer

`/log-analyzer` (`src/pages/log-analyzer.js`, `src/components/LogAnalyzer/`)
checks a log or support bundle in the browser against
`src/components/LogAnalyzer/rules.json`. Each rule cites the source line that
writes the message it matches and links the fix: `/<page>#<heading>` on
Stable, or `/next/<page>#<heading>` for a page only Next has.
`snapshot_stable.py` turns each `/next/` link whose page is in the new Stable
snapshot into a Stable link, so users of the release land on its docs. Test the
rules with:

```sh
node tools/docs/test_log_rules.mjs
```

It checks every cited message still exists in its source file (a message that
only moved is reported; `node tools/docs/test_log_rules.mjs --update` refreshes
the line numbers), every link against the version it points at, every link against the version
it points at, that each rule fires on its sample in `log-samples/rules/`, and
that `log-samples/clean.log` fires nothing. The Static checks workflow runs it
and `gen_reference.py --check` on every pull request.

## Translations

The site is set up for translation (`i18n` in `docusaurus.config.js`), with
English the only language so far. `npm run write-translations -- --locale
<locale>` generates the files under `i18n/<locale>/`;
[Translating the Docs](docs/translating-the-docs.md) covers the layout,
keeping a translation in sync, and the review rules.
