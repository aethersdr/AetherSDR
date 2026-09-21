---
name: release-prep
description: 'The AetherSDR release-prep pass — takes a CalVer version and drives the release from "main is where we want to cut" to "the prep PR is merged on main and ready to tag". Maps the range through the commit-to-PR API, writes the CHANGELOG section from PR bodies, refreshes README and ROADMAP, edits the six release files, validates, opens the prep PR, folds in what merges after the cutoff, and hands the merged squash commit to /tag-release for the tag, the release and the asset verification. Every judgment call goes to the maintainer through AskUserQuestion. Use when the user says "/release-prep 26.9.4", "/release-prep v26.9.4", "prep the release", "release prep", "write the changelog for 26.9.4", or otherwise wants a version prepped; the cut itself is "/tag-release".'
---

# Prep the release — map, write, validate, land

Work the version given in `$ARGUMENTS` (`26.9.4` or `v26.9.4`; if absent, ask
which). Six deliverables, in this order:

1. **A commit-to-PR mapping** of everything since the previous tag, saved.
2. **The six release files edited** — version spots, a new CHANGELOG section
   written from the PR bodies, and a README/ROADMAP content refresh.
3. **The validation list run and printed** — all of it, every time.
4. **The prep PR**, on a `release/vX.Y.Z` branch of the org repo, kept current
   with `main` until it merges.
5. **The merged squash commit handed to `/tag-release`** (step 7) — the tag,
   the release and the asset verification are that skill's deliverables.
6. **A markdown report to the operator** (step 9).

Where `gh` is unavailable — Claude Code Remote and web sessions have no `gh`
CLI — use the GitHub MCP tools (`mcp__github__*`) for the reads and writes. The
two scripts under `scripts/` shell out to `gh` and cannot run there; say so in
the report and do the mapping and the citation check by hand rather than
skipping them. Shell examples use Bash; on Windows use Git Bash, and `python`
where `python3` is written.

## Premise — the release is a claim about what shipped

A release is the one artifact users and packagers read instead of the commit
log. Its CHANGELOG section, its README claims and its ROADMAP entries are
statements of fact about a specific commit, and the tag pins which commit. Hold
this posture:

- **Every number is computed, never remembered.** The merged-change count,
  the contributor count, each author's count, the first-time list — all from
  the mapping in step 1. v26.8.1's count drifted 116 → 117 → 118 while `main`
  moved; v26.8.3's ROADMAP body said eleven entries when there were twelve.
- **The record is the PR bodies, mapped through the API.** In the v26.9.4
  range, 42 of 97 commit subjects carried no trailing `(#NNNN)` at all; others
  carry an *issue* number where a PR number would be, or carry the PR number
  ahead of a trailing `Principle N.` clause. Subject-suffix detection missed
  20 PRs in one cycle (#5464).
- **History stays byte-identical.** The only edit to `CHANGELOG.md` is the
  inserted section; the only edits to old version strings are the four
  current-version lines. "shipped v26.7.4" is a fact about July and stays.
- **The tag goes on `main`.** Five of the last ten tags (v26.9.2, v26.9.1,
  v26.8.4, v26.7.4.1, v26.7.3) were cut on the prep branch: binaries built from
  a commit absent from history, the next changelog range polluted by the
  squashed duplicate, and v26.7.4.1's prep never landing at all (#4519).
- **Principle VIII and XI govern the cut too.** A build or test the pass did
  not run is not claimed; the asset set is checked by listing it, not by
  trusting the workflow names; "signed" means the `.asc` files exist and
  postdate the binaries (v26.8.2 shipped unsigned for a week because the
  trigger could never fire and nobody looked).

## The decision gate — ask, never guess

**Every decision that has a real alternative goes to the maintainer through
the `AskUserQuestion` tool.** Not a prose list of options, not a question at
the end of the report, not a choice made quietly and mentioned afterwards.
The bar is: *would a different answer have changed the work?* If yes, ask.

### Always ask

- **The release date** when today differs from the planned cut, and **the
  version's month** when it differs from the cut date's month (v26.9.1 was cut
  on 2026-08-29 as a deliberate early September cut — that is a maintainer
  call, not a typo to fix).
- **Ready or draft** for the prep PR. #5673 opened as a draft for independent
  review; #5861 opened ready. Both are in the record.
- **Each ROADMAP "In flight" item the changelog suggests is finished.** Moving
  it to "Recently shipped" is a maintainer call — whether AetherTX closed the
  "TX DSP chain visual rebuild" item in v26.9.4 was put to the maintainer, not
  decided by the prep.
- **Any experimental → early → supported wording change for a radio family.**
  README "Supported Hardware" labels are the maintainer's, never the
  changelog's. #4872's reviewer struck the IC-9700 from the README outright.
- **Including a not-yet-merged PR as shipped.** If the maintainer wants it,
  the body carries the warning banner and the exact fallback edits
  (`references/pr-body-template.md`); v26.8.3 did this for #5016 and v26.8.1
  for #4719 (that one without a fallback plan, and it merged 85 seconds before
  the prep did).
- **Dropping or merging an entry that already sits in `[Unreleased]`.** Fold
  the block in whole; if an entry cannot be placed, ask. v26.8.1 claimed
  "everything folded in" and dropped #4687.
- **Any file beyond the six.** A README split like #5673's `docs/BUILDING.md`,
  a regenerated touchpoint manifest like #4872's — each is a decision with a
  reason that goes in the scope table.
- **An ambiguous contributor identity** — a login the API cannot resolve for a
  direct commit, or a display name that matches no login.
- **The month-rollover text** for "Recently shipped" (the intro names the
  current CalVer cycle and the list is trimmed to it at the rollover).
- **Whether to proceed when `main`'s full suite is red at the cutoff**, and
  how to word it — a prep does not fix a red test, but v26.8.3 shipped with
  `hl2_state_restore_test` red and said so under its own heading.
- **A stalled review.** `*.md` and `CMakeLists.txt` are owned by
  `@aethersdr/infrastructure`, the author cannot approve their own PR, and
  `enforce_admins` is off: admin-merge, wait, or route to a named code owner
  is the maintainer's route, not yours.
- **A tag day that slipped.** The CHANGELOG heading, the metainfo entry and
  the tag share one date; if the merge lands on a different day, both files
  change *on `main`* before the tag — a second PR — and that is a call.

### Do not ask — do it, and say so in one line

Wording of bullets; section order that follows the pattern in
`references/changelog-style.md`; excluding the previous prep's squashed
duplicate from the range when the previous tag is off-main; running the
validation; committing and pushing (standing authorization); folding in a PR
that merged after the cutoff; re-running the mapping before the merge.
Applying the gate to trivia turns a release into an interrogation.

### When the tool is not there

`AskUserQuestion` is unavailable in headless, cron and background runs, and an
"Always ask" item reached there leaves no legal move: guessing is forbidden and
stalling strands a dirty worktree. **Stop before the push.** Leave the branch
untouched, post nothing to GitHub, never tag, and deliver the open decisions
as the report — each with the options, the trade-offs and your recommendation
— so the next interactive run starts from a decision list. An unmade decision
handed back is a finished pass; an unmade decision guessed at is not.

### How to ask

Do **everything that does not depend on the answer first** — the mapping, the
version spots, the citation-complete draft — then batch the open decisions
into as few `AskUserQuestion` calls as possible (up to four questions per
call). Recommended option first, labelled `(Recommended)`. Each option says
what it means and what it costs. Then implement the answer and carry on.

## 0. Preflight — version, previous tag, cutoff, worktree, CI

- **Parse the version.** CalVer `YY.M.patch[.hotfix]`, with or without the
  leading `v`; the tag is `vX.Y.Z`, the files carry `X.Y.Z`. A nonzero fourth
  component is a hotfix: `packaging/windows/get-store-build-plan.ps1` marks it
  `storeEligible = false`, so the Windows job still attaches the installer and
  portable ZIP but produces **no `.msixupload`** and stages nothing for the
  Store (`docs/WINDOWS-STORE-MSIX.md` § "Version discipline"). `/tag-release`
  expects one asset fewer; say so in the PR body.
- **Refuse a version that already exists.** `git ls-remote --tags origin
  refs/tags/vX.Y.Z` must print nothing and `gh release view vX.Y.Z` must fail.
  A published tag is never moved (v26.9.1's maintainer decision, #5325).
- **Find the previous tag and check it is on `main`.**

  ```sh
  git fetch origin --tags
  PREV=$(git tag --sort=-v:refname | grep '^v' | head -1)
  git merge-base --is-ancestor "$PREV" origin/main && echo on-main || echo OFF-MAIN
  ```

  When it is off-main, its prep commit reached `main` as a squashed duplicate
  that sits inside `merge-base..origin/main`; `scripts/release_contents.py`
  starts the range at the merge base, flags the duplicate in its `PREP?`
  column, and `--exclude <PR>` drops it. #5673 did exactly this for #5464.
- **Record the cutoff.** `git rev-parse origin/main` right now; it goes in the
  PR body as the SHA the changelog was computed against, and anything that
  merges after it is folded in before the tag (step 6).
- **Fresh worktree on `release/vX.Y.Z`, on the org repo.** Never work in the
  invoking checkout; someone else may be working there and a checkout
  retargets them mid-task. Never `git stash` in this repo — the stash stack is
  shared across every worktree.

  ```sh
  repo_root="$(git rev-parse --show-toplevel)"
  git -C "$repo_root" fetch origin
  git -C "$repo_root" worktree add "$repo_root/.worktrees/release-vX.Y.Z" -b release/vX.Y.Z origin/main
  ```

  The branch lives on `origin` (the org repo), not a fork: `CODEOWNERS` review
  and the team review request need it there, and every prep in the record has
  been on `aethersdr/AetherSDR`.
- **Confirm you can sign.** `main` has `required_signatures: true`. Commit
  with the checkout's configured identity — never `-c user.email=` — and check
  `git config commit.gpgsign` / `git config gpg.format` before the first
  commit. A mismatched committer email produces `verified=false,
  reason=bad_email` and blocks the merge with everything else green.
- **Know who you are acting as.** `gh api user --jq .login`. The prep PR's
  `*.md` and `CMakeLists.txt` paths are `@aethersdr/infrastructure`-owned, so
  the approval has to come from a code owner who is not the author.
- **Read `main`'s CI at the cutoff.**

  ```sh
  gh run list --branch main --limit 12 --json workflowName,event,conclusion,headSha,createdAt,url
  gh run list --workflow sanitizers.yml --limit 3 --json conclusion,createdAt,url
  ```

  Read `conclusion`, not `status`. A red required job on the cutoff commit is
  a stop-and-ask. A red test in the weekly sanitizer lane or a known failing
  test on `main` is not fixed by a prep, but it is named in the PR body under
  its own heading (v26.8.3's `hl2_state_restore_test`, v26.8.4's
  `icom_session_test`) rather than shipped silently — and whether to proceed
  is a decision.

## 1. Release contents — the commit-to-PR API, not commit subjects

```sh
python3 .claude/skills/release-prep/scripts/release_contents.py \
  --from "$PREV" --to "$CUTOFF" --bodies --json mapping.json
```

For every commit in the range it calls
`repos/aethersdr/AetherSDR/commits/<sha>/pulls` and records the merged PR; a
commit with no PR is recorded by short SHA with the login GitHub resolves for
its author. It prints the mapping as TSV, then per-author units, then a
summary whose last line is the intro sentence, computed. Keep `mapping.json`
— it feeds the counts, the section, the validation and the fold-in diff.

- **Read the `PREP?` column.** A commit whose title reads like a release prep
  is either the previous prep's squashed duplicate (drop it with `--exclude`
  when the previous tag is off-main) or a hotfix prep that genuinely belongs.
- **Read every PR body.** `--bodies` stores them in the JSON; the changelog is
  written *from* them, per `AGENTS.md` § "`CHANGELOG.md` is a release-prep
  file". Titles are not enough — the numbers, the deliberate exclusions and
  the "not in this release" caveats live in the bodies.
- **Contributors.** One unit per PR (the repo squash-merges) or per direct
  commit, counted by **API login**. Bots are `type == Bot`: Dependabot becomes
  the sentence "Dependabot contributed N dependency updates"; `aethersdr-agent`
  is listed as **@aethersdr-agent** with "AetherClaude orchestrator". Handles
  come from the API, never from display names or callsigns — v26.8.3 resolved
  the display name "Jonathan" to a stranger's handle and credited @nigelfenton's
  callsign as a second person, and both are still on `main`.
- **First-time contributors** are the humans with no merged PR before the
  previous tag's timestamp (`gh pr list --author X --state merged --search
  "merged:<TS"`), which the script does per login. Never read
  `author_association`, and never match by display name — `--author=Paul`
  found ten prior commits that belonged to a different person (#5464 review).

## 2. The six files

| file | change |
|---|---|
| `CMakeLists.txt` | `project(AetherSDR VERSION X.Y.Z …)` — the only one that reaches the binary |
| `README.md` | the `**Current version: X.Y.Z**` line |
| `AGENTS.md` | `Current version: **X.Y.Z**.` |
| `CHANGELOG.md` | new `## [vX.Y.Z] — YYYY-MM-DD` section directly under `## [Unreleased]`, which stays and stays empty; everything below the previous section byte-identical |
| `packaging/linux/io.github.aethersdr.aethersdr.metainfo.xml` | `<release version="X.Y.Z" date="YYYY-MM-DD"/>` first in `<releases>`, list date-descending — AppStream and Flathub read this, not the tag |
| `ROADMAP.md` | `## Current cycle: post-vX.Y.Z` |

`AGENTS.md` § "Version and release files" is the spec and lists five; the
ROADMAP heading is the sixth edit. v26.7.3 changed three, v26.7.4.1 four,
v26.9.1 three and needed #5325 to repair it — check every row, not the first
two.

**One date.** The CHANGELOG heading, the metainfo entry and the tag carry the
same day. If the tag slips, both files change before tagging (decision gate).

**The `[Unreleased]` block.** Usually empty, because feature PRs do not write
to the file. If it carries entries, fold every one into the new section;
`check_release_prep.py` fails if a PR cited there is missing afterwards.

## 3. The CHANGELOG section — house style

`references/changelog-style.md` has the full anatomy, a v26.9.4 excerpt, the
Contributors grammar, the tag-message shape and the release-notes footer
verbatim. The rules that reviews have actually enforced:

- `### <headline>` — the release in one line, clauses joined with ` · `,
  operator-facing half first, structural half last. **The first clause becomes
  the GitHub release title** and the first line of the tag message.
- Intro paragraph: `N merged changes from M human contributors, AetherClaude
  and K Dependabot updates within that total.` then two or three sentences
  saying what the release is. Numbers from `mapping.json`.
- Topical `###` sections in review-weight order: what an operator sees first,
  then per-radio-family sections, then persistence/audio/devices, then
  structural (backend seam, aetherd), then Automation, then "Project and
  packaging" last. Headline features get a bold lead sentence with the PR
  number and a paragraph; everything else one bullet per change or per theme
  with several `(#NNNN)` citations. **Every PR in the range is cited at least
  once**; a direct commit is cited by short SHA. Dependabot bumps get one line.
- Numbers in the section come from the PR body. Where the body disagrees with
  itself, pick one and say which in the PR body (v26.8.1 shipped 25 in the
  headline and 24 in the bullet of the same entry, and both are still there).
- State what ships. Do not narrate what was reverted or removed.
- `### Contributors` — one paragraph, descending by count, `**@login** (N
  commits — what they did)`, the maintainer's entry carrying `maintainer;`,
  bots after humans, then the Dependabot sentence, then `Counts cover primary
  commit authors; co-author credit remains in the commit history.` Then
  `Welcome to first-time contributors **@a**, **@b**!` only when there are
  any. Then the sign-off `73, <name> <callsign> & <tool> (AI dev partner)` in
  the form of whoever is cutting.

## 4. The doc refresh — the half that three preps skipped

README and ROADMAP content is refreshed at every prep, not only the heading.
v26.8.4, v26.9.1 and v26.9.2 reduced this to a version bump, and #5673 had to
backfill 21 "Recently shipped" entries and four stale README facts (the backend
count, ANAN-G2 and Web-888 absent, an aetherd sentence two stages old). Every
time:

- **ROADMAP "In flight"** — advance every entry whose PRs landed (aetherd
  stages, HL2, Icom, ANAN-G2, RTL-SDR, workspace canvas, AppSettings) with a
  `vX.Y.Z` mention of what landed and what remains; remove an item that
  shipped outright — through the gate, since "finished" is the maintainer's
  word.
- **ROADMAP "Queued (next cycle)"** — delete anything that shipped.
- **ROADMAP "Open RFCs"** — diff against `gh issue list --label rfc --state
  open --json number,title,labels`: an issue carrying `RFC Approved` leaves
  the list (it moves up into the cycle), a closed one leaves, a new open one
  joins under the right heading.
- **ROADMAP "Recently shipped"** — prepend one entry per headline feature with
  a `(vX.Y.Z)` marker; markers run newest-first with no gap; the intro sentence
  names the current CalVer cycle (`v26.9.x`) and the list is trimmed to that
  cycle at the month rollover (gate). "Highlights from the last 30 days" once
  spanned 73 because no prep had pruned it.
- **README** — the Highlights list (engine counts, overlay lists, mode lists),
  "Supported Hardware → Other radio families" (one paragraph per non-Flex
  family with its maturity label — the label is the maintainer's), and the
  README "Roadmap" section, which mirrors ROADMAP "In flight" in five lines.
  Check the claims against the tree, not against the previous README:
  - the backend count against the family directories in `src/core/backends/`
    and the row count of the backend table in `AGENTS.md` § "In-flight: aetherd
    engine/UI decoupling";
  - the NR engine list against the method names `src/core/AetherRxProfiles.cpp`
    maps (`NR2`, `RN2`, `NR4`, `NNR`, `DFNR`, `BNR`, `MNR` at the time of
    writing);
  - overlay and mode lists against the PRs that added or removed one this
    cycle.
- **Cross-file consistency** — README, ROADMAP and the new CHANGELOG section
  must not contradict each other. #4872 was blocked for "transmit unverified"
  in the README against "Verified on an IC-9700" in the same PR's CHANGELOG;
  #5026 for "transmit confirmation beyond the 705" as remaining work directly
  under a paragraph reporting WSPR on the air from an IC-7300MK2.
  `check_release_prep.py` prints each family's maturity words per file at the
  end of its run; read them against each other before validating.
- **Historical mentions stay.** `docs/WINDOWS-STORE-MSIX.md` and
  `tests/windows_store_policy_test.ps1` use old versions as worked examples;
  `.github/workflows/cache-cleanup.yml`, `docs/architecture/audio-pipeline.md`,
  `src/core/CwSidetoneBackendPolicy.h` and the tests beside it say what a
  version did. Never find-and-replace. The check: outside `CHANGELOG.md`, the
  lines the diff removes that contain the old version are exactly the four
  current-version lines and nothing else.
- **Re-read a file after applying a GitHub suggestion.** A suggestion on a
  block of a different line count left a duplicated fragment in the README
  that shipped in v26.8.2 and was only removed by the next prep.
- **Anchors.** A heading removed from README or ROADMAP may be linked from
  another doc; #5673 removed `## Windows 11` and left
  `docs/first-contribution-cheatsheet.md` pointing at it. The validator greps
  for that; fix the referrer only if the maintainer wants a seventh file, else
  keep the heading.

## 5. Validation — run all of it, print all of it

```sh
python3 .claude/skills/release-prep/scripts/check_release_prep.py --mapping mapping.json
```

One command, non-zero on any `FAIL`, every check printed with its result:

- All six version spots agree — grepped, not trusted.
- `appstreamcli validate` on the metainfo when available, else `xmllint
  --noout`, else a Python XML parse; the release list date-descending.
- `[Unreleased]` first and empty; the new section directly under it; the
  CHANGELOG diff is exactly the inserted section (history byte-identical);
  every PR cited in the base's `[Unreleased]` survives the fold-in.
- Every PR in the range cited, every direct commit cited by SHA, every foreign
  number listed with what it is (issue, RFC, prior PR), the intro count, the
  Contributors counts and the first-time line re-derived from the mapping
  (`release_contents.py --check`, run for you when `--mapping` is given).
- ROADMAP `(vX)` markers newest-first, leading with the new version, no gap;
  the "Recently shipped" intro names the current cycle; every local link in
  README and ROADMAP resolves; no heading removed from README/ROADMAP is still
  linked elsewhere; code fences balance; no duplicated table headers or doubled
  `---` (both caught in #5673 review).
- Old-version mentions removed outside CHANGELOG are only the four
  current-version lines.
- `git diff --check`; the diff touches no `src/`, `tests/`, `third_party/` or
  `.github/workflows/` path; all six files present; extra files listed.
- The per-family maturity wording across the three files, printed for you to
  read.

Paste the output into the PR body's Validation section. **A configure is
optional and cheap where a toolchain is present** — `cmake -S . -B
<scratch>/cfg` then `grep CMAKE_PROJECT_VERSION <scratch>/cfg/CMakeCache.txt`
— and three preps have done it. A full build is not required for metadata,
and the body says "no build or test run; CI builds it" rather than implying
one. Never claim a build, test or hardware result the pass did not produce.

## 6. The PR — and watch `main` until it merges

- **Commit** as `release: prep vX.Y.Z — <what the notes lead with>. Principle
  VIII.` (or `docs(release): prep vX.Y.Z`), signed, one commit for the six
  files; a README split or any seventh file is its own commit with its own
  subject (#5673 precedent).
- **Body** per `references/pr-body-template.md`: Summary (cutoff SHA, counts,
  first-timers), Changelog (how the range was mapped, what the notes lead
  with, any off-main exclusion), Docs refresh, **Judgment calls flagged for
  the maintainer**, Scope table (the six files plus any extra with its
  reason), Validation (the step-5 output), `After merge: tag the resulting
  **main** commit, not this branch.`, then the Claude Code attribution line.
- **Ready or draft** is a gate question. Request review from
  `@aethersdr/infrastructure`; the author cannot approve. Assign yourself
  (`AGENTS.md` § "Issue / PR Claim Protocol").
- **While the PR is open, watch `main`.** Every merge after the cutoff must
  be folded in before the tag: v26.7.2 missed #4211 and nobody caught it;
  v26.8.1's reviewer caught #4718; #5673 folded #5595 in mid-review with a
  merge from `main` plus a fold-in commit. Either shape is fine because the PR
  squash-merges — merge `origin/main` into the branch, or rebase and push with
  `--force-with-lease`. Then:

  ```sh
  git fetch origin
  python3 .claude/skills/release-prep/scripts/release_contents.py --from "$PREV" --to origin/main --bodies --json mapping.json
  ```

  Diff the new mapping against the old, add the new PRs to the right sections,
  bump the intro count and the affected author counts, re-run step 5, update
  the PR body's numbers and its "Folded in after the cutoff" list, push.
- **Immediately before the merge, recompute.** `git rev-list --count
  "$PREV"..origin/main` against the intro count, and the mapping again. The
  body and the section must agree with each other and with `main`.
- **Never write up a PR as shipped that has not merged** unless the
  maintainer chose to through the gate; then the body carries the banner and
  the exact edits to make if it slips, and the merge order is stated.
- **Merging.** A code-owner approval and green required checks let auto-merge
  fire: `gh pr merge <PR> --auto --squash`. A `BLOCKED` state with green
  checks is an unresolved review thread or the missing code-owner approval
  (`main` has `required_conversation_resolution`); a stalled review is a gate
  question, never an admin-merge on your own authority. Do not tag while the
  PR is open.

## 7. Cut — hand off to `/tag-release`

After the prep PR merges, the cut is `/tag-release vX.Y.Z`
(`.claude/skills/tag-release/SKILL.md`). It finds the squash-merge commit of
this PR on `origin/main`, re-checks the six files and CI at that SHA, tags
it with a signed annotated tag, creates the release before CI attaches
anything, watches the three build workflows and the signing runs, verifies
the asset set on the files, reads the Store step, drafts the website post and
writes its own report. The tag, the release and the asset verification are
that skill's deliverables, not this one's; the tag-message shape and the
release-notes footer stay recorded in `references/changelog-style.md`, which
`/tag-release` reads.

Do not tag from here. The one thing to carry across is the PR number:

```sh
gh pr view <PR> --json mergeCommit,mergedAt --jq '"\(.mergeCommit.oid) \(.mergedAt)"'
```

If the merge landed on a day other than the one in the CHANGELOG heading and
the metainfo entry, that is `/tag-release`'s first gate question and it
comes back here as a second prep PR; do not pre-empt it.

## 8. Post-tag verification — done by `/tag-release`

Step 4 of `/tag-release` (`scripts/check_release_assets.py`) verifies the
fifteen-asset set (fourteen for a hotfix), the `.asc` timing, the
`.msixupload` version, `SHA256SUMS.txt` coverage, the downloaded signatures
and the Store step, and its report carries the result. Nothing here
duplicates it.

What remains for this skill after the merge: clean up. `git worktree remove`
the prep worktree, `git worktree prune`, and delete the local
`release/vX.Y.Z` branch (the remote one auto-deletes on merge).

## 9. Report (markdown, to the operator)

```markdown
## vX.Y.Z — prepped

**Prep PR:** #<PR> <url> — merged as `<sha>` on `main` at <time> / open, awaiting <what>.
**Next:** `/tag-release vX.Y.Z` tags `<sha>`; nothing is tagged or published by this pass.

### Contents
N merged changes (P PRs, J direct commits) from M human contributors, AetherClaude (A), Dependabot (K); first-time: @a, @b / none. Range `v<prev>..<cutoff>`; folded in after the cutoff: #NNNN, #NNNN / none. Previous tag on main: yes / no (duplicate #NNNN excluded).

### Decided by you
Each decision put through AskUserQuestion and the answer. "None." if so.

### Validation
The step-5 output, or the lines that changed since the PR body's copy. Configure run: yes (`CMAKE_PROJECT_VERSION=X.Y.Z`) / no. Build and tests: not run — metadata only.

### Date
CHANGELOG heading and metainfo entry say YYYY-MM-DD; the merge landed on YYYY-MM-DD. Same day / differs — `/tag-release` will ask before tagging.

### Cleanup
Worktree removed, branch deleted / what remains and why.
```

State current state, not the churn.

## Never

- Touch `CHANGELOG.md` except to insert the new section; never edit history
  below it.
- Blanket-replace the previous version string; never touch a historical
  mention.
- Tag anything from this pass — the tag is `/tag-release`'s, on the merged
  squash commit, never on the prep branch.
- Cite a PR the range does not contain, leave one uncited, or invent a
  count — every number in the intro, the Contributors paragraph and the PR
  body is computed from the mapping.
- Take a contributor handle from a display name or a callsign.
- Claim a build, test or hardware result the pass did not produce.
- Push to `main` directly or edit `.github/workflows/`.
- Decide anything in the "Always ask" list without asking; admin-merge a
  stalled review on your own authority.
- Work, build or `git stash` in the invoking checkout.
- Call the pass finished before the prep PR is merged and the squash commit
  is named in the report for `/tag-release`.
