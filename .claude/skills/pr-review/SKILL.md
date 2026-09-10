---
name: pr-review
description: Full adversarial PR review for AetherSDR — red-teams the PR against its linked issue, tests every claim in the body, audits scope creep and undisclosed changes, checks governance and code quality, posts one GitHub review with inline comments, and reports blockers/nits/recommendation as markdown. Use when asked to review a PR (e.g. "/pr-review 4609").
---

# PR Review (issue-fit + governance + quality)

Review the PR given in `$ARGUMENTS` (a PR number, or a full PR URL; if absent,
use the PR for the current branch via `gh pr view`). The deliverables are
**one posted GitHub review** (step 9 — inline comments, suggestions, and the
right review event) and **a markdown report to the operator** (step 10).
Post nothing else to GitHub — no labels, no extra comments, no merges.

After step 0 passes, work in parallel where the remaining steps are
independent. Do all repo reads through `gh`/`git show`, or check the PR out in a **scratch worktree** — never mutate
the checkout you were invoked in. Someone else may be working in it, and a
`git checkout` there retargets them silently. (Some setups make this a hard
rule in a local `CLAUDE.local.md`; treat it as one regardless.)

Where `gh` is unavailable — Claude Code Remote and web sessions have no `gh`
CLI — use the GitHub MCP tools (`mcp__github__*`) in its place throughout:
`pull_request_read` for `gh pr view` / `gh pr diff`, `issue_read` for
`gh issue view`, and `pull_request_review_write` +
`add_comment_to_pending_review` for the step 9 posting flow.

## Stance — adversarially red-team the PR

**Your job is to break the PR, not to bless it.** Assume the author is
competent and that the PR body is written to sound airtight; verify every
claim in it, and treat anything you cannot reproduce as a finding. A review
that ends "looks good to me" without naming what you tried and failed to
break is not a review — it is a signature.

Hold this posture for the whole pass:

- **The burden of proof is on the PR, not on you.** The author asserts; you
  falsify. "I see no problem" is not a conclusion — "I tried X, Y and Z and
  the code survived all three" is. Every clean verdict must name the attacks
  it survived.
- **Every sentence in the body is a claim to test.** "No behavior change",
  "pure refactor", "trivial", "cannot fail", "matches SmartSDR", "no impact
  on other radios", "existing tests cover this", "safe on all platforms" —
  each one is a hypothesis with a specific way to be wrong. Find the way, run
  it, and report the result either way. A claim you could not test is itself
  reportable: say which claim, and what would have settled it.
- **Reading the diff is not reviewing it.** Ask what input makes this code
  wrong: first/last/empty/zero/negative/overflow, null or dangling pointers,
  disconnect mid-operation, reentrancy, the callback firing during teardown,
  two threads, the error path nobody exercises, the second radio, the second
  slice, the second monitor. Then look for whether the diff handles it.
- **Read what is NOT in the diff.** The strongest findings are usually
  absences: the sibling call site left unfixed, the error return nobody
  checks, the migration path for existing users' saved state, the test that
  would have caught this. A diff can only show you what changed; you have to
  go get what didn't.
- **Attack the tests, not just the code.** A test that passes against the
  *unfixed* code proves nothing. Ask — and where feasible check — whether it
  fails when the fix is reverted or the guard is broken on purpose. Tests
  that assert the implementation back to itself, or that would pass with the
  function body deleted, are findings.
- **Polish is not evidence.** Clean formatting, a confident PR body, a
  thorough-looking test file, and a green CI badge are all cheap to produce
  and none of them are correctness. Where the presentation is most polished,
  spend *more* scrutiny, not less — that is where a wrong assumption hides
  longest.
- **Distrust green.** CI proves the filtered subset passed on some merge
  base, nothing more (see step 7). A bot's comment is a lead, not a finding.
  A previous approving review is not a reason to look less hard.
- **You can settle most runtime questions, so settle them.** This app is
  drivable: the automation bridge (step 8) will run the PR head against the
  built-in demo simulator, click the control, read back model state, and
  screenshot the panadapter — no hardware, no risk to the operator's station.
  A behavioral claim you argued about instead of reproducing is a claim you
  left untested.
- **Disagree with the framing when the framing is wrong.** The PR chooses the
  problem statement, the fix's shape, and where the seam goes. Any of those
  can be the actual defect — a correct implementation of the wrong change is
  still a finding.

The one thing adversarial does **not** mean: manufacturing findings. The
stance is a burden of proof, not a quota, and a PR that survives it earns a
clean report stated plainly and without hedging. Severity stays calibrated
(step 10), tone stays collegial and evidence-first, and every finding cites
what you actually observed — file:line, command output, failing scenario.
Break the code, not the author.

## 0. Test-boundary preflight — before parallel work

Complete this preflight before building or running any test. It overrides the
parallel-work instruction below, and it gates *executing* a socket-owning
target — it does not suspend the requirement to finish and post the review.

Read `AGENTS.md`'s "Test-layer boundary". Fetch the PR's own sources before
inspecting anything — `gh pr diff <PR>`, or `git fetch origin pull/<PR>/head`
then `git show FETCH_HEAD:<path>`; never read the working tree, which is the
base branch and will pass this preflight vacuously on a PR that does add a
socket test. Inspect the PR's added or modified test sources — not only
`tests.cmake` — for socket ownership or a synthetic peer. Look for
`QTcpServer`, `QTcpSocket`, `QUdpSocket`, `QLocalServer`,
`QWebSocketServer`, `bind()`, `listen()`, `connectToHost()`, peer processes,
and `Fake*` radio, amplifier, or tuner classes. Also check whether an existing
registered target has quietly gained network behavior. These are inspection
candidates, not proof by themselves; distinguish a socket object used only as
an inert value from a test that opens, binds, listens, or connects.

If a new or modified socket-based test is found, **stop before executing it**:

- Notify the operator with the test, socket type, target, and whether CI runs it.
- Do not build or run that target. Get explicit, PR-specific operator direction
  before you do.
- Continue the rest of the review and post it, recording the socket test and the
  notification in the review body. `AGENTS.md` requires the operator be notified
  *before continuing*, not that the review be abandoned — and an unattended run
  has nobody to supply the direction, so aborting there would mean the PR is
  never reviewed at all.
- A PR that **removes** a socket test or fake peer is the remediation #5254 asks
  for. Notify, then review it normally; do not treat it as a stop.

Classify acceptable coverage before requesting a regression test:

- Wire encoding, parsing, model tables, scheduling, DSP, capabilities, and
  safety policy: a socket-free CTest.
- Refusals, malformed or disconnected input, dropped messages, non-events, and
  TX guards: socket-free transport or state-machine injection.
- Race or lifetime behavior: the appropriate sanitizer lane.
- Positive session, RX, control, or meter convergence: the automation bridge
  plus `radiocert` against real firmware.
- A necessary simulator closed loop: explicit opt-in, never the default graph.

Do not request or accept a synthetic peer standing in for third-party radio,
amplifier, tuner, or other external-device firmware. Tests of AetherSDR's own
server surfaces (rigctld, CAT, the TCI server, the automation bridge transport)
are legitimate per `AGENTS.md`, but still trigger the notify rule. For those,
check the three obligations canon puts on them and report any that are missing:

- the test is disclosed in the PR body;
- its `tests.cmake` block names the socket it binds;
- it fails fast, or skips with **exit 77** (`SKIP_RETURN_CODE 77`), when it
  cannot bind — rather than consuming its timeout.

Bracket-commented, retired, or otherwise unexecuted tests do not count as
delivered regression coverage. Deliberately opt-in targets are different and do
count, provided the PR says how to run them: an `option()`-gated or
`EXCLUDE_FROM_ALL` target carrying the `# not registered: <reason>` marker is
the shape the bullet above routes a simulator closed loop to — see
`hl2_tx_loopback_test` in `tests/tests.cmake`.

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
- After completing the test-boundary preflight, check whether admissible
  coverage fails without the fix and passes with it. Prefer the smallest
  socket-free behavioral seam and require mutation evidence when practical.
- Missing coverage is a blocker only when the reported behavior has a
  deterministic, policy-compliant test seam or project canon explicitly makes
  that coverage merge-gating. If the only apparent approach is a synthetic
  firmware peer, do not request it; describe the honest coverage boundary and
  route positive convergence to bridge/`radiocert` evidence. Report the gap as
  a nit either way — an untestable gap is still worth naming, it just is not a
  reason to withhold a merge.
- Never count additions to a retired target, or to an unregistered target that
  lacks the `# not registered: <reason>` marker `AGENTS.md` recognizes, as
  coverage.
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

Read hostilely: for each non-trivial hunk, spend a moment constructing the
input, ordering, or lifecycle event that makes it misbehave before you accept
that it doesn't. Walk the failure paths as carefully as the happy path — what
the code does when the write fails, the pointer is stale, the radio drops
mid-call, or the user does it twice.

Verify at least one non-trivial claim empirically when feasible (build the PR
head in a scratch worktree, run the PR's own tests, and drive the result
through the automation bridge — step 8, which is the default verification
path for anything user-visible). Re-run the step-0 preflight against the exact
target before invoking it, prefer focused socket-free targets, and do not start
a broad suite merely to find something empirical to report. Prefer verifying
the claim the PR most depends on and would most like you to take on faith. Say in the report what
you verified vs. only read, and
say plainly when a load-bearing claim went unverified — an untested assertion
reported as untested is honest; one reported as fine is not. Never trust a
green CI badge over a local reproduction when they disagree.

Where the PR adds or changes a test, break the code on purpose and confirm
the test notices. A regression test that still passes with the fix reverted
is a blocker in its own right — it pins nothing, and it will read as coverage
forever after.

**"CI is green" is not "the suite passes."** Every `ctest` call in `ci.yml` is
`-R`-filtered to a handful of named tests, so only a few of the ~240 tests
gate a merge. If you run the suite locally, expect failures that have nothing
to do with the PR.

**Before blaming the PR for a test failure, prove it.** Build the PR's merge
base clean in a separate worktree and run the same test there. For a genuinely
intermittent, socket-free failure, compare repeated runs on each side using a
proportionate sample. For a bind, sandbox, permission, or unavailable-peer
failure, do not repeat it — but do check the merge base once before classifying
it. If the merge base fails the same way it is an environment or test-layer
boundary; stop under step 0 and say so. If only the PR head fails to bind, that
is a finding about the PR, not the sandbox — our own server surfaces regress
exactly that way. Report the comparison, not the impression.

## 8. Drive the app — verify findings against the running GUI

**Any claim about runtime behavior gets tested against the running app, not
argued from the diff.** UI state, a control's effect, a dialog's lifecycle,
what the panadapter actually renders, connect/disconnect and slice-recreate
paths, whether a setting survives a restart — the automation bridge can reach
all of it, so "I think this breaks X" is a hypothesis you are equipped to
settle. Drive it before you post. This is what turns a suspicion into a
finding a maintainer can act on, and what kills the plausible-but-wrong ones
before they reach the author.

`docs/automation-bridge.md` is the reference; it is written for agents and is
copy-pasteable.

### Demo mode, never the live radio

**Every bridge session in a review runs against the built-in demo simulator
(`SimBackend`, RFC #4288) — never the operator's FLEX-8600.** The demo is a
synthetic backend that generates its own RX audio and matching panadapter, it
exercises the same `RadioModel` / slice / pan / settings paths the GUI uses
with real hardware, and by construction it **cannot key** (Principle VI). It
is the correct target for a review: reproducible, no hardware contention, no
emissions, and nothing you do to it can disturb the operator's station.

The trap is that this is opt-*out*, not opt-in: `AutoConnectToLastRadio`
defaults on, and on the operator's machine the saved settings point at the
real radio — so a bridge instance launched with the default config **will
connect itself to the live FLEX** before you have issued a single verb. Two
things prevent it, and you do both:

1. **Isolate the settings store**, so there is no saved radio to autoconnect
   to. `AETHER_SETTINGS_DIR` is honoured at runtime (`SettingsPaths.cpp`);
   point it at a fresh scratch dir. A per-session scratch `HOME` /
   `XDG_CONFIG_HOME` does the same job. A clean store also means
   `ShowDemoRadio` is at its default (on), so the demo entry is in the list.
2. **Connect explicitly to the demo, and to nothing else.** Its discovery
   serial is `DEMO-0001` (`SimBackend::demoSerial()`), family `sim`, shown as
   "Simulator (not on the air)".

```sh
export SCRATCH=<your scratch dir>
AETHER_AUTOMATION=1 QT_QPA_PLATFORM=offscreen \
AETHER_SETTINGS_DIR="$SCRATCH/settings" \
AETHER_AUTOMATION_IDENTITY=pr-<PR>-review \
AETHER_AUTOMATION_SOCKET=aethersdr-pr<PR> \
AETHER_AUTOMATION_NO_TX=1 \
setsid nohup ./build/AetherSDR >"$SCRATCH/app.log" 2>&1 &
```

Then, before anything else, confirm where you are pointed and attach the demo:

```text
connect list                      → verify DEMO-0001 is offered
connect local serial DEMO-0001
connect wait 30000                → asserts connected, returns the radio block
get radio                         → confirm model "AetherSDR Demo", family sim
```

If `get radio` ever shows a FLEX serial, you are on the operator's hardware:
`disconnect` immediately, fix the isolation, and say so in the report.

The demo also gives you failure paths that live hardware will not: the `sim`
verb injects faults (`swr`, `dropslice`, `stallscope`, `disconnect`,
`malformed`, `clear`) while it is connected — that is how you exercise the
error handling in step 7 rather than only reading it.

Its limits, so you don't mistake one for a finding: it advertises a **single
slice**, it cannot transmit, and it does not implement every Flex-specific
protocol surface. When a PR's behavior genuinely cannot be reached in demo
mode, that is a legitimate "could not verify" — report it as one (see the end
of this step). It is **not** a licence to reach for the real radio; using
live hardware needs the operator's explicit go-ahead in that review, and even
then never for TX.

### Driving it

With the demo attached, use the `aethersdr-automation` MCP tools:
`bridge_status` →
`dump_tree filter=<widget>` → `invoke` / `gesture` / `shortcut` / `tune` /
`slice` / `menu` → `assert_state` / `wait_for` on the model property that
should have changed → `grab_widget` when the claim is visual. `bridge_command`
is the escape hatch for verbs without a typed tool (`hover`, `clickAt`,
`contextMenu`, `layout`, `tci`, …). If MCP is unavailable, `python3
tools/automation_probe.py` or raw line-delimited JSON over the socket does the
same job — the bridge being awkward to reach is not a reason to skip it.

The loop that produces evidence:

1. **Reproduce on the merge base first** when the PR claims a fix. A "before"
   you cannot make fail is itself a finding — either the repro is wrong or the
   fix is fixing nothing. Same worktree layout, same steps, different binary.
2. **Re-run identically on the PR head.** Prefer `assert_state`/`wait_for` —
   they read model truth and report pass/fail — over eyeballing a screenshot.
   Grab pixels when the claim is about pixels.
3. **Then attack past the happy path**, which is the part the author almost
   certainly did not drive: the second slice, the second pan, disconnect
   mid-operation, the dialog closed and reopened, the value at its range
   limits, the same action twice. Persistence claims are only proven across a
   process boundary — relaunch and re-read, don't trust the in-session value.
4. **Quote what the app returned** — the `get_state`/`assert_state` JSON, the
   `get_log` lines, the PNG — in the finding. Bridge output is the strongest
   evidence available in a review; use it verbatim rather than paraphrasing.

Use it on the body's runtime claims too, not only on your own suspicions.
"No behavior change", "the panel still remembers its geometry", "other radios
are unaffected", "the shortcut still works" are all one bridge session away
from being confirmed or refuted.

Non-negotiable operating rules:

- **`pgrep -a AetherSDR` first.** Instances that are not yours — especially
  any launched from the shared checkout — are the operator's session. Never
  drive, close, or kill one.
- **Always pass an explicit socket.** The discovery file
  `<temp>/aethersdr-automation.json` is last-writer-wins and will happily
  point you at another agent's instance.
- **Never key TX.** The transmit verbs stay gated behind
  `AETHER_AUTOMATION_ALLOW_TX`; a review never needs them. Launch with
  `AETHER_AUTOMATION_NO_TX=1` (or via the MCP `app_instance` tool, which pins
  it) so the gate is off regardless of the saved preference.
- **Demo mode, offscreen, isolated settings — always.** Never point a review
  instance at the FLEX-8600. It is single-occupancy and it is the operator's
  station, not test equipment; a review has no business on it. If a finding
  truly cannot be reached in demo mode, ask the operator rather than
  connecting.
- **Close your instance when done**, and say in the report which instance you
  drove and that it was the demo.

When you genuinely cannot drive it — the change is headless, the path needs
hardware you must not touch, the build fails — say so explicitly and label the
affected findings as reasoned-from-code rather than reproduced. An unverified
finding reported as unverified is honest; one reported as observed is not.

## 9. Post the review to the PR

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
  read, and — briefly — what you tried to break that held up, so the author
  can see the review was adversarial rather than cursory, and can correct you
  if you attacked the wrong thing. Where a finding came from driving the app,
  paste the bridge evidence with it: the state JSON, the log line, or the
  `grab_widget` PNG (attach images by URL in the comment body). A reproduced
  finding with app output attached rarely gets argued with. Anchor each
  out-of-scope finding inline on the file it concerns — line 1 of an added
  file is a valid anchor — so the author sees it where the change is, not only
  in the summary.
- If the posting API call fails (e.g. an anchor line is not in the diff),
  fix the anchors and retry once; if it still fails, fall back to
  `gh pr review` with the full markdown body and say so in the report.
- Branch protection has `dismiss_stale_reviews` enabled: **any push dismisses
  an existing approval.** If you are also pushing commits, approve *after*
  the last push, and re-approve after any later merge-from-main. Check
  `reviewDecision` afterwards rather than assuming it stuck.

## 10. Report (markdown, to the operator only)

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

### What I tried to break
The attacks that did NOT produce a finding — the claims from the body you
tested and that held, the edge cases and failure paths you walked, the tests
you inverted, what you built or ran, and **the bridge session**: which build
you drove against the demo, the verbs/tools you used, and the state you
asserted. Two to five
bullets. This is what makes a clean review trustworthy; without it, "no
blockers" and "I did not look" are indistinguishable. Name anything you could
not test and why — including "did not drive the app because …".

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
