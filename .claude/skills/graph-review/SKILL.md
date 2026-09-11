---
name: graph-review
description: Graph-orchestrated adversarial PR review for AetherSDR — the /pr-review stance and rules, run as a multi-agent workflow. Four parallel audits (issue-fit, scope→preference, governance, quality) fan out alongside a single build; every finding is independently refuted by two verifier lenses before it can be reported; one bridge session drives every runtime claim against the demo backend; one synthesis node writes the review and report. Posts one GitHub review with inline comments and returns a markdown report. Use when asked to graph-review a PR (e.g. "/graph-review 4609"). Falls back to /pr-review where the Workflow tool is unavailable.
---

# Graph review (issue-fit + governance + quality, as a graph)

Review the PR given in `$ARGUMENTS` (a PR number, or a full PR URL; if
absent, use the PR for the current branch via `gh pr view`). The
deliverables are the same as `/pr-review`: **one posted GitHub review**
(inline comments, suggestions, the right review event) and **a markdown
report to the operator**. Post nothing else to GitHub — no labels, no extra
comments, no merges.

The difference is shape. `/pr-review` is one agent walking ten steps in one
context. This skill runs the same steps as a graph:

```
              ┌──────────── you (coordinator, inline) ────────────┐
              │ 0 preflight   1 gather + worktrees   2 dispatch    │
              └───────────────────────┬───────────────────────────┘
                                      ▼  Workflow(workflow.js, args)
   ┌───────────┬────────────┬─────────────┬───────────┐   ┌───────┐
   │ issue-fit │ scope      │ governance  │ quality   │   │ build │  Audit (parallel)
   │           │  └►pref.   │             │           │   │       │
   └─────┬─────┴─────┬──────┴──────┬──────┴─────┬─────┘   └───┬───┘
         ▼           ▼             ▼            ▼             │
      verify×2    verify×2      verify×2     verify×2         │       Verify (per finding,
     (reproduce, calibrate) — as each audit lands, no barrier │        two lenses)
         └───────────┴──────┬──────┴────────────┘             │
                            ▼  barrier: all runtime claims + build
                        ┌───────┐
                        │ drive │  one bridge session, demo backend only    Drive
                        └───┬───┘
                            ▼
                      ┌────────────┐
                      │ synthesize │  review payload + operator report      Synthesize
                      └─────┬──────┘
              ┌─────────────▼───────────────────────────────────┐
              │ you: 3 post the review   4 report + clean up     │
              └─────────────────────────────────────────────────┘
```

Why this shape earns its cost: each audit gets a small, single-purpose
context instead of position 400 of 640 lines; the scope table and the
"attacks survived" list are schema-required, so they cannot be skipped; and
no finding reaches the author until an agent with no stake in it has tried
to refute it. Expect roughly 8 agents plus 2 per raised finding; the
verifiers are what make a clean verdict trustworthy, so do not trim them.

Every node reads [`nodes/stance.md`](nodes/stance.md) and then its own file
under [`nodes/`](nodes/). Those files are the rules; this file is the
control flow. Edit the rules there, the graph in
[`workflow.js`](workflow.js), and keep this file about what you do inline.

**If the `Workflow` tool is not available in this session** (Codex, Copilot,
a remote session without it), do not emulate the graph by hand — invoke
`/pr-review` on the same argument and say so in the report. This skill's
instructions are the explicit opt-in the Workflow tool requires; you do not
need the operator to say "workflow" as well.

Do all repo reads through `gh` / `git show`, or in the scratch worktrees you
create below. **Never mutate the checkout you were invoked in.** Someone else
may be working in it, and a `git checkout` there retargets them silently.
Where `gh` is unavailable, use the GitHub MCP tools (`pull_request_read`,
`issue_read`, `pull_request_review_write` + `add_comment_to_pending_review`)
in its place throughout.

## 0. Test-boundary preflight — inline, before anything runs

This is the gate for the whole graph; the build node treats its output as
binding and will not build or run anything you flag.

Read `AGENTS.md`'s "Test-layer boundary". Fetch the PR's own sources first —
`gh pr diff <PR>`, or `git fetch origin pull/<PR>/head` then
`git show FETCH_HEAD:<path>`; never the working tree, which is the base
branch and passes this preflight vacuously. Inspect the PR's added or
modified test sources — not only `tests.cmake` — for socket ownership or a
synthetic peer: `QTcpServer`, `QTcpSocket`, `QUdpSocket`, `QLocalServer`,
`QWebSocketServer`, `bind()`, `listen()`, `connectToHost()`, peer
processes, and `Fake*` radio, amplifier, or tuner classes. Also check
whether an existing registered target has quietly gained network behavior.
Distinguish a socket object used as an inert value from a test that opens,
binds, listens, or connects.

If a new or modified socket-based test is found:

- Notify the operator now, with the test, socket type, target, and whether
  CI runs it. Do not wait for the graph to finish.
- Record it as `preflight.socketTestFound: true`, list the CTest targets in
  `preflight.socketTargets`, and put the notification text in
  `preflight.notice` — the synthesis node includes it in the review body.
- Continue: the rest of the review still runs and still gets posted.
  `AGENTS.md` requires the operator be notified *before continuing*, not
  that the review be abandoned. Getting explicit, PR-specific direction
  before anyone *executes* that target is the build node's constraint, and
  it honors it.
- A PR that **removes** a socket test or fake peer is the remediation
  #5254 asks for. Notify, then review it normally.

## 1. Gather — inline, then create the worktrees

Scout first; the graph needs a work-list, not a blank PR number.

```sh
gh pr view <PR> --json title,body,author,baseRefName,headRefName,state,mergeable,statusCheckRollup,closingIssuesReferences,reviews,comments,headRefOid
gh pr view <PR> --json files   -q '.files[] | "\(.additions)+ \(.deletions)-  \(.path)"'
gh pr view <PR> --json commits -q '.commits[] | "\(.oid[0:8]) \(.authoredDate[0:10]) \(.messageHeadline)"'
gh pr diff <PR>
```

From that, assemble:

- `linkedIssues`: `closingIssuesReferences` plus any `#NNNN` the title/body
  says it fixes/closes/resolves.
- `ciSummary`: one line — which checks passed/failed, and whether the
  trigger was `pull_request` (tests the merge result, so green includes
  current main) or `push` (does not).
- `botComments`: the text of any review-bot comment already on the PR, or
  null. Nodes treat it as leads, never as findings.
- `testsToInvert`: for every test the diff adds or modifies, an entry
  `{test: <ctest name>, mutation: "revert the fix hunks in <files>"}`. The
  build node runs these; audits may request more, and the report will say
  which of those did not get run.
- `buildBase`: true when the PR claims a fix whose "before" the drive node
  could reproduce against the demo backend (a user-visible symptom with a
  repro in the issue). False for refactors, docs, headless changes.

Then the worktrees — on disk, never on the tmpfs scratchpad, per the local
operator rules if present:

```sh
SID=<first 8 chars of your session id>
ROOT=~/agent-worktrees/$SID; mkdir -p $ROOT/tmp
git -C <shared checkout> fetch origin pull/<PR>/head:refs/remotes/origin/pr-<PR>-head
git -C <shared checkout> worktree add $ROOT/wt-pr<PR>-head origin/pr-<PR>-head --detach
# only if buildBase:
BASE=$(git -C <shared checkout> merge-base origin/main origin/pr-<PR>-head)
git -C <shared checkout> worktree add $ROOT/wt-pr<PR>-base $BASE --detach
```

Chain `worktree add && …` — a failed add must not drop later commands into
the shared checkout.

## 2. Dispatch the graph

Call the `Workflow` tool with `scriptPath` pointing at this skill's
[`workflow.js`](workflow.js) and `args` as a real JSON object (not a
string):

```json
{
  "pr": 4609, "title": "…", "author": "…", "body": "…",
  "headSha": "…", "baseSha": "…",
  "linkedIssues": [4600],
  "files": ["12+ 3-  src/…", "…"],
  "commits": ["abcd1234 2026-09-01 fix(gui): …", "…"],
  "headWorktree": "/home/…/wt-pr4609-head",
  "baseWorktree": "/home/…/wt-pr4609-base",   // or null
  "tmpDir": "/home/…/agent-worktrees/<sid>/tmp",
  "scratchDir": "<your scratchpad>",
  "buildBase": true,
  "testsToInvert": [{"test": "…", "mutation": "…"}],
  "preflight": {"socketTestFound": false, "socketTargets": [], "notice": ""},
  "ciSummary": "…", "botComments": null,
  "skillDir": "/abs/path/to/checkout/.claude/skills/graph-review"
}
```

`skillDir` must be **absolute** (the nodes `Read` their rule files by absolute
path); point it at this skill directory in the checkout you were invoked
from. The workflow runs in the background; you get a task notification
when it completes. Do not do parallel review work of your own while you
wait — that defeats the point, and your context is needed clean for step 3.

While waiting, if the preflight flagged a socket test and the operator has
answered with direction, hold it for the report; the graph will not have
run that target either way.

When the notification arrives, read the return value. If it is empty or the
run failed, read `<transcriptDir>/journal.jsonl` before diagnosing — it
records each agent's actual return. Resume with `resumeFromRunId` after
fixing whatever broke rather than re-running from scratch.

## 3. Post the review

The synthesis node returned `synth.review` as `{event, body, comments}`.
Post exactly ONE review from it:

- Write it to `review.json` in your scratchpad and pass it with `--input` —
  bodies contain newlines, backticks, and code fences that do not survive
  shell quoting:
  `gh api repos/{owner}/{repo}/pulls/<PR>/reviews --input review.json`
- If `socketNotice` is non-empty and not already in the body, prepend it.
- If the call fails (an anchor line not in the diff is the usual cause),
  drop or re-anchor the offending comment and retry once; if it still
  fails, fall back to `gh pr review --comment|--request-changes -F body.md`
  with the inline findings folded into the body, and say so in the report.
- Branch protection has `dismiss_stale_reviews` enabled: any push dismisses
  an existing approval. Approval stays the operator's call; this skill never
  posts `APPROVE`.

## 4. Report and clean up

Return `synth.report` to the operator as markdown, verbatim, followed by a
short **Graph stats** line from the workflow's `stats`: findings raised /
confirmed / refuted, runtime claims driven, whether the build succeeded and
the app was driven, and any inversions the audits requested that were not
run. A high refuted count is a feature — it is the plausible-but-wrong
findings that did not reach the author.

Then clean up, in this order:

1. `pgrep -a AetherSDR` — if the drive node left an instance with your
   `AETHER_AUTOMATION_IDENTITY=pr-<PR>-review`, close it. Never touch one
   that is not yours.
2. `git -C <shared checkout> worktree remove --force $ROOT/wt-pr<PR>-head`
   (and `-base`), then `git worktree prune`. The build trees go with them.

Say in the report which instance was driven and that it was the demo, and
that the worktrees were removed.
