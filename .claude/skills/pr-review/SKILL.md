---
name: pr-review
description: Full PR review for AetherSDR — verifies the PR against its linked issue, audits scope creep and undisclosed changes, checks governance and code quality, posts one GitHub review with inline comments, and reports blockers/nits/recommendation as markdown. Use when asked to review a PR (e.g. "/pr-review 4609").
---

# PR Review (issue-fit + governance + quality)

Review the PR given in `$ARGUMENTS` (a PR number, or a full PR URL; if absent,
use the PR for the current branch via `gh pr view`). The deliverables are
**one posted GitHub review** (step 8 — inline comments, suggestions, and the
right review event) and **a markdown report to the operator** (step 9).
Post nothing else to GitHub — no labels, no extra comments, no merges.

Work in parallel where the steps are independent. Do all repo reads through
`gh`/`git show`, or check the PR out in a **scratch worktree** — never mutate
the checkout you were invoked in. Someone else may be working in it, and a
`git checkout` there retargets them silently. (Some setups make this a hard
rule in a local `CLAUDE.local.md`; treat it as one regardless.)

Where `gh` is unavailable — Claude Code Remote and web sessions have no `gh`
CLI — use the GitHub MCP tools (`mcp__github__*`) in its place throughout:
`pull_request_read` for `gh pr view` / `gh pr diff`, `issue_read` for
`gh issue view`, and `pull_request_review_write` +
`add_comment_to_pending_review` for the step 8 posting flow.

## 1. Gather

- `gh pr view <PR> --json title,body,author,baseRefName,headRefName,state,mergeable,statusCheckRollup,closingIssuesReferences,reviews,comments`
- `gh pr diff <PR>` — the full diff is the primary evidence. For context
  beyond the hunks, read the touched files at the PR head
  (`git fetch origin pull/<PR>/head` then `git show FETCH_HEAD:<path>`), or
  check the branch out in a **new scratch worktree** if you need to build.
- CI: note failing/passing checks and whether CI ran against a stale merge
  base. Check the workflow trigger before drawing conclusions: a
  `pull_request` trigger tests the *merge* result, so a green check already
  includes current main; a `push`-triggered check on the branch does not.

## 2. Linked issue → does the PR actually solve it?

- Linked issues = `closingIssuesReferences` plus any `#NNNN` referenced in
  the PR title/body as "fixes/closes/resolves". Read each with
  `gh issue view <N> --json title,body,comments` (skim comments for accepted
  repro steps, maintainer rulings, or scope changes — the issue *thread*
  often redefines the ask).
- Build a short requirements list from the issue (symptom, repro, acceptance
  expectations, any explicit non-goals), then map each requirement to the
  diff: which hunk addresses it? Flag requirements the diff does not touch,
  and diff changes that no requirement explains — those feed the scope audit
  in step 3, which is mandatory on every review.
- Check the tests: is there a test that fails without the fix and passes
  with it? A fix for a reported bug with no regression test pinning the
  reported symptom is at minimum a nit, and a blocker for bug-class fixes
  the project has pinned before (see the mutation-testing culture in recent
  settings PRs: "break the guard on purpose and watch the test fail").
- If there is NO linked issue: say so, review the PR against its own stated
  intent, and note whether project process wanted an issue/RFC first
  (GOVERNANCE.md — architectural changes need an RFC; "bug fixes with a
  clear root cause" explicitly do not).

## 3. Scope audit — does the PR do only what it says? (always)

Run this on every review, whether or not the operator asked for it, and give
it a section in both deliverables. A PR is a claim ("this does X"), and the
diff is the evidence; anything in the diff that X does not explain is a
finding. This is the check that most often turns up something real, and it is
cheap.

Start from the file and commit lists, not the prose:

```sh
gh pr view <PR> --json files   -q '.files[] | "\(.additions)+ \(.deletions)-  \(.path)"'
gh pr view <PR> --json commits -q '.commits[] | "\(.oid[0:8]) \(.authoredDate[0:10]) \(.messageHeadline)"'
```

Use **`authoredDate`, not `committedDate`** — a rebase rewrites the commit
date to the day the branch was pushed, so `committedDate` flattens the whole
branch to one date and hides exactly the outlier you are looking for. The
author date survives a rebase.

Then build a **scope table** — one row per file or coherent group: what it
changes, whether the title/body claims it, and a verdict. Put that table in
the posted review body and in the operator report. It is the artifact; prose
alone lets things slide past.

What to look for:

- **Commit dates that predate the PR's own first commit**, or a commit whose
  message has nothing to do with the linked issue. A local build workaround
  cherry-picked onto the branch shows up exactly this way.
- **Files no requirement explains.** Build config, CI, unrelated plugins,
  vendored trees, formatting-only churn in files the fix does not need.
- **New public surface**: a new protocol verb, wire message, config key, CLI
  flag, exported API, settings key, or capability field. Third parties bind
  to these and they outlive the fix. Even when the linked issue motivates it,
  a *protocol addition arriving as a side effect of a bug fix* is a maintainer
  call — name it as one (step 5's GOVERNANCE.md framing) rather than either
  waving it through or calling it a violation.
- **Deleted behavior, not just added code.** Read the `-` lines as carefully
  as the `+` lines. A removed guard, early return, confirmation, or comment
  citing a fixed issue means a previously-fixed bug may be back. Grep the
  removed side for it:
  `gh pr diff <PR> | grep '^-' | grep -iE 'guard|#[0-9]{3,}|return|if \(' `
  When a removal deletes a comment that *names a symptom*, quote that comment
  back and ask what now prevents it. If the replacement code still concedes
  the same precondition, the guard's removal is a regression, not a cleanup.
- **Sibling implementations left behind.** If the fix touches one of several
  parallel copies (one of N plugins, one of N backends, one of N call sites),
  grep for the others and say which remain broken. That is a completeness
  note, not usually a blocker — but the PR should not read as "fixed" when
  two of three surfaces still carry the defect.
- **Dead additions.** An added file or target that nothing references is
  still scope, and it is worse than scope — verify reachability before
  believing it works (grep for whatever registers it: the module path, the
  target list, the dispatch table). "It's harmless, it's only for my machine"
  is an argument for a separate PR, not for merging it here.
- **The body's own checklist.** Many templates carry claims like "Changes are
  limited to the scope of this issue" / "No unrelated files or formatting
  changes". Check them against the diff. A self-certification that is false
  is itself worth reporting — plainly and without moralizing.

Verdicts — apply these consistently:

| Finding | Verdict |
|---|---|
| Unrelated to the issue and to the stated fix | **Blocker.** Ask to unbundle: drop the commit, open its own PR |
| Explained by the issue *thread* but absent from the PR body | Not a blocker on its own — but name it, and ask for the body to be updated so it is reviewable and searchable later |
| New public/protocol surface | **Needs maintainer decision**, flagged to the maintainer by name |
| A removed guard whose symptom can recur | **Blocker** (a regression), with the deleted comment quoted as evidence |
| User-visible default changed, correct but undisclosed | Nit — plus a request to state it in the body |

Distinguish this from step 6. Scope is about *whether the change belongs in
this PR*; step 6 is about *whether the change is legitimate at all*. A change
can be perfectly correct and still be out of scope, and that is the common
case — say so without implying bad faith. Bundling is usually convenience,
not concealment. Report what the diff does and let the maintainer rule.

## 4. Automated pass

If a `code-review` skill is available and model-invocable, run it at medium
effort on this PR and fold its findings into the report — attribute them
("automated pass found …"), keep only findings you verified yourself, and drop
anything you can refute with evidence.

It may not be invocable (some builds mark it `disable-model-invocation`, and
the call fails). That is not a blocker: do the equivalent pass yourself under
step 7 and say in the report that the automated pass was unavailable. Never
imply an automated pass ran when it did not.

If a review bot has already commented on the PR, read it — but verify its
claims rather than adopting them. Bots miss things and occasionally assert
things that are not true; both are worth catching.

## 5. Governance audit (project canon, in priority order)

Read the diff against each of these; cite the specific rule when flagging:

- **CONSTITUTION.md** — the principles are binding. Most commonly implicated:
  I (FlexLib is protocol/model authority — `reference/FlexLib_*` is the
  answer, and for HL2 the gateware RTL is the analogue), II/III
  (radio-authoritative state: the client never re-asserts what the radio
  owns; persistence must be **capability-shaped**, never family-checked),
  V (feature-owned config: one versioned JSON document, one owner, one
  migration point), VI (TX safety: nothing restores or automates into a
  keyed transmitter).
- **AGENTS.md** — Settings Persistence (SQLite store; flat keys are
  app-global only; per-radio state goes in `radio_settings` feature
  documents via `RadioModel::settingsScope()`; check the write result),
  Settings Migration (one-shot claim-and-freeze; no perpetual legacy
  fallbacks), credentials (never in the store — `SettingsCredentialPolicy.h`
  is THE table; QtKeychain only), capability declarations
  (`RadioCapabilities` + caps-map doc + gating test, per that file's
  ADDING-A-FIELD contract).
- **CMake contract** — any target compiling `AppSettings.cpp` uses
  `${AETHER_SETTINGS_SOURCES}` and joins `AETHER_SETTINGS_CONSUMERS`; tests
  isolate via `TestSettingsProfile.h` (`AETHER_SETTINGS_DIR`).
- **docs/style/dialog-patterns.md** — new dialogs ride `PersistentDialog`
  (#2605); geometry base64; frameless propagation.
- **docs/a11y.md** — accessible names on interactive widgets, throttled
  `updateAccessibility`, no interactive QLabels.
- **CONTRIBUTING.md** — tests for behavior changes, touchpoint manifest regen
  when applicable, cross-platform unless solving a platform-specific problem.
- **GOVERNANCE.md** — should this change have had an RFC/issue first? Is it
  within the scope a maintainer already ruled on?

### `CHANGELOG.md`: never ask for an entry

`CHANGELOG.md` is a **release-prep file** (AGENTS.md, the five-places version
table). An ordinary PR must not add an entry, however user-visible the change
is — the PR body and commit message are where it gets described. Do not ask
for one, and **flag one as a change to remove** if a PR adds it.

Every entry prepends to the top of the same `## [Unreleased]` list, so any two
PRs that both add one conflict with each other, and every open PR goes stale
when one of them merges. That churn is the whole reason for the rule.

### Cite the sentence, or downgrade it to a nit

Before calling anything a governance blocker, **find the sentence in canon
that states the rule** and quote it. If you cannot, it is a convention at
most, and conventions are nits.

Do not infer a rule from `git log`. If you do try to measure how universal a
practice is, note that `git log -- <path>` returns *only* commits that touched
that path — using it to compute "N of the last N commits did X" is circular
and will always return 100%. Iterate over unfiltered history and test each
commit. This exact error once turned a nonexistent CHANGELOG rule into a
merge blocker on someone else's PR.

## 6. Personal-preference / bias check

Some contributors code their personal preferences into the app without going
through the RFC process. **Any modification to an EXISTING UI element or
behavior** (new features notwithstanding) gets classified: is this a *fix*
(restores documented/intended behavior, corrects a defect, matches SmartSDR/
FlexLib reference behavior, closes an accessibility gap) or a *preference*
(changes a default, reorders/rewords/restyles working UI, alters workflow
because the author likes it better)?

How to tell them apart — a fix can point at an authority; a preference can't:

- The linked issue describes it as broken, with a repro — not "I find it
  annoying".
- FlexLib / SmartSDR reference behavior, the HL2 gateware, a spec, or
  project docs say what SHOULD happen, and the PR moves toward that.
- The old behavior contradicts the Constitution, a11y doc, or an explicit
  maintainer ruling.

Red flags for smuggled preference: changed default values (an existing
settings key's default, a slider range, a timer interval) with no issue
citing the old default as a defect; visual restyles (colors, spacing, order,
labels) bundled into an unrelated fix; keyboard/mouse behavior changes
described as "improvements"; removed confirmations or notices; scope-audit
rows from step 3 that touch UI. Bundling is itself the tell — a genuine fix
rarely needs to adjust neighboring working UI.

Verdict handling: a preference change inside a fix PR is a **blocker** — not
because the preference is necessarily wrong, but because it needs its own
issue/RFC and a maintainer ruling per GOVERNANCE.md (cite it). Suggest the
split: keep the fix hunks, move the preference to a proposal. If the whole
PR is a preference change presented as a fix, say so plainly and route it to
**Needs maintainer decision**. Never let polished code quality launder an
unratified behavior change.

## 7. Code-quality audit

Beyond the automated pass, read for: correctness of the actual state
machine/lifecycle being touched (connect/disconnect, slice recreate, radio
swap are the recurring minefields), thread-safety (audio callback vs main
thread; AppSettings is thread-safe but `save()` does I/O — never on the
render callback), Qt object lifetime (`QPointer`/`WA_DeleteOnClose`,
parenting), error handling per house style (no exceptions; check returns;
`qWarning` with category), silent failure modes (unchecked writes, swallowed
errors), and whether comments explain *why* (constraints), not *what*.

Verify at least one non-trivial claim empirically when feasible (apply the
diff in a scratch worktree and build the touched target, or run the PR's own
tests). Say in the report what you verified vs. only read. Never trust a
green CI badge over a local reproduction when they disagree.

**"CI is green" is not "the suite passes."** Every `ctest` call in `ci.yml` is
`-R`-filtered to a handful of named tests, so only a small fraction of the suite
gates a merge. If you run the suite locally, expect failures that have nothing
to do with the PR.

**Before blaming the PR for a test failure, prove it.** Build the PR's merge
base clean in a separate worktree and run the same test there. For an
intermittent failure, run it 20× on each side and compare rates — a single
run tells you nothing. Report the comparison, not the impression.

## 8. Post the review to the PR

Post exactly ONE review carrying the findings as **inline comments anchored
to the diff lines they concern**, with GitHub suggestion blocks wherever the
fix is a concrete small edit:

- Build the review via the API (the `gh pr review` command cannot attach
  inline comments). Assemble the payload as JSON and pass it with `--input`
  rather than repeated `-f` flags — bodies contain newlines, backticks and
  code fences that do not survive shell quoting:
  `gh api repos/{owner}/{repo}/pulls/<PR>/reviews --input review.json`
  where the JSON is `{event, body, comments: [{path, line, side: "RIGHT",
  body}, …]}` (use `start_line`+`line` for multi-line anchors). Anchor to
  lines that are IN the diff; a finding about untouched code goes in the
  review body instead, with a `file:line` reference.
- For mechanical fixes, embed a fenced ` ```suggestion ` block in the inline
  comment so the author can one-click apply. Suggestions must be drop-in
  correct — matching indentation, compiling in context — never pseudocode.
- **Review event:** any blocker → `REQUEST_CHANGES`. No blockers →
  `COMMENT` (nits inline, verdict in the body) — approval stays the
  operator's call unless they have said otherwise for this PR.
- The review body, in this order: the issue-fit verdict (one short
  paragraph), the **scope table** from step 3 (always — write "everything in
  the diff is explained by the issue" and move on when it is clean), the
  numbered blockers (each cross-referencing its inline comment), then nits
  marked explicitly non-blocking. State what was verified empirically vs.
  read. Anchor each out-of-scope finding inline on the file it concerns —
  line 1 of an added file is a valid anchor — so the author sees it where the
  change is, not only in the summary.
- If the posting API call fails (e.g. an anchor line is not in the diff),
  fix the anchors and retry once; if it still fails, fall back to
  `gh pr review` with the full markdown body and say so in the report.
- Branch protection has `dismiss_stale_reviews` enabled: **any push dismisses
  an existing approval.** If you are also pushing commits, approve *after*
  the last push, and re-approve after any later merge-from-main. Check
  `reviewDecision` afterwards rather than assuming it stuck.

## 9. Report (markdown, to the operator only)

```markdown
## PR #NNNN — <title> (@author)

**Issue:** #MMMM — one-paragraph summary of the problem as reported.
**Proposed fix:** one-paragraph summary of the approach the diff takes.
**Does it solve the issue?** Yes / Partially / No — with the requirement→hunk
mapping and anything unaddressed.

### Scope
The step 3 table: file/group → claimed in the title or body? → verdict. Then
one line on whether the body's own checklist claims hold up. If nothing is
out of scope, say so in a sentence — never omit the section, because "I
checked and it is clean" and "I did not check" must not look the same.

### Blockers
Numbered. Each: what's wrong, the evidence (file:line, build output, failing
scenario), which rule it violates (if governance), and what a fix looks like.
"None." if none.

### Nits
Bulleted, explicitly non-blocking. Style, naming, missed niceties, stale docs.

### Recommendation
One of: **Approve** / **Approve with nits** / **Request changes** /
**Needs maintainer decision** — plus 2–3 sentences of reasoning, what you
verified empirically, and the concrete next step (e.g. "admin-merge after CI",
"ask author for X", "needs a ruling on Y before review can conclude").
```

Calibrate severity honestly: a blocker is something that breaks users,
violates canon, or will fail on current main — not a preference. When the
verdict hinges on a scope call only the maintainer can make, say exactly
that instead of manufacturing a verdict.
