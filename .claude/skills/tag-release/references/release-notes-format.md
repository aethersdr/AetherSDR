# Release notes format — tag message, release body, hotfix body, retired shapes

The wording of the footer and the full tag-message sample are recorded once,
verbatim, in `../../release-prep/references/changelog-style.md` § "The
release-notes footer, verbatim" and § "The tag message, verbatim shape".
This file says what is *derived* from what, so that
`scripts/release_notes.py` and a hand-written body agree, and lists the
shapes that are retired so nobody regenerates them.

## Three strings that are one string

| where | text |
|---|---|
| CHANGELOG `### <headline>` | `<first clause> · <second clause> · …` |
| tag message, first line | `AetherSDR vX.Y.Z — <first clause>` |
| release title | `AetherSDR vX.Y.Z — <first clause>` |

The first clause is the headline text before the first ` · `. v26.8.1's
release title ("Settings Store, TCI Rig Control & Metering Accuracy") is not
its tag's first line ("Hermes-Lite 2, SQLite Settings & Capability-Gated UI");
v26.8.2's and v26.7.2's differ the same way, and v26.7.4.1's differs by
case. The rule since v26.9.3: one string, three places.

## The tag message

```
AetherSDR vX.Y.Z — <first clause>
<blank>
<one paragraph, five to seven lines, wrapped at ~76 columns: what the release
is, operator-facing half first, structural half last — rewritten from the
CHANGELOG intro, not pasted>
<blank>
N merged changes from M human contributors, AetherClaude and Dependabot.
Cut by <name> <callsign>.
```

- `N` and `M` are the CHANGELOG intro's numbers. The Dependabot count is not
  repeated here (the intro carries it).
- `release_notes.py` seeds the paragraph from the intro with the count
  sentence removed, wrapped; read it and rewrite it if it is not one
  paragraph of the right length. The count line and the `Cut by` line are
  fixed.
- Samples of record: v26.9.3 (in `changelog-style.md`) and v26.9.4:

```
AetherSDR v26.9.4 — AetherRX and AetherTX as one window each, Neural Noise Reduction and global precipitation

The receive and transmit chains each become one window with a draggable stage
column and a profile library. WDSP moves to 2.10 and brings Neural Noise
Reduction as the seventh client-side NR method, worldwide precipitation joins
the PSK Reporter map, and the TGXL and PGXL get front-panel presentations. The
Hermes-Lite 2 transmits through WDSP's TXA modulator by default, its ALC only
reduces, and its S-meter and dBm reference read what the hardware delivers.
Underneath, aetherd gains credential-bound transmit grants.

97 merged changes from 13 human contributors, AetherClaude and Dependabot.
Cut by Jeremy KK7GWY.
```

## The release body — derivation

Take the CHANGELOG section for the version (from its `## [vX.Y.Z] — date`
heading up to the next `## [` heading). Then:

1. Drop the `## [vX.Y.Z] — YYYY-MM-DD` line.
2. Drop the `### <headline>` line — the first `###` heading, the one whose
   text is joined with ` · `.
3. Drop the `73, <name> <callsign> & <tool> (AI dev partner)` line that ends
   the Contributors block.
4. Strip leading and trailing blank lines. The body now starts with the
   intro paragraph (`N merged changes from …`) and ends with the Contributors
   paragraph (or the `Welcome to first-time contributors …` line).
5. Append a blank line, then the footer from `changelog-style.md` with the
   versions substituted:

```markdown
### Downloads

Tag-triggered CI publishes Linux AppImages (x86-64 and aarch64), macOS DMGs (Apple Silicon and Intel), and Windows installers and portable ZIPs as jobs finish. Assets may still be building when this release first appears. macOS signing and notarization, and release checksum and signature generation, run in their respective workflows.

**Full diff:** [vPREV...vX.Y.Z](https://github.com/aethersdr/AetherSDR/compare/vPREV...vX.Y.Z). See [verification instructions](https://github.com/aethersdr/AetherSDR/blob/vX.Y.Z/docs/VERIFYING-RELEASES.md).

73, <name> <callsign> & <tool> (AI dev partner)
```

The sign-off appears once, at the very end. `release_notes.py --check`
fetches the published release and diffs its title and body against this
derivation, so a hand edit shows in the report.

Everything between steps 1 and 3 is kept byte-for-byte: the topical `###`
sections, the bold lead sentences, the `(#NNNN)` citations, the Contributors
paragraph, the Dependabot sentence and the welcome line.

## The hotfix body

Structure from v26.7.4.1; footer current (v26.7.4.1's own footer was the
retired shape below and is not reproduced):

```markdown
> **Hotfix release.** This is [vBASE](…/releases/tag/vBASE) plus one fix and nothing else. If you are on vBASE and <who is affected>, update. If you are on vPREVFULL or earlier, this is your upgrade target — the full vBASE notes follow below.

## The fix

**<What is restored.>** (#issue, #issue — fixed in #PR)

<Why it broke, what it did to operators, why it could not wait for the next
release, who reported it. Several paragraphs are fine; it is the whole point
of the release.>

---

# Everything in vBASE

<the base release's body, from its intro paragraph through its Contributors
block — everything above its `### Downloads`>

### Downloads

<the fixed paragraph>

**Full diff:** [vBASE...vX.Y.Z.H](…/compare/vBASE...vX.Y.Z.H) (this hotfix) · [vPREVFULL...vX.Y.Z.H](…/compare/vPREVFULL...vX.Y.Z.H) (since the last full release). See [verification instructions](…/blob/vX.Y.Z.H/docs/VERIFYING-RELEASES.md).

73, <name> <callsign> & <tool> (AI dev partner)
```

`release_notes.py --hotfix-base vBASE --fix-file fix.md` assembles this: the
blockquote and `## The fix` from `fix.md` (its first line is the bold lead),
the base body fetched from the published base release, the two-link footer.
The hotfix tag message is the first line, the fix paragraph, and `Cut by`.
The CHANGELOG still carries a `## [vX.Y.Z.H]` section for the hotfix (the
prep writes it); the release body leads with the fix rather than repeating
the section.

## Retired shapes — recognise, do not regenerate

**Tag messages.**

- v26.8.1–v26.8.3: `AetherSDR vX — <Title Case Headline>`, then `N commits
  since vPREV.`, then four or five paragraphs. Too long, and the count is
  commits, not merged changes.
- v26.9.1, v26.9.2, v26.7.3 (Pat): one sentence, ending `Cut by Pat
  KI6BCJ.`; v26.7.3's also carries `25 commits since v26.7.2.` mid-sentence.
- v26.7.2: `AetherSDR v26.7.2` alone on the first line, no headline; a
  feature list and `See CHANGELOG.md.`

**Release bodies** (v26.7.3 through v26.9.1, and v26.7.4.1's base block):

- Opening: `N commits across M contributors since [vPREV](…). Full detail in
  [CHANGELOG.md](…/blob/vX/CHANGELOG.md#…).`
- A curated `### Headlines` block with bold topic lines and bullets,
  re-summarising the CHANGELOG instead of carrying it.
- `Big thanks to **@login** (N commits — …)` Contributors with the
  maintainer's entry as `(maintainer, N commits — …)`.
- Footer: `**Downloads**` in bold (not a `###`), a paragraph that names
  "the Stream Deck plugin" among the assets (plugins removed in #5600), a
  `**Full diff:**` line without the verification link, and a separate
  `**Verifying releases:** see [docs/VERIFYING-RELEASES.md](…). Signing key:
  [docs/RELEASE-SIGNING-KEY.pub.asc](…).` line.

None of these come back. A body that carries `### Headlines`, `Big thanks`,
`**Downloads**` in bold, `Stream Deck`, or `Signing key:` is the wrong shape.
