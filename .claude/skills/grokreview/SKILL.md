---
name: grokreview
description: >
  Fast AetherSDR PR review, sibling of /pr-review. Same verdicts (issue-fit,
  scope, governance, quality, one GitHub review) with shape-routed verification:
  no socket-test interrupts, no CI log hunts, one worktree build, demo-bridge
  only for GUI claims. Use when the user runs /grokreview (e.g. "/grokreview
  4609"). Do not use for generic "review this PR" — that stays on /pr-review
  until this skill is chosen to replace it.
---

# /grokreview — fast PR review

Sibling of [`.claude/skills/pr-review/SKILL.md`](../pr-review/SKILL.md). Same
deliverables, less theater. Do **not** load or follow `/pr-review` during a
`/grokreview` run — the whole point is a side-by-side comparison.

Review the PR in `$ARGUMENTS` (number or URL; if absent, `gh pr view` for the
current branch). Deliverables:

1. **One** posted GitHub review (inline comments + the right event).
2. **One** markdown report to the operator, including a **Cost** section so
   this run can be compared with `/pr-review` on the same PR.

Post nothing else to GitHub — no labels, no extra comments, no merges. Never
mutate the checkout you were invoked in; build only in a scratch worktree.

## What this deliberately skips (vs `/pr-review`)

- Socket-test **interrupt** (notify-and-wait). Socket tests are findings, not stops.
- Nested `code-review` skill / second review pass.
- Re-reading CONSTITUTION / AGENTS / CONTRIBUTING / GOVERNANCE / a11y /
  dialog-patterns unless the **diff** implicates them.
- Dual worktree (merge-base + head) as the default.
- Driving the app for headless / protocol / DSP / CMake / docs PRs.
- Fetching CI logs, running the local suite, or 20× flake loops.
- Pasting the test-layer table, CHANGELOG history, or bridge launch novel —
  those live in `AGENTS.md` and `docs/automation-bridge.md`. Point; do not copy.

## Stance (short)

The PR asserts; you falsify the load-bearing claims. A clean verdict names
what you actually tried. Do not manufacture findings, and do not manufacture
expensive evidence that cannot change the verdict. Cite file:line, command
output, or a bridge JSON — not impressions. Break the code, not the author.

## 0. Claim, then gather

Before commenting or posting:

```sh
gh pr view <PR> --json assignees,number,title,body,author,baseRefName,headRefName,state,mergeable,files,commits,closingIssuesReferences,reviews,comments,url
gh pr diff <PR> --name-only
gh pr checks <PR>
```

Then, if unassigned or assigned ONLY to `@aethersdr-agent`:
`gh pr edit <PR> --add-assignee @me`. If another human/agent is already
assigned, leave a coordination comment and stop (AGENTS.md Principle X).

Then `gh pr diff <PR>` itself. **Size gate:** if the named-files list is
mostly generated/vendored blobs, or the diff is huge, review from the file
list and only open hunks on the risky paths — do not dump a multi-MB diff
into context.

Parse `files[].path` and `commits[]` (`authoredDate` + `messageHeadline`,
never `committedDate`). Linked issues = `closingIssuesReferences` plus
`fixes`/`closes`/`resolves #N` in the title/body. Read each issue's title +
body; skim comments **only** if the body is thin or a maintainer clearly
re-scoped the ask.

**CI:** record check *names and conclusions*. Fetch a log only when a
`FAILURE`/`ERROR` name overlaps a changed file or test target. Ignore red
jobs on untouched surfaces. Green means “the filtered gate for this SHA
passed,” nothing more — do not locally re-run CI, and do not open
`ci.yml`.

## 1. Shape — this decides verification cost

Classify once from the file list. Mixed PRs take the **most expensive**
matching row, but still only for the files that justify it.

| Shape | Typical paths | Verify |
|---|---|---|
| **docs** | `*.md`, metainfo, skills, comments-only | Read. No build, no tests, no bridge. |
| **tests / CMake** | `tests/`, `tests.cmake`, workflow YAML | Build **the touched test target**. Mutation-check **new** tests. |
| **engine** | `src/core/`, `src/models/`, backends, DSP | Focused socket-free `ctest` of changed targets. No GUI. |
| **gui** | `src/gui/`, applets, resources the user sees | One PR-head worktree + demo bridge of **the claimed path**. |
| **mixed** | both engine and gui | Engine tests + bridge only for the GUI claim. |

Write the shape into the operator report **Cost** section before you spend
it. If a later step would exceed the row, skip it and say so.

## 2. Issue-fit

Build a short requirements list from the issue (symptom, repro, acceptance,
explicit non-goals). Map each to a hunk. Flag requirements the diff does
not touch, and hunks no requirement explains (feeds scope).

No linked issue: review against the PR's stated intent, and note whether
`GOVERNANCE.md` wanted an issue/RFC first (architecture yes; clear-root-cause
bug fixes no).

Missing tests: a nit unless project canon makes that coverage merge-gating
or a deterministic socket-free seam exists. Do **not** request a synthetic
firmware peer (`AGENTS.md` Test-layer boundary). Unexecuted / bracket-commented
targets are not coverage.

## 3. Scope audit (always — this is the cheap high-signal pass)

One table, in both deliverables: file/group → claimed in title/body? → verdict.

Look for: commits that predate the PR or whose message is unrelated; files
no requirement explains; new public surface (protocol verb, config key, CLI
flag, exported API, capability field); deleted guards / comments that name a
symptom; sibling copies left unfixed; added files nothing references; a
false “limited to this issue” checklist.

| Finding | Verdict |
|---|---|
| Unrelated to the issue and the stated fix | **Blocker** — unbundle |
| In the issue *thread* but missing from the PR body | Name it; ask the body be updated |
| New public/protocol surface | **Needs maintainer decision** |
| Removed guard whose symptom can recur | **Blocker** |
| User-visible default changed, correct but undisclosed | Nit |

Scope is “does it belong in this PR,” not “is it wrong.” Bundling is usually
convenience. Report the diff; let the maintainer rule.

## 4. Governance — open canon only if the diff implicates it

Do **not** preload the canon. Open at most the files the paths require:

| Diff touches | Open |
|---|---|
| Settings, `radio_settings`, credentials, `AppSettings` | `AGENTS.md` Settings Persistence / Migration / credentials |
| Persistence / restore / TX keying | `CONSTITUTION.md` II, III, VI |
| New `QDialog` / popout | `docs/style/dialog-patterns.md` |
| Interactive widgets under `src/gui/` | `docs/a11y.md` |
| Engine↔GUI includes, vendor headers | `AGENTS.md` EB1/EB2/EB3 |
| Architecture / new family / new thread | `GOVERNANCE.md` (RFC?) |
| Tests, registration, socket peers | `AGENTS.md` Test-layer boundary |

**Cite the sentence, or it is a nit.** Do not infer rules from `git log`.

`CHANGELOG.md`: never ask for an entry; **flag one to remove** if the PR adds
it (`AGENTS.md`).

**Preference vs fix** (existing UI/behavior only): a fix points at an
authority (issue repro, FlexLib/SmartSDR, gateware, canon, maintainer
ruling). A preference cannot. A preference bundled into a fix PR is a
**blocker** — split it. A whole PR that is a preference presented as a fix
→ **Needs maintainer decision**.

## 5. Code

Read the hunks and the call sites they need. Recurring minefields:
connect/disconnect, slice recreate, radio swap, audio-callback vs main
thread, Qt lifetime, unchecked writes. For each non-trivial hunk, spend a
moment on the input or lifecycle that makes it wrong — then look.

Do **not** invoke the `code-review` skill. If a file crosses ~1000 lines
because of this PR, or the diff bolts a special case onto an unrelated
flow, say so as a nit unless it is actually a defect.

### Socket tests — classify, do not run, do not wait

Inspect added/modified test sources and `tests.cmake` for `QTcpServer`,
`QTcpSocket`, `QUdpSocket`, `QLocalServer`, `QWebSocketServer`, `bind()`,
`listen()`, `connectToHost()`, peer processes, `Fake*` radio/amp/tuner.

- **Do not build or execute** that target.
- If the PR **adds** a socket-owning test or fake firmware peer: finding.
  Our own server surfaces (rigctld, CAT, TCI, automation bridge) are
  allowed by `AGENTS.md` but still need: disclosed in the PR body, cmake
  block names the socket, fail-fast or skip **exit 77** when bind fails.
  A synthetic peer for third-party firmware is not allowed in the default
  graph.
- If the PR **removes** one: that is the #5254 direction; review normally.
- Never stop to ask the operator. Record it in the review body and continue.

## 6. Verify — only what the shape row bought

Scratch worktree, never the invocation checkout:

```sh
git fetch origin pull/<PR>/head
git worktree add .worktrees/grokreview-<PR> FETCH_HEAD
```

Build with the tree's usual cmake invocation (`-j$(nproc)`, or the
operator's documented core count). Tests run headless:
`QT_QPA_PLATFORM=offscreen`. If you built the app, put the binary path in
**Cost**.

**Default is one build (PR head).** A second worktree at the merge base
only when (a) a **touched** test failed and you need to attribute it, or
(b) the issue has a concrete <2 min repro and the PR claims that exact
fix. Mutation-check new tests by breaking the guard in the test binary
you already built — do not rebuild the world.

**Bridge** only for a **gui** (or mixed-with-GUI-claim) shape, and only
the claimed path. Recipe: `docs/automation-bridge.md`. Non-negotiables:
`pgrep -a AetherSDR` first and never touch instances that are not yours;
isolated `AETHER_SETTINGS_DIR`; explicit `AETHER_AUTOMATION_SOCKET`;
`AETHER_AUTOMATION_NO_TX=1`; connect `DEMO-0001` only; if `get radio`
shows a FLEX serial, disconnect immediately; close your instance when
done. Demo is one slice and cannot TX — “could not verify” is the honest
label, not a licence to use the live radio.

Skip the bridge with one Cost line when the change is headless, the path
needs hardware, or the build failed.

Do not run the full `ctest` suite. Do not execute socket-owning targets.

## 7. Post one GitHub review

API, not `gh pr review` (that cannot attach inlines):

```sh
gh api repos/{owner}/{repo}/pulls/<PR>/reviews --input review.json
```

Payload: `{event, body, comments: [{path, line, side: "RIGHT", body}, …]}`.
Omit nothing required; **do** set `event`: any blocker → `REQUEST_CHANGES`;
no blockers → `COMMENT`. Approval stays the operator's unless they said
otherwise for this PR. Anchor to lines **in the diff**; out-of-diff findings
go in the body with `file:line`. Mechanical fixes get a GitHub
`suggestion` fence that applies cleanly. Retry once on 422 (bad anchors);
then fall back to `gh pr review` with the markdown body and say so.

Body order: issue-fit paragraph; **scope table** (always); numbered
blockers (cross-ref inlines); nits marked non-blocking; one line on what
was verified vs only read.

## 8. Operator report

```markdown
## PR #NNNN — <title> (@author)

**Shape:** docs | tests/CMake | engine | gui | mixed
**Issue:** #MMMM — one paragraph.
**Proposed fix:** one paragraph.
**Does it solve the issue?** Yes / Partially / No — requirement→hunk map.

### Scope
The step 3 table. One line on the body's checklist. Never omit this section.

### Blockers
Numbered, with evidence and the fix shape. "None." if none.

### Nits
Bulleted, explicitly non-blocking.

### Cost
- Builds: 0 / 1 (head) / 2 (head+base)
- Tests run: <target names or none>
- Bridge: no / demo (<verbs>)
- Canon files opened: <paths or none>
- CI logs fetched: none / <job names>
- Socket tests: none / added <name> (not executed) / removed <name>

### What I tried to break
Two to five bullets of attacks that did **not** become findings, plus
anything you could not test and why.

### Recommendation
**Approve** / **Approve with nits** / **Request changes** /
**Needs maintainer decision** — 2–3 sentences, next step.
```

A blocker breaks users, violates a cited canon sentence, or fails on
current main. When only the maintainer can make the scope call, say that
instead of inventing a verdict.
