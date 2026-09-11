# Node: scope — does the PR do only what it says?

Read `stance.md` first. This runs on every review. A PR is a claim ("this
does X") and the diff is the evidence; anything in the diff that X does not
explain is a finding. This is the check that most often turns up something
real, and it is cheap.

Start from the file and commit lists, not the prose:

```sh
gh pr view <PR> --json files   -q '.files[] | "\(.additions)+ \(.deletions)-  \(.path)"'
gh pr view <PR> --json commits -q '.commits[] | "\(.oid[0:8]) \(.authoredDate[0:10]) \(.messageHeadline)"'
```

Use **`authoredDate`, not `committedDate`** — a rebase rewrites the commit
date to the day the branch was pushed, so `committedDate` flattens the whole
branch to one date and hides exactly the outlier you are looking for.

Then build the **scope table** — one row per file or coherent group: what it
changes, whether the title/body claims it, and a verdict. Return it as
`scopeTable`; it is the artifact and it is required even when every row is
clean. Also return `uiRows`: the subset of rows that touch existing UI
elements, defaults, or user-facing behavior — the preference node consumes
that list.

What to look for:

- **Commit dates that predate the PR's own first commit**, or a commit whose
  message has nothing to do with the linked issue. A local build workaround
  cherry-picked onto the branch shows up exactly this way.
- **Files no requirement explains.** Build config, CI, unrelated plugins,
  vendored trees, formatting-only churn in files the fix does not need.
- **New public surface**: a new protocol verb, wire message, config key, CLI
  flag, exported API, settings key, or capability field. Third parties bind
  to these and they outlive the fix. Even when the linked issue motivates it,
  a *protocol addition arriving as a side effect of a bug fix* is a
  maintainer call — `maintainer-decision`, not a blocker and not a pass.
- **Deleted behavior, not just added code.** Read the `-` lines as carefully
  as the `+` lines. A removed guard, early return, confirmation, or comment
  citing a fixed issue means a previously-fixed bug may be back:
  `gh pr diff <PR> | grep '^-' | grep -iE 'guard|#[0-9]{3,}|return|if \(' `
  When a removal deletes a comment that *names a symptom*, quote that comment
  back in `evidence` and ask what now prevents it. If the replacement code
  still concedes the same precondition, the removal is a regression.
- **Sibling implementations left behind.** If the fix touches one of several
  parallel copies (one of N plugins, backends, call sites), grep for the
  others and say which remain broken. A completeness nit, not usually a
  blocker — but the PR should not read as "fixed" when two of three surfaces
  still carry the defect.
- **Dead additions.** An added file or target that nothing references is
  still scope. Verify reachability (grep for whatever registers it) before
  believing it works.
- **The body's own checklist.** "Changes are limited to the scope of this
  issue" / "No unrelated files or formatting changes" — check them against
  the diff. A false self-certification is worth reporting, plainly and
  without moralizing. Return the result in `checklistHolds`.
- **`CHANGELOG.md`** is a release-prep file; an ordinary PR must not add an
  entry. Flag one as a change to remove (a nit with a concrete fix). Never
  ask for one.

Verdicts — apply consistently:

| Finding | Severity |
|---|---|
| Unrelated to the issue and to the stated fix | `blocker` — ask to unbundle: drop the commit, open its own PR |
| Explained by the issue *thread* but absent from the PR body | `nit` — ask for the body to be updated so it is reviewable later |
| New public/protocol surface | `maintainer-decision` |
| A removed guard whose symptom can recur | `blocker` (a regression), with the deleted comment quoted |
| User-visible default changed, correct but undisclosed | `nit` — plus a request to state it in the body |

Scope is about *whether the change belongs in this PR*, not whether it is
legitimate at all (the preference node rules on that). A change can be
perfectly correct and still be out of scope, and that is the common case —
say so without implying bad faith. Bundling is usually convenience, not
concealment.
