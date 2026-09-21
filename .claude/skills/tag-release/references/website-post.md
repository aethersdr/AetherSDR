# The website post — `aethersdr/aetherweb` checklist

One post per release, slug `release-X-Y-Z`, written the day of the tag.
`README.md` § "Adding a blog post" and § "Machine-readable surface" in that
repo are the authority; this is the release-specific path through them.

## Files touched

| file | change |
|---|---|
| `scripts/gen-blog-art.py` | prepend `("release-X-Y-Z", "vX.Y.Z", "<kicker>", "<motif>")` to `RELEASES`; a new motif is a new `GLYPHS[...]` entry (SVG fragments drawn around 0,0 in a ~200 px box, `currentColor` strokes) |
| `assets/img/release-X-Y-Z-hero.svg`, `-card.svg` | generated: `python3 scripts/gen-blog-art.py` (deterministic; `--force` to redraw) |
| `blog.html` | a card in `.blog-grid` **below the pinned card**, above the newest release card; a hidden `<article class="blog-post" data-post="release-X-Y-Z" tabindex="-1" aria-labelledby="h-release-X-Y-Z" hidden>` further down, in the same order as the cards |
| `index.html` | `<b data-latest-tag>vX.Y.Z</b>` — the front-page chip; `api/v1/site.json`'s `latestReleaseMentionedOnSite` is generated from it |
| `roadmap.html` | one `<div class="rm-rel is-latest">` block per release — move `is-latest` to the new block; the badge reads `Released, D Mon YYYY`, the sub-line is the headline's first clause |
| `blog.md`, `roadmap.md`, `api/v1/posts.json`, `api/v1/site.json`, `sitemap.xml`, `functions/md-map.js` | generated: `python3 scripts/gen-agent-discovery.py`; then `--check` reports no drift |

Do not edit the generated files by hand; do not touch `styles.css` (a change
there needs the `?v=` cache-buster bumped on three pages). CI regenerates the
discovery surface on every deploy and smoke-tests it; Cloudflare Pages
deploys on push to `main`.

## The card

```html
<a class="blog-card" href="#release-X-Y-Z">
  <div class="blog-card-media">
    <img width="640" height="560" src="assets/img/release-X-Y-Z-card.svg" alt="vX.Y.Z release art — <kicker, lower case>, drawn as an AetherSDR spectrum panel" />
  </div>
  <div class="blog-card-body">
    <span class="blog-meta">
      <time datetime="YYYY-MM-DD">Month D, YYYY</time>
      <span class="sep" aria-hidden="true">·</span>
      <span>N min read</span>
    </span>
    <h3>vX.Y.Z: <editorial title in lower case></h3>
    <p><three to four lines: the release in one breath></p>
    <span class="blog-more">Read post
      <svg …arrow…></svg>
    </span>
  </div>
</a>
```

Copy the arrow SVG and the `blog-back` links from the neighbouring post; the
markup is identical post to post.

## The article

Hero `<figure class="blog-hero">` with the 1200×630 SVG and the same alt
text; the meta line; `<h1 id="h-release-X-Y-Z">` with the title; then
`.blog-body`: a `<p class="lede">`, two to four `<h2>` sections, a closing
`<h2>Also in this release</h2>` if the CHANGELOG has more than the headline
sections, and the closing paragraph:

```
N merged changes from M contributors, plus AetherClaude and K Dependabot update(s)[ — including J first-time contributors this cycle]. Full release notes and the complete commit list: <a href="https://github.com/aethersdr/AetherSDR/releases/tag/vX.Y.Z">vX.Y.Z on GitHub</a>.
```

`M` is the human contributor count; the first-time clause appears only when
there are any. Reading time 4–6 minutes (about 900–1300 words).

## The voice — v26.9.3's opening, as the sample

> **v26.9.3: a Tools menu, and a digipeater that only arms when you say so**
>
> Eighty-six merged changes, and the operator-facing half of them starts at
> the very top of the window: the menu bar is now organised around what you
> reach for while operating, rather than around where things happened to get
> built.
>
> **Where things are**
>
> The top level is now **File · Settings · Profiles · Tools · View · Help**.
> The operating tools that had scattered themselves across File, Settings,
> View and Help — PSK Reporter, Memory, Waveforms, Radio Health, the modem
> and KiwiSDR setup, the guarded tuner operations — collect under **Tools**,
> which sits ahead of **View** because you reach for them more often than you
> reach for display settings.
>
> Nothing underneath changed: the same actions, the same handlers, the same
> shortcuts and lifecycle behaviour. What changed is where things are found,
> and that is worth a section of its own, because a menu bar is the map of an
> application and it is the one you read under time pressure — mid-contest,
> mid-QSO, with the band open and closing.
>
> […] One removal is worth naming. Settings carried a placeholder loop that
> reported "not yet implemented" for actions that in fact worked perfectly
> well. That is the mirror image of the failure we wrote about last release —
> not a control that lies about succeeding, but one that lies about failing.
> It is the cheaper of the two mistakes, and it still sends an operator off
> looking for a workaround to a problem they didn't have.

What the voice does: leads with the operator's experience, says *why* a
decision was made and what it costs, names what is deliberately not included,
connects to the previous release where the thread continues, and never
reads as a bullet list of PR titles. Numbers are spelled out in the lede and
given as digits in the closing line.

## Titles, kickers and motifs of record

| slug | date | title | kicker | motif |
|---|---|---|---|---|
| release-26-9-3 | 2026-09-13 | v26.9.3: a Tools menu, and a digipeater that only arms when you say so | The tools, first | toolbar |
| release-26-9-2 | 2026-09-06 | v26.9.2: four skimmers on one stream, and two more ways to receive | Four skimmers, one stream | skim |
| release-26-9-1 | 2026-08-29 | v26.9.1: a globe, a rotator, and buttons that tell the truth | The map goes round | azimuth |
| release-26-8-4 | 2026-08-22 | v26.8.4: capabilities the radio has to prove | Evidence, not assumption | attest |
| release-26-8-3 | 2026-08-16 | v26.8.3: a new shell, and the reason it's off by default | The workspace canvas | canvas |
| release-26-8-2 | 2026-08-09 | v26.8.2: two more radios, and what "experimental" has to mean | Two more radios | seam |
| release-26-8-1 | 2026-08-02 | v26.8.1: settings that survive, and a way in when they don't | Settings that survive | store |

The post's date is the release date, not the day it was written (the three
posts back-filled on 2026-09-01 carry August dates). Titles are
`vX.Y.Z: <lower-case editorial title>`, usually two clauses joined by a
comma or "and".

Kickers are two to five words; motifs are one word naming a glyph in
`GLYPHS`. A release whose headline feature has no glyph gets a new one drawn
in the same style — proposed through the gate with the title and kicker.

## The PR

Branch `blog/release-X-Y-Z` in a fresh worktree of `aetherweb`; commit
`Blog: vX.Y.Z — <editorial title>`; PR against `main` (or a direct push when
the maintainer says so — gate). The `aetherweb` repo has no CODEOWNERS
gate; the deploy workflow runs on push to `main` and the post is live within
minutes.
