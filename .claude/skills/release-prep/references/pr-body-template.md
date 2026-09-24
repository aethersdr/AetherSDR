# Prep PR body — the skeleton

Title: `release: prep vX.Y.Z — <short description>` (or `docs(release): prep vX.Y.Z`).
Both forms are in the record; `release: prep` is the more common.

The repo squash-merges with the body as the commit message, so this text
becomes permanent history on `main`. It states the current state of the
branch, not the churn that produced it. Keep every number in it computed from
the mapping, and re-check them before the merge — v26.8.1's body drifted
116 → 117 → 118 while `main` moved under it.

```markdown
## Summary

Six-file release prep for **vX.Y.Z**, metadata and docs only — no source, test or CI changes.

Cutoff: `origin/main` at `<sha>` (#NNNN, merged <date>). Anything that merges after it is folded in before the tag (see "Folded in after the cutoff" below if that happened).

**N merged changes** from **M human contributors**, AetherClaude (A) and Dependabot (K). First-time contributors: **@a**, **@b** — or "none this cycle" — confirmed by looking for a prior merged PR per login before the previous tag, not from `author_association`.

## Changelog

Built from `v<prev>..<sha>` mapped through the commit-to-PR API (`repos/…/commits/<sha>/pulls`), not from commit subjects: <N> PRs and <J> direct commits, every one cited in the section. The notes lead with <headline features>. Numbers in the section come from the PR bodies; where a body disagreed with itself (#NNNN: x vs y) the section uses <x> because <reason>.

<If the previous tag was off-main:> `v<prev>` is not an ancestor of `main`; its prep landed as the squashed duplicate `<sha>` (#NNNN) and is excluded from the range.

## Docs refresh, not just version strings

- **README** — <highlights lines changed (engine count, overlay list, mode list)>, <Supported Hardware → Other radio families paragraphs touched, and why>, <the Roadmap section lines that moved>.
- **ROADMAP** — In flight: <entries advanced with a vX.Y.Z mention; entries removed as shipped>. Queued: <entries deleted as shipped>. Open RFCs: <checked against `label:rfc`; joined / left>. Recently shipped: <J> new entries carry a `(vX.Y.Z)` marker<; month-rollover trim if any>.

## Judgment calls flagged for the maintainer

- <Each decision put through the decision gate and the answer, e.g. "AetherTX (#5819) is treated as delivering the "TX DSP chain visual rebuild" roadmap item, so that line leaves In flight in both README and ROADMAP.">
- <A radio family's README maturity label, if one changed, and who decided.>
- "None — nothing in this prep needed a call." if so.

<Only when the maintainer chose to include an unmerged PR:>
> ### ⚠️ This PR documents #NNNN as shipped, and #NNNN has not merged yet
> Per maintainer direction, **#NNNN (<title>)** lands before the tag, so it is written into the CHANGELOG, `ROADMAP.md` and `README.md` here, and the stated count (N) assumes it merges as one squash commit. **Merge #NNNN first.** **If #NNNN slips, edit before tagging:** drop <the bullet at CHANGELOG § …>, <the sentence at ROADMAP § …>, <the clause at README § …>, set **@login** back to <n> commits, and set the count back to <N−1>.

## Scope

| file | change |
|---|---|
| `CMakeLists.txt` | `project(AetherSDR VERSION X.Y.Z)` |
| `README.md` | Current version line, plus the prose refresh above |
| `AGENTS.md` | Current version line |
| `CHANGELOG.md` | new `[vX.Y.Z] — YYYY-MM-DD` section; `[Unreleased]` stays and stays empty; history below byte-identical |
| `packaging/linux/io.github.aethersdr.aethersdr.metainfo.xml` | `<release version="X.Y.Z" date="YYYY-MM-DD"/>` first in `<releases>` |
| `ROADMAP.md` | cycle heading → post-vX.Y.Z, plus the prose refresh above |
| <`extra/file.md`> | <why — e.g. "the README split moved build detail here (#5673 precedent)"> |

## Validation

Output of `python .claude/skills/release-prep/scripts/check_release_prep.py --mapping mapping.json`, with each line's result:

- All six version spots agree on X.Y.Z (grepped, not trusted).
- `appstreamcli validate` — <result> (or `xmllint --noout` / XML parse, and which).
- Every PR in `v<prev>..<sha>` is cited; the only other numbers cited are <#NNNN (issue), #NNNN (RFC), #NNNN (prior PR)>.
- Intro count, Contributors counts and the first-time line re-derived from the mapping — match.
- `[Unreleased]` empty; CHANGELOG history below `[v<prev>]` byte-identical to `origin/main`.
- ROADMAP `(vX)` markers newest-first, no gap; every local link in README and ROADMAP resolves; no heading removed from README/ROADMAP is still linked elsewhere; code fences balance; no duplicated table headers or doubled `---`.
- Outside `CHANGELOG.md`, the only `<prev>` lines removed are the four current-version lines.
- `git diff --check` clean; the diff touches no `src/`, `tests/`, `third_party/` or `.github/workflows/` path.
- <Configure, if run:> `cmake` configure reports `CMAKE_PROJECT_VERSION=X.Y.Z`. No build or test run — this is metadata; CI builds it. <Never claim a build or tests that did not run.>
- `main` CI at the cutoff: <green / which job is red and why; e.g. "hl2_state_restore_test fails on the unchanged cutoff source (#NNNN); this prep does not fix it and the release ships with it red">.

<If `main` moved during review:>
## Folded in after the cutoff

- #NNNN (`<sha>`, merged <date>) — added to § <section>; counts moved N−1 → N, **@login** n−1 → n.

After merge: tag the resulting **main** commit, not this branch.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
```
