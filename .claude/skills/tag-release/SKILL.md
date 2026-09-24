---
name: tag-release
description: 'The AetherSDR cut pass — takes a CalVer version whose prep PR has merged on main and drives it from "the prep is on main" to "the release is published, every asset is signed and verified, the Store draft is staged, the website post is up and the report is written". Finds the squash-merge commit, checks the six release files at that SHA, tags it with a signed annotated tag, creates the release before CI attaches anything, watches the three build workflows and the signing runs, verifies the fifteen-asset set on the files, reads the Store staging step, drafts the website post, and reports the first 48 hours. Every judgment call goes to the maintainer through AskUserQuestion. Use when the user says "/tag-release 26.9.5", "/tag-release v26.9.5", "tag the release", "cut the tag", "publish v26.9.5", "the prep merged, tag it", or otherwise wants a prepped version tagged and published. /release-prep ends where this begins.'
---

# Tag the release — tag, publish, watch, verify, announce

Work the version given in `$ARGUMENTS` (`26.9.5` or `v26.9.5`; if absent, ask
which). Nine deliverables, in this order:

1. **The prep found on `main`** — the squash-merge SHA of the prep PR, with the
   six release files and CI read at that SHA, not in a checkout.
2. **A signed annotated tag on that SHA**, verified through the API.
3. **The GitHub release**, created in the same minute as the tag push, with
   the title that equals the tag's first line and the derived notes.
4. **The three build workflows and the signing runs watched to conclusion** —
   re-run or dispatched by hand where the record says they stall.
5. **The asset set verified on the files** — fifteen assets (fourteen for a
   hotfix), every `.asc` postdating what it signs, `SHA256SUMS.txt` and one
   artifact signature actually checked with `gpg`.
6. **The Microsoft Store step read** — draft staged, or the reason it failed
   and the recovery, stated.
7. **The website post drafted** in a fresh worktree of `aethersdr/aetherweb`,
   opened as a PR there.
8. **The announcement decision and the first-48-hours sweep** — issues filed
   against the version, hotfix material flagged.
9. **A markdown report to the operator** (step 8).

Where `gh` is unavailable — Claude Code Remote and web sessions have no `gh`
CLI — use the GitHub MCP tools (`mcp__github__*`) for the reads and writes. The
two scripts under `scripts/` shell out to `gh` and cannot run there; say so in
the report and do the derivation and the asset checks by hand rather than
skipping them. Shell examples use Bash; on Windows use Git Bash, and `python`
where `python3` is written. `gpg` on Windows means Gpg4win; where it is
absent, the signature checks are reported as not run, never as passed.

`/release-prep` produces the prep PR and stops when it merges. This skill
starts there. The tag-message shape and the release-body footer are recorded
verbatim in `../release-prep/references/changelog-style.md`;
`references/release-notes-format.md` here says which lines are dropped, what
the hotfix body looks like, and which older shapes are retired.

## Premise — the tag is a claim about a commit, the release is a claim about files

A tag pins one commit; every binary, tarball and checksum on the release is a
statement about that commit and nothing else. Eleven tags of record say where
that claim goes wrong:

- **The tag goes on `main`, on the prep's squash-merge commit.** Five of the
  last eleven (v26.9.2, v26.9.1, v26.8.4, v26.7.4.1, v26.7.3) were cut on the
  prep branch: the binaries were built from commits absent from history, the
  next prep had to exclude the squashed duplicate by hand, and v26.7.4.1's
  prep only reached `main` through a second PR (#4519).
- **A published tag is never moved.** v26.7.4 was pushed three times on
  2026-07-26 while the AppImage job was red under it, and its release still
  shows `published_at` before `created_at`. v26.9.1's maintainer chose to leave
  a tag whose tree missed three of the six files rather than move it (#5325).
  A bad tag means a new version.
- **The release is created before CI attaches anything.** When it is not, the
  first `softprops/action-gh-release` upload creates a bare release authored by
  `github-actions[bot]` and the title and body are pasted in afterwards
  (v26.8.2, v26.8.3). Three releases (v26.8.1, v26.8.2, v26.7.2) still carry a
  title that is not their tag's first line.
- **Assets are verified on the files, not on run colours.** `sign-release.yml`
  could not fire before #5029 — v26.7.1, v26.7.2 and v26.7.4.1 were never
  signed, v26.7.3 through v26.8.1 were signed by hand-dispatch, and v26.8.2
  sat unsigned for a week. A `.asc` that exists is not a signature that
  verifies; a DMG that exists is not a notarized DMG.
- **The Store step colours the Windows run.** Four of the last eight Windows
  runs went red at "Stage Microsoft Store submission (draft)" after all three
  Windows assets had attached (v26.8.1, v26.8.4, v26.9.1, v26.9.4). Red
  Windows with the assets present is a Store problem; read the step.
- **The website post is part of the release.** Three of the last five posts
  were back-filled days or weeks late, and the front page's latest-release
  chip still said `v26.7.1` on 2026-09-20.
- **Principles VIII and XI govern the cut.** Nothing is claimed that the pass
  did not verify: every check the report lists names the command that ran.

## The decision gate — ask, never guess

**Every decision that has a real alternative goes to the maintainer through
the `AskUserQuestion` tool.** Not a prose list of options, not a question at
the end of the report, not a choice made quietly and mentioned afterwards.
The bar is: *would a different answer have changed the work?* If yes, ask.

### Always ask

- **A tag day that differs from the CHANGELOG heading and metainfo date.** The
  two files change on `main` first, as a second PR, and the tag waits; or the
  maintainer accepts the mismatch knowingly. Either way it is their call.
- **A red required check on the SHA.** The tag builds this exact tree.
- **Partner Center holding an in-progress submission** the maintainer has not
  cleared, or one they are unsure about. The staging step deletes and
  recreates only API-created submissions; a portal-created one fails it, and a
  failed staging cannot be re-run (step 5).
- **The hotfix branch strategy** for a nonzero fourth component — recommended:
  the prep lands on `main` through `/release-prep`, tag the squash commit.
  v26.7.4.1 was built on a `release/v26.7.4.1` branch from the fix commit and
  needed #4519 to land its prep.
- **`--latest=false`** for a hotfix on an older line than the current release.
  Latest is otherwise automatic by date and version.
- **A platform job that failed twice.** Re-run again, upload the artifact by
  hand (as the maintainer did for v26.7.2's DMGs), or ship without it and say
  so on the release — each with its cost.
- **Dispatching the signing workflow when a run exists but is stuck** (as
  opposed to no run at all, which is a do-not-ask below).
- **The website post's title, kicker and motif**, each with a proposal, and
  **PR versus direct push** to `aetherweb`'s `main`.
- **Whether and where to announce**, with a one-paragraph draft.
- **Whether a first-48-hours report is hotfix material.** A crash-at-open or
  no-audio class report against the new version is flagged; starting the
  hotfix cycle is the maintainer's decision.

### Do not ask — do it, and say so in one line

The tag message paragraph (written from the CHANGELOG intro, shape fixed);
creating the release; running the verifications; re-running a failed platform
job once (`gh run rerun <id> --failed`); dispatching the signing workflow when
no run has succeeded ten minutes after the four signables are present;
opening the website PR once the title is chosen; committing and pushing the
website branch (standing authorization). Applying the gate to trivia turns a
release into an interrogation.

### When the tool is not there

`AskUserQuestion` is unavailable in headless, cron and background runs, and an
"Always ask" item reached there leaves no legal move: guessing is forbidden and
stalling strands a half-cut release. **Stop before the tag push.** Nothing in
step 0 changes anything; if an "Always ask" item is open at the end of it,
push no tag, create no release, post nothing to `aetherweb`, and deliver the
open decisions as the report — each with the options, the trade-offs and your
recommendation — so the next interactive run starts from a decision list. An
item that opens *after* the tag is pushed (a twice-failed platform job, a
Store failure needing a Partner Center answer) is reported with the release
left exactly as CI left it; a published tag is never rolled back. An unmade
decision handed back is a finished pass; an unmade decision guessed at is not.

### How to ask

Do **everything that does not depend on the answer first** — the preflight
reads, the derived notes, the tag message — then batch the open decisions
into as few `AskUserQuestion` calls as possible (up to four questions per
call). Recommended option first, labelled `(Recommended)`. Each option says
what it means and what it costs. Then implement the answer and carry on.

## 0. Preflight — version, prep, six files, date, CI, signer, Store, worktree

Nothing in this step changes anything. All of it runs before any "Always
ask" question, so the questions are batched once.

- **Parse the version.** CalVer `YY.M.patch[.hotfix]`, with or without the
  leading `v`; the tag is `vX.Y.Z`, the files carry `X.Y.Z`. A nonzero fourth
  component is a **hotfix**: `packaging/windows/get-store-build-plan.ps1`
  returns `storeEligible = false`, so the Windows job attaches the installer
  and the portable ZIP but builds **no `.msixupload`** and stages nothing —
  fourteen assets, not fifteen, the Store skipped by design, the release body
  in the hotfix shape, and the notes covering one fix.
- **Refuse a version that already exists.** Both must be empty:

  ```sh
  git ls-remote --tags origin "refs/tags/vX.Y.Z"
  gh release view vX.Y.Z
  ```

  If either finds something, stop: a published tag is never moved or
  deleted, and a wrong one means the next version, not this one.
- **Find the prep.** The tag target is the squash-merge commit of the prep PR
  on `origin/main`, never a branch head:

  ```sh
  git fetch origin --tags
  PR=$(gh pr list --state merged --search "release: prep vX.Y.Z in:title" \
       --json number,mergeCommit,mergedAt --jq '.[0].number')
  SHA=$(gh pr view "$PR" --json mergeCommit --jq .mergeCommit.oid)
  git merge-base --is-ancestor "$SHA" origin/main && echo on-main || echo STOP
  git show -s --format='%H %cs %s' "$SHA"
  ```

  No merged prep PR means the prep has not landed — `/release-prep` first.
  The ancestor assertion failing means the SHA is wrong; stop.
- **Check the six files at that SHA**, not in a checkout that may have moved:

  ```sh
  git show "$SHA:CMakeLists.txt" | grep 'project(AetherSDR VERSION'
  git show "$SHA:README.md"      | grep 'Current version'
  git show "$SHA:AGENTS.md"      | grep 'Current version'
  git show "$SHA:ROADMAP.md"     | grep '^## Current cycle'
  git show "$SHA:CHANGELOG.md"   | grep -m1 '^## \[v'
  git show "$SHA:packaging/linux/io.github.aethersdr.aethersdr.metainfo.xml" | grep -m1 '<release '
  ```

  All six say `X.Y.Z`. The Windows workflow compares the tag version with
  `project(AetherSDR VERSION …)` and skips the MSIX and the Store when they
  differ — the installer and ZIP still attach, so the mismatch costs the Store
  package silently (a `::warning` annotation on the run, nothing else).
  v26.9.1's tagged tree missed three of the six and needed #5325 to repair
  `main`; the tag itself stayed wrong.
- **The date.** The CHANGELOG heading, the metainfo `<release date>` and the
  tag share one day. `git show -s --format=%cs "$SHA"` is the day the merge
  landed; today is the day the tag will carry. If the two files say another
  day, both change on `main` first (a second PR, through `/release-prep`'s
  rules) — an "Always ask" item.
- **CI on the SHA.** The four required contexts green on this commit:

  ```sh
  gh run list --branch main --limit 30 --json headSha,workflowName,conclusion,url \
    --jq '.[] | select(.headSha == "'"$SHA"'") | "\(.conclusion)\t\(.workflowName)\t\(.url)"'
  ```

  Read `conclusion`, not `status`. A red required job is an "Always ask";
  the tag builds this exact tree. A run still in progress is waited for.
- **Who is cutting, and how they sign.** `gh api user --jq .login`, then
  `git config user.name`, `git config user.signingkey`, `git config
  gpg.format`. Jeremy's tags are GPG-signed; Pat's are SSH-signed. `git tag
  -v` verifies a GPG tag locally and **fails on an SSH tag** with
  "gpg.ssh.allowedSignersFile needs to be configured" — GitHub still verifies
  both, and the API (`git/tags/<tag-object-sha> .verification`) is the check
  this skill relies on. Before creating the tag, confirm the key is unlocked:
  `echo test | gpg --clearsign --default-key <id> >/dev/null` for GPG, or
  `ssh-add -l` for SSH. A tag created with a locked key fails at creation,
  not at push, so this is a convenience, not a gate.
- **Partner Center state.** `gh api repos/aethersdr/AetherSDR/actions/variables
  --jq '.variables[].name'` lists `AETHERSDR_STORE_PRODUCT_ID`, so every
  non-hotfix `v*` tag stages a Store draft. The staging step runs `msstore
  publish … --noCommit`; it deletes an existing API-created submission and
  creates a new one, and **fails when Partner Center holds an in-progress
  submission that was created in the portal** — "Ingestion API can only
  update, delete, and commit submissions that are created through the API"
  (v26.8.1, v26.9.4). The skill cannot see Partner Center. Ask the maintainer
  to confirm there is no in-progress submission **before** the tag; a failed
  staging cannot be re-run, because a manual dispatch never stages
  production (step 5).
- **Hotfix mode** (nonzero fourth component): the branch strategy is an
  "Always ask" with "prep lands on `main` through `/release-prep`, tag the
  squash commit" recommended; the base release's notes are fetched for the
  body (step 2); `--latest=false` is asked when the base line is older than
  the current latest.
- **Fresh worktree, never the invoking checkout; never `git stash`.** The tag
  needs no worktree state beyond a fetched `origin/main` — tag from any
  clone of the org repo with `origin` pointing at `aethersdr/AetherSDR`. The
  website post (step 6) needs a worktree of `aetherweb`, made when it is
  needed. Keep `tagmsg.txt`, `notes.md` and the downloads in a scratch
  directory outside any repo.

Now batch the open "Always ask" items into one `AskUserQuestion` call and
wait. Nothing below runs until they are answered.

## 1. The tag — signed, on the squash-merge SHA, pushed alone

`scripts/release_notes.py` writes `tagmsg.txt` and `notes.md` from the
CHANGELOG at the SHA:

```sh
python3 .claude/skills/tag-release/scripts/release_notes.py \
  --version vX.Y.Z --prev vPREV --ref "$SHA" --cut-by "Jeremy KK7GWY" --out-dir "$SCRATCH"
```

`--prev` is the previous tag (the highest `v*` tag below this version; a
hotfix's `--prev` is its base). The script prints both files; **read the tag
message and rewrite its paragraph** if the CHANGELOG intro does not read as
one five-to-seven-line paragraph, operator-facing half first, structural half
last. The shape (v26.9.3, v26.9.4):

```
AetherSDR vX.Y.Z — <first clause of the CHANGELOG headline>

<one paragraph, five to seven lines>

N merged changes from M human contributors, AetherClaude and Dependabot.
Cut by <name> <callsign>.
```

The first clause is the headline text before the first ` · `. The count line
carries no Dependabot number (the CHANGELOG intro does). Retired: `N commits
since vPREV.` as the opener with five paragraphs (v26.8.x); a one-sentence
message ending `Cut by Pat KI6BCJ.` (v26.9.1, v26.9.2); no headline at all
(v26.7.2). `references/release-notes-format.md` has them so nobody
regenerates them.

```sh
git tag -s vX.Y.Z "$SHA" -F "$SCRATCH/tagmsg.txt"
git tag -v vX.Y.Z                      # GPG: must pass; SSH: the allowedSignersFile error is expected
git cat-file -p vX.Y.Z | head -5       # object = $SHA, tagger = you
git push origin vX.Y.Z                 # the tag alone — never with a branch, never --tags
date -u +%FT%TZ                        # record: the push time
```

Immediately after the push:

```sh
TAGOBJ=$(git rev-parse vX.Y.Z)         # the tag object, not the commit
gh api "repos/aethersdr/AetherSDR/git/tags/$TAGOBJ" --jq '{verified: .verification.verified, reason: .verification.reason, tagger: .tagger.name}'
```

`verified == true, reason == "valid"` is the verification the report cites,
for GPG and SSH alike. The three build workflows start within ten seconds of
the push; do not wait for them before step 2.

## 2. The release — created before CI attaches anything

In the same minute as the tag push:

```sh
gh release create vX.Y.Z --verify-tag \
  --title "AetherSDR vX.Y.Z — <first clause>" \
  --notes-file "$SCRATCH/notes.md"
```

Jeremy's last two releases were created in the same second as the tag. The
title, the tag's first line and the CHANGELOG headline's first clause are one
string; the script writes the title as the first line of `notes.md`'s
companion `title.txt`, so copy it rather than retyping it.

**The body**, verbatim shape from v26.9.4 and derived by the script: the
CHANGELOG section minus its `## [vX.Y.Z] — date` line, minus its `###
<headline>` line, and minus the `73,` sign-off that ends the Contributors
block; then `### Downloads` with the fixed paragraph; then the `**Full
diff:**` line with the `docs/VERIFYING-RELEASES.md` link at the tag; then the
`73,` sign-off once. The footer wording is in
`../release-prep/references/changelog-style.md`. Retired and not
regenerated: `N commits across M contributors since [vPREV]…` with a curated
`### Headlines` and a `**Verifying releases:** … Signing key:` footer
(v26.7.3–v26.9.1), and the Stream Deck plugin sentence (plugins removed in
#5600).

**Hotfix body** (structure from v26.7.4.1, footer current): a `> **Hotfix
release.**` blockquote naming the base release and who should upgrade; `##
The fix` with the fix, its issue and PR numbers and why it could not wait;
`---`; `# Everything in vBASE` repeating the base release's body up to its
`### Downloads`; then the current footer with two `**Full diff:**` links —
base…hotfix and previous-full…hotfix. `release_notes.py --hotfix-base vBASE
--fix-file fix.md` assembles it.

After creation, confirm and record:

```sh
gh release view vX.Y.Z --json isDraft,isPrerelease,targetCommitish,author,createdAt,publishedAt,url
gh api repos/aethersdr/AetherSDR/releases/latest --jq .tag_name
python3 .claude/skills/tag-release/scripts/release_notes.py --version vX.Y.Z --prev vPREV --ref "$SHA" --check --no-write
```

Not draft, not pre-release, `target_commitish` `main`, author is you, latest
names this tag (unless `--latest=false` was decided), and `--check` reports
no diff between the published title and body and the derivation. A hand edit
to the body is allowed only when the maintainer asked for it, and the report
says what changed.

## 3. Watch the builds — assets, not run colours

The tag starts **AppImage**, **Windows Installer** and **macOS DMG**
(`push: tags: ['v*']`).

```sh
gh run list --branch vX.Y.Z --json workflowName,event,status,conclusion,attempt,createdAt,updatedAt,url,databaseId
```

Timing from the last five tags (v26.8.4 through v26.9.4), measured from the
tag push to the asset's `created_at`:

| asset | attaches after |
|---|---|
| both AppImages | 13–22 min |
| Apple Silicon DMG | 15–19 min |
| Windows setup, ZIP and `.msixupload` | 27–40 min |
| Intel DMG | 30–42 min (July's Intel legs took 1.7–2 h) |

Poll every few minutes; read `conclusion`, not `status`. The Intel leg is the
one that fails: v26.7.4.1's succeeded on attempt 2 a day later; v26.7.2's
DMG job failed and the maintainer uploaded both DMGs by hand the next
morning. A failed platform job is re-run once without asking (`gh run rerun
<id> --failed`); a second failure is an "Always ask"; a hand upload goes in
the report by name and uploader.

**Sign Release Artifacts** runs on `workflow_run` after **AppImage** and
after **Windows Installer** each conclude — two runs per tag, serialised by
a concurrency group. Each run is skipped by its own `if` unless the
triggering build **succeeded** and its head branch starts with `v`. A run
that passes the `if` waits up to 30 minutes for the four GPG-signable assets
(both AppImages, the setup.exe, the portable ZIP), checks out the **tag** for
the source tarball, writes `SHA256SUMS.txt` over exactly five files (the four
plus the tarball — DMGs and the `.msixupload` are deliberately absent), signs
everything and uploads with `--clobber`. Its runs execute on the default
branch, so `gh run list --branch vX.Y.Z` never shows them:

```sh
gh run list --workflow sign-release.yml --limit 6 --json event,status,conclusion,createdAt,updatedAt,url
```

Match `createdAt` to the build completions: the first run starts seconds
after AppImage concludes and spends its time in the wait step until the
Windows assets land; the second starts when Windows concludes and is
`skipped` when Windows went red (v26.8.4, v26.9.1, v26.9.4 — one success, one
skipped, the set fully signed). **If no signing run has succeeded within ten
minutes of the four signables being present, dispatch it** (do not ask):

```sh
gh workflow run sign-release.yml -f tag=vX.Y.Z
```

A run that exists and is neither concluded nor waiting for a missing asset
is an "Always ask" before a second dispatch.

**The Windows run's colour includes the Store step** ("Stage Microsoft Store
submission (draft)" is not `continue-on-error`). Red Windows with all three
Windows assets attached is a Store problem, not a build problem:

```sh
gh run view <windows-run-id> --json jobs \
  --jq '.jobs[] | {name, conclusion, steps: [.steps[] | select(.name | test("Plan Store|Create MSIX|Attach to release|Stage Microsoft")) | {name, conclusion}]}'
```

Do not re-run a Windows job that is red only at the Store step: the assets
are attached, the re-run rebuilds and re-attaches them under new timestamps
after their signatures, and the staging still fails (step 5).

## 4. Verify the asset set — Principle VIII, on the files

`scripts/check_release_assets.py` does steps 3, 4 and 5 as one command,
printing each check as `PASS`, `FAIL`, `WARN`, `INFO` or `SKIP` and exiting
non-zero on any `FAIL`:

```sh
python3 .claude/skills/tag-release/scripts/check_release_assets.py --version vX.Y.Z --download-dir "$SCRATCH/dl"
```

Run it after the signing run concludes, and paste its output into the
report. What it checks, and what you check by hand where it cannot run:

- **The tag**: annotated, on `origin/main`, `verification.verified` true with
  reason `valid`, tagger named.
- **The release**: exists, not draft, not pre-release, target `main`, title
  equals the tag's first line, latest points here (or `--not-latest`).
- **The runs**: the three build workflows' conclusions and attempts; the
  macOS jobs' "Code sign application", "Sign DMG" and "Notarize DMG" step
  conclusions per matrix leg — **that is the notarization proof**, not the
  DMG's presence, since `spctl` cannot run from Linux or Windows; the Windows
  job's "Create MSIX package", "Attach to release" and "Stage Microsoft Store
  submission (draft)" steps; the signing runs since the tag push and whether
  one succeeded.
- **The set** (`references/asset-manifest.md`; v26.9.3 and v26.9.4 have
  exactly these fifteen):
  - `AetherSDR-vX.Y.Z-x86_64.AppImage` and `-aarch64.AppImage`, each with `.asc`
  - `AetherSDR-vX.Y.Z-macOS-apple-silicon.dmg` and `-macOS-intel.dmg` (Apple-notarized, no `.asc`)
  - `AetherSDR-vX.Y.Z-Windows-x64-setup.exe` and `-Windows-x64-portable.zip`, each with `.asc`
  - `AetherSDR-vX.Y.Z-source.tar.gz` with `.asc`
  - `SHA256SUMS.txt` with `.asc`
  - `AetherSDR-X.Y.Z.0-Windows-x64.msixupload` — absent for a hotfix

  Every `.asc` on the four CI-built binaries postdates its binary (a re-run
  that re-attached a binary after signing breaks this, and the signature is
  then for other bytes); the tarball and `SHA256SUMS.txt` are made by the
  signing job and uploaded in the same batch as their signatures, so those
  timestamps tie. The
  `.msixupload` version is the release version plus `.0`: v26.9.2's read
  `26.9.205.0` (the workflow run counter, fixed in #5467) and v26.7.4.1's
  `26.7.4.1` (a hotfix number the Store cannot take; a hotfix now produces
  none). Anything else on the release is a `WARN` to explain.
- **Actually verify, do not list.** The script downloads `SHA256SUMS.txt`,
  its `.asc`, the source tarball and the x86_64 AppImage (`--all` downloads
  every signable), imports `docs/RELEASE-SIGNING-KEY.pub.asc` (fingerprint
  `B765 6E6B CB2E 022B 79F0 F97B 5578 D10E 3D59 18F3`), runs `gpg --verify
  SHA256SUMS.txt.asc SHA256SUMS.txt`, recomputes SHA-256 over every
  downloaded file against `SHA256SUMS.txt`, runs `gpg --verify` on one
  artifact's `.asc`, checks that `SHA256SUMS.txt` names exactly the five
  files, and opens the tarball to read `AetherSDR-vX.Y.Z/CMakeLists.txt`'s
  `project(AetherSDR VERSION …)` — the pre-#5029 tarballs were archived from
  `main`, not the tag. Where `gpg` is absent the signature checks print
  `SKIP` with the reason, and the report says they did not run.

By hand, the same thing is:

```sh
gh release download vX.Y.Z --dir "$SCRATCH/dl" --pattern 'SHA256SUMS.txt*' --pattern '*source.tar.gz*' --pattern '*x86_64.AppImage*'
gpg --import docs/RELEASE-SIGNING-KEY.pub.asc
(cd "$SCRATCH/dl" && gpg --verify SHA256SUMS.txt.asc SHA256SUMS.txt && sha256sum -c --ignore-missing SHA256SUMS.txt && gpg --verify AetherSDR-vX.Y.Z-source.tar.gz.asc AetherSDR-vX.Y.Z-source.tar.gz)
tar -xzOf "$SCRATCH/dl/AetherSDR-vX.Y.Z-source.tar.gz" AetherSDR-vX.Y.Z/CMakeLists.txt | grep 'project(AetherSDR VERSION'
```

## 5. The Microsoft Store — read the step, state the recovery, touch nothing

On a non-hotfix `v*` tag the Windows workflow builds the `.msixupload`,
attaches it, and stages a **draft** production submission (`msstore publish
… --noCommit`); a maintainer clicks **Submit to Store** in Partner Center. CI
never submits. The skill:

- **Reads the step result** from the Windows run's jobs (step 3's query).
  Success: "Microsoft Store submission staged successfully." Say so, and that
  the click is the maintainer's.
- **On failure, quotes the reason** — the lines after `Running: msstore
  publish …` in the job log (`gh run view --job <id> --log`, or
  `check_release_assets.py`, which prints them). Two shapes are in the record:
  - *"Ingestion API can only update, delete, and commit submissions that are
    created through the API. Please delete the current in-progress
    submission and create one using the API"* (v26.8.1, v26.9.4): Partner
    Center holds a portal-created in-progress submission.
  - *"Uploading Bundle to Azure blob: 0%"* then exit `-1` (v26.8.4,
    v26.9.1): the `msstore` CLI 0.4.0/0.4.1 zero-timeout regression, closed
    by the v0.4.2 pin and the explicit `--uploadTimeout 300`. If it recurs,
    that is a workflow defect to file, not a Partner Center action.
- **States the recovery**: a maintainer deletes or submits the in-progress
  submission in Partner Center, then uploads the attached `.msixupload` by
  hand. **The workflow will not stage production on a re-run or a
  dispatch** — `get-store-build-plan.ps1` grants `productionDraft` only to a
  `push` of a `refs/tags/v*` ref in `aethersdr/AetherSDR`, and a re-run
  rebuilds the assets over their signatures.
- **Puts version ordering to the maintainer** when the failure is an
  ordering rejection: `docs/WINDOWS-STORE-MSIX.md` § "Version discipline" —
  Partner Center rejects a version lower than the highest accepted, and a
  `26.9.205.0` accepted under the old run-counter scheme outranks every
  `26.9.x.0`. The skill cannot see what Partner Center has accepted.
- **Never** runs the developer-flight dispatch, never passes
  `publish_store_flight`, never submits, never touches Partner Center.

## 6. The website post — the same day, in a fresh worktree of `aetherweb`

`aethersdr/aetherweb` carries one post per release. `references/website-post.md`
is the checklist with the v26.9.3 post's opening as the voice sample; the
short form:

- Clone or fetch `aethersdr/aetherweb`; `git worktree add <path>/aetherweb-vX.Y.Z -b blog/release-X-Y-Z origin/main`.
- Prepend `("release-X-Y-Z", "vX.Y.Z", "<kicker>", "<motif>")` to `RELEASES`
  in `scripts/gen-blog-art.py`; a new motif is a new `GLYPHS` entry drawn in
  that file (a ~200 px box around 0,0). Run `python3 scripts/gen-blog-art.py`
  — it writes `assets/img/release-X-Y-Z-hero.svg` and `-card.svg`.
- In `blog.html`: a card in the `.blog-grid` **below the pinned card** and
  above the newest release post (date, `N min read`, the title, a 3–4 line
  summary), and a hidden `<article class="blog-post" data-post="release-X-Y-Z"
  … hidden>` with the hero, the meta line, the `<h1>` and the body. Slug
  `release-X-Y-Z`; title `vX.Y.Z: <editorial title in lower case>`; 4–6 min
  read.
- The body is an editorial rewrite of the CHANGELOG's headline sections in
  the house voice — why a decision was made, what it costs, what is
  deliberately not included — not a bullet list of PRs. It closes with:
  `N merged changes from M contributors, plus AetherClaude and K Dependabot
  update(s)[ — including J first-time contributors this cycle]. Full release
  notes and the complete commit list: [vX.Y.Z on GitHub](…/releases/tag/vX.Y.Z).`
- `index.html`'s `<b data-latest-tag>` chip names the latest release; it is
  what `api/v1/site.json`'s `latestReleaseMentionedOnSite` is generated from,
  and it read `v26.7.1` on 2026-09-20. Update it.
- `roadmap.html`'s release view carries one `rm-rel` block per release with
  `is-latest` on the newest ("Roadmap: make the release view a record of
  what shipped", 2026-09-11) — it tracked releases through v26.9.2 and had no
  v26.9.3 or v26.9.4 row on 2026-09-20. Add the version's block and move
  `is-latest`; back-fill a missing predecessor only through the gate.
- `python3 scripts/gen-agent-discovery.py` regenerates `blog.md`,
  `api/v1/posts.json`, `api/v1/site.json`, `sitemap.xml`, `roadmap.md` and
  `functions/md-map.js`; `--check` must then report no drift. CI regenerates
  on deploy and smoke-tests the surface; Cloudflare Pages deploys on push to
  `main`.
- Title, kicker and motif are editorial: propose one of each and put them
  through the gate, with PR-versus-push. Then commit, push and open the PR
  (or push to `main` if that was the answer).

Posts have lagged: v26.8.3, v26.8.4 and v26.9.1 were written on 2026-09-01,
v26.9.2 on 2026-09-18, v26.9.3 five days late. The draft is written the day
of the tag.

## 7. Announcements and the first 48 hours

- **Announcing** is an "Always ask", with a one-paragraph draft. The
  Discussions **Announcements** category carried release posts through v0.7.9
  (2026-03-29) and none since; no CalVer release has been announced there.
  The site's nav links a Discord community. The skill has no Discord tooling
  and never posts anywhere but GitHub and the `aetherweb` PR.
- **The first 48 hours.** Bug reports arrive with the in-app reporter's
  title `[bug] <model> vX.Y.Z — …` within hours: v26.9.3 had three the next
  day, one of them a crash at open on Windows (#5713, the PortAudio sidetone
  sink). List every issue created since the tag push whose title names the
  version:

  ```sh
  gh issue list --state all --limit 100 --search "created:>=<push-time>" \
    --json number,title,createdAt,author --jq '.[] | select(.title | test("X\\.Y\\.Z"))'
  ```

  A crash-at-open or no-audio class report is flagged as hotfix material for
  the maintainer to decide. The hotfix itself is a `/release-prep` +
  `/tag-release` cycle on the fourth component, not something this pass
  starts.

## 8. Report (markdown, to the operator)

```markdown
## vX.Y.Z — cut

**Tag:** `vX.Y.Z` → `<commit sha>` (squash-merge of #<PR> on `main`, verified ancestor); tag object `<tag-object sha>`, API verification `verified=true reason=valid`, tagger <name>; pushed <time> UTC.
**Release:** <url> — title "AetherSDR vX.Y.Z — …", created <time> (<n> s after the tag), not draft, not pre-release, target `main`, Latest: yes / no (`--latest=false`, decided). Body: derived, `--check` clean / hand-edited: <what>.

### Decided by you
Each decision put through AskUserQuestion and the answer. "None." if so.

### Workflows on the tag
| workflow | run | attempt | conclusion | note |
|---|---|---|---|---|
| AppImage | <url> | 1 | success | |
| Windows Installer | <url> | 1 | failure | red at Store step; assets attached 19:38 |
| macOS DMG | <url> | 1 | success | Sign DMG / Notarize DMG success on both legs |
| Sign Release Artifacts | <url> | — | success | workflow_run, after AppImage |
| Sign Release Artifacts | <url> | — | skipped | workflow_run, Windows red |
Re-run or dispatched by hand: <what, when> / none.

### Assets (15 / 14 for a hotfix)
- [x] AetherSDR-vX.Y.Z-x86_64.AppImage — .asc +<min> after the binary (tarball and SHA256SUMS: same upload as their .asc)
- … one line per expected asset, ticked or **missing** …
- [x] AetherSDR-X.Y.Z.0-Windows-x64.msixupload — version checked / n/a (hotfix)
SHA256SUMS.txt covers: the five expected files. Verified locally: `gpg --verify SHA256SUMS.txt.asc` OK, sha256 of <files> OK, `gpg --verify <artifact>.asc` OK, tarball CMakeLists says X.Y.Z / not run: <why>.

### Store
Draft staged: yes — Submit to Store is the maintainer's click / **no** — step failed: "<quoted reason>". Recovery: <as step 5>. / n/a (hotfix).

### Website
aetherweb PR <url> (title "…", kicker "…", motif `…`) / pushed to main <sha>. `data-latest-tag` and roadmap row updated: yes / no.

### Announcement
Decided: <where, or not>. Text: <the paragraph> / n/a.

### Issues naming vX.Y.Z since the tag
#NNNN <title> — <flagged hotfix material / noted> / none yet.

### Cleanup
aetherweb worktree removed; `tagmsg.txt`, `notes.md`, `title.txt` and the downloads deleted from the scratch directory / what remains and why.
```

State current state, not the churn.

## Never

- Tag a commit that is not an ancestor of `origin/main`, tag before the prep
  PR merges, move or delete a published tag, or push the tag with a branch.
- Let CI create the release: create it first, with the title that equals the
  tag's first line.
- Hand-edit a release body away from the derived notes without saying so in
  the report.
- Claim an asset is signed because a `.asc` exists — its timestamp and a
  `gpg --verify` are the claim; claim notarization from a filename.
- Regenerate a retired notes shape, the `### Headlines` block or the Stream
  Deck sentence.
- Submit to the Store, run the developer-flight dispatch, pass
  `publish_store_flight`, or touch Partner Center.
- Re-run a Windows job that is red only at the Store step.
- Post to Discord or Discussions on your own initiative; publish the website
  post without the maintainer's title.
- Edit `.github/workflows/`; work or `git stash` in the invoking checkout.
- Decide anything in the "Always ask" list without asking.
- Call the pass finished before the asset set has been verified on the
  files and the report written.
