---
name: pr-land
description: Post-review remediation pass for AetherSDR — takes a PR whose proposed change the maintainer has already accepted, fixes every outstanding finding with proof, answers and resolves every review thread, dismisses blocking reviews with evidence, posts the approving review and arms auto-merge. Every judgment call goes to the maintainer through AskUserQuestion. Use after /pr-review, e.g. "/pr-land 5601".
---

# Land the PR — remediate, resolve, approve, arm

Work the PR given in `$ARGUMENTS` (a number or a full URL; if absent, the PR
for the current branch via `gh pr view`). Seven deliverables, in this order:

1. **One push** that resolves every outstanding finding, each one proved.
2. **Every review thread answered and resolved** — evidence in the reply.
3. **Every blocking review dismissed** with the evidence that retires it.
4. **One approving review**, posted after the last push.
5. **Auto-merge armed and confirmed able to fire** — not merely requested.
6. **A markdown report to the operator** (step 12).
7. **`main` green on the squash-merge commit** (step 13) — the check that
   actually demonstrates the fix, and the one a PR-branch pass never does.

Where `gh` is unavailable — Claude Code Remote and web sessions have no `gh`
CLI — use the GitHub MCP tools (`mcp__github__*`) throughout. Everything below
that uses `gh api graphql` has no MCP equivalent; say so in the report rather
than skipping the thread sweep.

## Premise — the change is approved, the code is not yet mergeable

The maintainer has already accepted **what this PR sets out to do**. You are
not re-litigating the premise, the problem statement, or the shape of the fix;
that ruling is made and re-opening it wastes the pass. `/pr-review`'s
adversarial stance toward the PR's *reason to exist* does not carry over here.

What is **not** approved is the code as it currently stands. Hold this posture:

- **You are the last reader before the merge button.** There is no second
  reviewer behind you. Auto-merge fires unattended — possibly hours later,
  possibly while CI is the only thing watching. Everything upstream of the
  arming step is verified, not assumed.
- **Principle VIII (Evidence Over Assertion) and XI (Fixes Are Demonstrated)
  govern this skill.** "Fixed" is not a disposition. "Fixed in `<sha>`,
  `<file>:<line>`, proved by `<test name + output>` / `<bridge state JSON>`"
  is. Every thread reply, every dismissal message and the approving review
  carry that shape or they do not get posted. XI also sets the ceiling on
  what your own evidence is worth: it names the demonstration as CI re-running
  on the **squash-merge commit on `main`**, not on the PR branch, and says in
  as many words that agent self-grading does not substitute for that
  independent re-run. Everything you prove in steps 5 and 6 is the hypothesis.
  Step 13 is the check, and the pass is not over until it passes.
- **Completeness over deferral.** Nothing is left "for the author". Nothing
  becomes a follow-up issue in order to avoid making a call. If a finding
  needs a decision, the decision goes to the maintainer (next section) — not
  into a TODO, not into a deferred-to-later paragraph, not into your own
  judgment.
- **Silence is not resolution.** A thread resolved without a reply is a
  finding you hid. A blocking review dismissed without evidence is a finding
  you overrode.
- **The approving review is a claim you are accountable for.** It asserts:
  every finding is addressed, each fix is demonstrated, and nothing I changed
  is unproven. Do not post it while any part of that is untrue.
- **Your own fixes get the adversarial treatment the PR's did.** The code you
  push in this pass has had no review at all. Attack it the way `/pr-review`
  attacks a diff — the second slice, the error path, disconnect mid-operation,
  the value at its range limit, the call site you did not change.

## The decision gate — ask, never guess

**Every decision that has a real alternative goes to the maintainer through
the `AskUserQuestion` tool.** Not a prose list of options, not a question at
the end of the report, not a choice made quietly and mentioned afterwards.
The bar is: *would a different answer have changed the work?* If yes, ask.

### Always ask

- **Anything inside `AGENTS.md` § "Autonomous Agent Boundaries"** — visual
  design (colours, fonts, layout, theme), UX behaviour (how a control works,
  what a click does, keyboard shortcuts), architecture (new threads, changed
  signal routing, a new dependency), feature scope, or a **default value**.
  The maintainer is the sole authority on these; an agent may implement them,
  never choose them.
- **Two or more defensible fixes** for the same finding with different
  trade-offs. Give each option a `preview` with the concrete code shape —
  the enum, the signature, the guard — so the choice is made by looking, not
  by imagining.
- **A finding you intend to decline.** If the refutation is reproducible
  evidence (the reviewer read a stale line; the test proves the opposite; the
  claimed path does not exist), refute it in the thread and move on — that is
  not a decision. If it rests on judgment ("this is fine in practice", "that
  edge case cannot happen here"), it is a decision: ask.
- **A fix whose blast radius exceeds the PR.** A sibling call site, a shared
  helper, another radio family, a second dialog with the same bug. Fold it in
  or file it? That is the maintainer's scope call, and this is the moment to
  put it to them.
- **Any CI workflow change.** Never fold an edit to `.github/workflows/` into
  this push — not even adding a new test to a gate regex. What runs in a merge
  gate changes cost and flake exposure for every PR. Make the code and test
  change, then ask.
- **Anything that would *loosen* an aetherd ratchet** — growing a baseline
  count in `tools/check_engine_boundary.py` or
  `tools/check_command_plane.py`, adding a stem or a row to
  `KNOWN_VENDOR_INCLUDE_BASELINE`, raising `FROZEN_BOOL_COUNT`, or retagging a
  header in `docs/architecture/aetherd-touchpoint-tags.json` — and **any new
  `gui/`→engine touchpoint**. See the prohibition in step 4. A **reduction**
  is not this: dropping a stem whose coupling the push actually removed,
  lowering a freeze count, deleting an emptied row — canon demands those of a
  conversion, so do them and say so in one line.
- **Restructuring toward the aetherd RFC.** `AGENTS.md` is explicit: do not
  pre-emptively restructure code toward it — no new engine/UI seams, no
  backend interfaces, no speculative library targets — and architecture ahead
  of the RFC's staged order is maintainer-only. A remediation pass helpfully
  adding a seam is exactly how the migration gets set back.
- **Anything only the live FLEX-8600 could verify.** Never connect to it on
  your own initiative; the demo simulator is the target (step 6).
- **History rewrite on someone else's branch** — squash, rebase, amend,
  reordering, anything beyond appending commits — and any non-trivial
  conflict resolution against `main`.
- **A blocking review whose request the maintainer's approval overrules.**
  You are about to dismiss a human's objection on the strength of a decision
  they were not party to. Confirm it, and say in the dismissal that it was a
  maintainer call rather than implying the concern evaporated.
- **CI still red after one re-run** for reasons unrelated to the PR, and
  **any finding that suggests the PR should be split** or is a bigger change
  than it looked.

### Do not ask — do it, and say so in one line

Mechanical and obvious work is not a decision: typos, a missing null check, a
missing `const`, dead code, naming that matches the file's convention, an
accessible name on a new widget, using the helper that already exists, adding
the regression test the review asked for, committing and pushing (standing
authorization), running the automated `code-review` pass (standing
authorization). Conventional defaults with one sensible answer are not
decisions either. Applying the gate to trivia is its own failure mode — it
turns a cleanup pass into an interrogation.

### When the tool is not there

`AskUserQuestion` is unavailable in headless, cron and background runs, and an
"Always ask" item reached there leaves no legal move: guessing is forbidden and
stalling strands a dirty worktree and a half-swept thread list. **Stop before
the push.** Leave the branch untouched, post nothing to GitHub, and deliver the
open decisions as the report — each with the options, the trade-offs and your
recommendation — so the next interactive run starts from a decision list rather
than re-deriving one. An unmade decision handed back is a finished pass; an
unmade decision guessed at is not.

### How to ask

Do **everything that does not depend on the answer first**, then batch the
open decisions into as few `AskUserQuestion` calls as possible (up to four
questions per call). Recommended option first, labelled `(Recommended)`. Each
option says what it means and what it costs. Then implement the answer and
carry on — the pass is not finished until every answered decision is in the
push.

## 0. Preflight — claim, isolate, anchor

Do all of this before touching code. Each item has cost a real merge.

- **Claim the PR.** `gh pr view <PR> --json assignees`; if unassigned or
  assigned only to `@aethersdr-agent`, `gh pr edit <PR> --add-assignee @me`.
  If another non-AetherClaude agent holds it, leave a coordination comment and
  stop (AGENTS.md § Issue / PR Claim Protocol, Principle X).
- **Anchor to the head SHA.** `gh pr view <PR> --json headRefOid` — record it.
  Everything you read, build and prove is against this SHA, and you re-check
  it before posting anything (steps 9, 10).
- **Establish where the head branch lives.**
  `gh api repos/aethersdr/AetherSDR/pulls/<PR> --jq '{fork: .head.repo.full_name, branch: .head.ref, modify: .maintainer_can_modify, draft: .draft}'`
  Most contributor PRs live on a fork. `git push origin HEAD:<branch>`
  **silently creates a stray branch on the org repo and does not move the
  PR** — push by the contributor's configured remote name instead, and
  `git ls-remote <remote> refs/heads/<branch>` first. If
  `maintainer_can_modify` is false you cannot push at all: ask.
- **Check who owns the PR.** `gh api user --jq .login` against
  `.author.login`. **GitHub refuses an approving review on your own PR**, so
  if they match, the step-10 approval is impossible and `required_approving_
  review_count: 1` will hold auto-merge at `BLOCKED` forever. Surface that at
  step 10 as a decision (admin-merge / wait for a core dev / leave armed), and
  know it now so nothing later is a surprise.
- **Read the live protection rules** rather than trusting this file:
  `gh api repos/aethersdr/AetherSDR/branches/main/protection`. At the time of
  writing: required contexts `build`, `check-windows`, `check-macos`,
  `Static checks`; `strict: false`; `required_signatures: true`;
  `required_conversation_resolution: true`; `dismiss_stale_reviews: true`;
  `require_last_push_approval: false`; `required_approving_review_count: 1`
  with CODEOWNERS review required; `enforce_admins: false`. Two of those set
  the order of this skill and are the most common way it fails silently —
  see step 10.
- **Know which GitHub identity you are acting as** — `gh api user --jq .login`.
  Everything this skill posts is attributed to it, and step 10 only works if
  it is a **code owner** for the paths the PR touches: `main` requires a
  CODEOWNERS approval, and `GOVERNANCE.md` § AI Contributors puts
  `@AetherClaude` deliberately outside every code-owner team precisely so a
  bot cannot approve its own work or another agent's. Run under a code-owner
  identity (the operator's) and the approval counts. Run under the bot's, and
  it never can: do the remediation, resolve the threads, and hand the approval
  to a human code owner rather than posting one that cannot satisfy the gate.
- **Work in a fresh worktree, never in the invoking checkout.** Someone else
  may be working there and a checkout retargets them mid-task. Build there
  too. Never `git stash` in this repo — the stash stack is shared across every
  worktree and will eat another agent's work.
- **Confirm you can sign.** `main` has `required_signatures: true`; an
  unsigned or mis-attributed commit blocks the merge with CI green and an
  approval in place. Commit with the checkout's configured identity — never
  pass `-c user.email=` / `-c user.name=`, and never the harness-supplied
  address, which differs and produces `verified=false, reason=bad_email`. If
  `gpg-connect-agent 'keyinfo --list' /bye` shows no cached passphrase, a
  plain `git commit` will block on a pinentry the agent cannot answer: ask the
  operator to unlock before you have a dirty tree waiting on it.

## 1. Inventory — everything that must be answered

The one-line version of this task ("fix all issues, resolve all comments") is
only as good as the list. Build it exhaustively; a finding you never collected
is a finding you will approve over.

**Review threads** — the ones that block the merge. Paginate; a `first: 20`
query silently truncates a busy PR:

```sh
gh api graphql --paginate -f query='
query($owner:String!,$repo:String!,$pr:Int!,$endCursor:String){
  repository(owner:$owner,name:$repo){ pullRequest(number:$pr){
    reviewThreads(first:100, after:$endCursor){
      pageInfo{ hasNextPage endCursor }
      nodes{ id isResolved isOutdated path line
        comments(first:100){ nodes{ author{login} body url } } } } } }
}' -F owner=aethersdr -F repo=AetherSDR -F pr=<PR>
```

Collect **every thread with `isResolved: false`**, including ones marked
`isOutdated` (the code moved under them — they still block) and ones the
author labelled "nit, non-blocking" (they still block; see step 11).

**Reviews** — `gh api repos/aethersdr/AetherSDR/pulls/<PR>/reviews --paginate
--jq '.[] | {id, user: .user.login, state, submitted_at}'`. Only the latest
review per reviewer counts toward `reviewDecision`; note every
`CHANGES_REQUESTED` and the body text of each, which often carries findings
that never became inline threads.

**Top-level comments** — `gh pr view <PR> --json comments`. These do not block
the merge, so they are the ones that get skipped; bot findings and maintainer
asides live here.

**CI** — `gh pr view <PR> --json statusCheckRollup`. Every failing check is an
item. Read the workflow trigger before concluding anything: a `pull_request`
trigger tests the merge result, a `push`-triggered check on the branch does
not include current main.

**The prior review** — if `/pr-review` ran in this session, its blockers and
nits are items. If it did not, read the posted review bodies; do not assume
the inline threads are the whole of it.

## 2. Fresh adversarial pass

The inventory is what others found. This step is what they missed, and it is
the reason this pass exists rather than a checklist of replies.

- **Run the automated pass.** `code-review` is a harness-provided skill, not
  one of this repo's — `.claude/skills/` holds only `pr-review` and `pr-land`
  — so its availability varies by build. Where it is model-invocable, run it
  at medium effort against the PR; standing authorization covers that, so do
  not skip it and do not ask. Fold in only findings you verify yourself and
  drop anything you can refute with evidence. Where it is absent or marked
  `disable-model-invocation`, do the equivalent pass by hand and say in the
  report that the automated pass was unavailable — never imply it ran. The
  `ultra` variant is user-triggered and billed: never launch it yourself.
- **Re-read the diff for what is not in it.** The sibling call site, the error
  return nobody checks, the migration path for existing users' saved state,
  the second slice, the second radio, the test that would have caught this.
- **Audit governance on the current head**, because the head has moved since
  the review: `CONSTITUTION.md` (I FlexLib authority — and the HL2 gateware
  RTL as its analogue; II/III radio-authoritative state and capability-shaped
  persistence; V feature-owned config; VI TX safety), `AGENTS.md` settings
  persistence, credentials policy, capability declarations, the CMake settings
  contract, `docs/style/theme-style-guide.md` (every colour through a
  ThemeManager token), `docs/a11y.md`, test registration in `tests/tests.cmake`.
  Quote the sentence in canon or it is a nit, not a blocker.
- **Audit the aetherd migration ratchets**, which that list does not reach.
  Read `AGENTS.md` § "In-flight: aetherd engine/UI decoupling" on the head you
  are landing — the whole section, because it changes as the RFC's staged
  order advances — and audit the diff against it: EB1/EB2/EB3, the
  build-target link rules, the capability-record and command-plane freezes,
  the routing table for where radio-facing code goes, and the THREADING AND
  LIFETIME CONTRACT at the top of `IRadioBackend.h`. This skill does not
  restate any of that; the section below names only what reading it will not
  tell you.
- **`CHANGELOG.md` is release-prep only.** Never add an entry in this push,
  and if the PR added one, removing it is one of your fixes.
- **Attack the tests.** A test that would pass against the unfixed code proves
  nothing. Where feasible, break the guard on purpose and confirm the test
  goes red.

### The ways an aetherd regression reaches `main` on a green run

`Static checks` is a required context, so a red gate blocks the merge. That is
not the same as the gates catching a regression, and each of these has a green
run behind it.

**A finding inside a tracked baseline only warns.** EB2/EB3 findings against
tracked files are warnings — about a hundred of them ride on a green run
today — and only a new violation or a grown baseline errors. So run the gates
yourself, against the **merge base and against your head**, and diff the
per-file findings instead of reading exit codes. They are stdlib Python and
take seconds:

```sh
python tools/check_engine_boundary.py --strict
python tools/gen_touchpoint_manifest.py --check
python tools/check_capability_records.py --strict
python tools/check_command_plane.py --strict
```

EB2 is a per-file **count**, not a set: a lateral swap inside a tracked file —
drop one QtWidgets usage, add another — keeps the number flat and passes. Read
the file's findings, not its total.

**A new `gui/`→engine include stops at "regenerate", not at "justify".** The
manifest records per-header includer counts, so a new include makes the table
stale and `gen_touchpoint_manifest.py --check` *does* go red inside the
required context. That is the whole of what it asks: regenerate with
`python tools/gen_touchpoint_manifest.py`, commit the result, and the grown
burndown is green with nothing anywhere flagging that a touchpoint was added.
So diff the manifest itself, merge base against head — a new row, or a row
whose includer count went up, is a finding: name the header and the includer
and put it to the maintainer. And regenerate it, never repair it by hand. The
table is generated and says so in its first line; editing it to match, or
adding a tag so it matches, falsifies the burndown rather than fixing
anything.

**The seam contract has no test behind most of it.** `IRadioBackend`'s rules
are pinned for the simulator and for HL2 re-entrancy; live seam emission for
flex, anan, icom and rtl is a *survey result*, so a violation there is caught
by reading and by nothing else. If your push touches a backend, read the
contract text against the diff, and where a test drives a backend, carry
`tests/SeamThreadAffinityProbe.h` into it — that is the proof artifact for
this class of fix. The neighbouring trap no checker sees:
`IRadioBackend::audioFrameReady` has two routes to
`AudioEngine::feedPcmFrame`, so an in-process backend double-feeds the sink
unless one of them is gated. **The two gates have opposite senses and are
deliberately named apart.** The relay's is
`MainWindow::backendFeedsEngineDirectly()` (`dynamic_cast<SimBackend*>`); the
direct connect in `wireBackendSeam()` is the site whose gate belongs on
"does this backend own its RX audio". Do not merge them: an HL2 is
`ownsRxAudio() == true` **and** needs the relay, so delegating the relay to
`backend()->ownsRxAudio()` swallows every HL2 frame and silences the speaker —
`MainWindow_Session.cpp` has a paragraph at the function asking you not to
make exactly that substitution. `Qt::UniqueConnection` protects neither site:
at the relay they are two different signals arriving at one slot, and at
`wireBackendSeam()` it cannot catch a lambda connect. The spectrum side had
the same shape and was resolved by one producer and one path through `panFeed`
rather than by a gate.

**The #5554 items are mostly invisible to CI.** The notice at the top of
`AGENTS.md` § "AI Agent Guidelines" gates any change touching
`src/core/backends/`, `RadioModel`, `RadioSession`, `TransmitModel`,
`ConnectionPanel`, discovery or `RadioCapabilities` — which is most of what
this skill pushes into. Check the diff adds to none of its items: no new
`usesFlexCommandPlane()` or family-string branch, no new raw Flex wire text
above the seam, no new `dynamic_cast` to a concrete backend, no new capability
without a verb behind it, no HL2 scaffolding copied into another host-DSP
family, no keying-class verb that skips `RadioModel`'s TX gate.

**If the PR touches `src/aetherd/` or `RadioResourceAdapter`**, that same
AGENTS.md section carries the protocol audit: new resource fields belong in
the adapter and the versioned catalogue, never in a transport and never via
QObject reflection; the local transport defaults to observe permission and
`--allow-local-control` grants non-TX control only; admission is fail-closed
on TX-idle; and **no protocol TX method is advertised before the RFC's
step-4 arbiter exists** — the aetherd-shaped half of Principle VI, which the
app-level TX rules in step 4 of this skill do not cover.

Everything found here joins the inventory and is worked in this pass — subject
to the decision gate, which is exactly where a new finding that reopens a
design question belongs.

## 3. Triage — one disposition per item, no third bucket

Write the inventory out with a disposition each:

- **Fix** — mechanical, or the decision is already made. Just do it.
- **Decide** — goes into the `AskUserQuestion` batch. Do the independent work
  while it waits.
- **Refute** — invalid on reproducible evidence; the evidence goes in the
  thread reply.

There is no "defer", no "leave for the author", no "file a follow-up" unless
the maintainer picked that option. Anything you cannot place in one of the
three buckets is a decision — ask.

## 4. Fix

Standing prohibitions, each of which has cost something real:

- **No `.github/workflows/` edits.** Ask instead (decision gate).
- **No `CHANGELOG.md` entry.**
- **No TX keying, ever**, and no automation into a keyed transmitter
  (Principle VI). TX verbs stay behind `AETHER_AUTOMATION_ALLOW_TX`.
- **No scope growth past what the maintainer approved** without asking.
- **Never loosen an aetherd ratchet to get green** — and know which direction
  is which, because one of them is required of you. The baselines in
  `tools/check_engine_boundary.py` (`KNOWN_VENDOR_INCLUDE_BASELINE` and the
  `KNOWN_WIDGETS_LEGACY` EB2 counts), `FROZEN_BOOL_COUNT` in
  `tools/check_capability_records.py`, the per-file baseline in
  `tools/check_command_plane.py`, and the tags in
  `docs/architecture/aetherd-touchpoint-tags.json` **are** the enforcement,
  not paperwork in front of it.
  - **Shrinking is conformance.** Drop the stem whose vendor include the push
    actually removed and delete the row when it empties; lower
    `FROZEN_BOOL_COUNT` when a bool became a record — the checker prints the
    number to lower it to; lower a converted file's command-plane count. Canon
    demands each of these of the PR that does the conversion. Do them, prove
    the coupling is gone, and note it in one line.
  - **Growing is the prohibition.** A larger count, a new stem or row, a
    raised `FROZEN_BOOL_COUNT`, a retagged header. AGENTS.md's rule is to
    restructure the change — not to move the file, weaken the check, or add an
    exemption. The tags file is the sharpest edge, because EB3 derives its
    vendor vocabulary from it at runtime: retagging a `vendor(...)` header as
    `mixed(...)` or `peripheral(...)` un-gates it for every file above the
    seam. `VENDOR_STEMS_PINNED` now catches that as a blocking `EB3-load`
    naming the stem — which makes it a design conversation, not a pin to
    edit your way past. Canon allows
    exactly two such moves — an EB3 vocabulary reclassification proved against
    the merge base with documented evidence and an explicit maintainer review,
    and a `FROZEN_BOOL_COUNT` raise on a maintainer ruling — and both are
    decisions carrying that evidence through `AskUserQuestion`. Neither is ever
    a way to make a check pass.
- **No revert commits and no "we used to do X" notes.** When something is
  dropped, the branch must read as if it never existed — squash the removal
  into the commit that introduced it rather than committing a revert on top.
  Same for the PR body and the report.
- **New tests register in `tests/tests.cmake`**, never in `ci.yml` and never
  in the root `CMakeLists.txt`. A test not being in a CI gate regex is not a
  coverage gap — the sanitizer sweep runs the unfiltered tree.
- **A socket-owning test needs operator direction before it is executed**
  (`AGENTS.md` § Test-layer boundary). Notify, do not run it, continue.

## 5. Prove each fix

A fix without evidence is not landable, and the evidence is what you paste
into the thread reply and the dismissal. Per item:

- **Build and run the tests** in your worktree — every commit, not just the
  last. `cmake --build build --parallel`, which is portable; drop to an
  explicit lower job count when `pgrep -lf ninja` shows another build already
  running.
- **Run the static gates before the push**, merge base versus head (step 2).
  A boundary regression caught here costs a rebuild; caught by `Static checks`
  it costs a CI cycle; inside a tracked baseline it is not caught at all.
- **Name the test that fails without the fix.** For any behaviour change,
  the strongest artifact is the test that goes red when you revert the fix and
  green with it. Run it both ways where the fix is small enough to invert.
- **Drive the app for runtime claims** (step 6) rather than arguing them from
  the diff.
- **Quote what you observed** — ctest output, the `assert_state` JSON, the log
  line, the screenshot. Paraphrase is not evidence.

## 6. Drive the app — demo simulator only

Any claim about UI state, a control's effect, a dialog's lifecycle, what the
panadapter renders, connect/disconnect and slice-recreate paths, or whether a
setting survives a restart gets tested against the running app.
`docs/automation-bridge.md` is the reference.

**Every bridge session runs against the built-in demo simulator
(`SimBackend`), never the operator's FLEX-8600.** This is opt-*out*:
`AutoConnectToLastRadio` defaults on, and the operator's saved settings point
at the real radio, so a default-config instance connects itself to live
hardware before you issue a verb. Two guards, both mandatory — isolate the
settings store (`AETHER_SETTINGS_DIR` at a fresh scratch dir, or a scratch
`HOME`/`XDG_CONFIG_HOME`), and connect explicitly to `DEMO-0001` and nothing
else. If `get radio` ever shows a FLEX serial, `disconnect` immediately, fix
the isolation, and say so in the report.

```sh
export SCRATCH=<your scratch dir>
AETHER_AUTOMATION=1 QT_QPA_PLATFORM=offscreen \
AETHER_SETTINGS_DIR="$SCRATCH/settings" \
AETHER_AUTOMATION_IDENTITY=pr-<PR>-land \
AETHER_AUTOMATION_SOCKET=aethersdr-pr<PR> \
AETHER_AUTOMATION_NO_TX=1 \
nohup ./build/AetherSDR >"$SCRATCH/app.log" 2>&1 &
```

Detach so a shell exit cannot `SIGHUP` the instance — prefix `setsid` on Linux,
where it exists. On macOS the binary is inside the bundle,
`./build/AetherSDR.app/Contents/MacOS/AetherSDR`; `docs/automation-bridge.md`
gives both paths.

`pgrep -lf AetherSDR` **first** — `-lf` is the portable spelling, since BSD and
macOS read a bare `-a` as "include ancestors" and print no command line to
judge by. Instances that are not yours are the
operator's session; never drive, close or kill one. Always pass an explicit
socket; the discovery file is last-writer-wins and will point at another
agent's instance. The `sim` verb injects faults (`swr`, `dropslice`,
`stallscope`, `disconnect`, `malformed`) so error paths get exercised rather
than read. Close your instance when done and say which one you drove.

Persistence claims are only proven across a process boundary: relaunch and
re-read, never trust the in-session value.

## 7. Commit and push

- **One push where possible.** Each push restarts CI on three platforms; a
  drip of six pushes is six CI cycles and six chances to race your own thread
  resolutions.
- **Signed, with the checkout's configured identity** (step 0). Commit
  subjects end with the most load-bearing principle: `Principle <N>.`
- **Push by the contributor's remote name** with
  `--force-with-lease=refs/heads/<branch>:<expected-sha>`. The lease is what
  catches a wrong-fork push instead of clobbering an unrelated contributor.
  Never `git push origin HEAD:<branch>` on a fork PR.
- **Verify the commits are verified** before going further — one bad-email
  commit blocks the merge with everything else green:
  `gh api repos/aethersdr/AetherSDR/pulls/<PR>/commits --jq '.[] | "\(.sha[0:9]) \(.commit.verification.verified) \(.commit.verification.reason)"'`
- **Re-read the PR body.** The repo squash-merges with `PR_BODY` as the
  commit message, so the body becomes permanent history on `main`. If your
  fixes changed what the PR does, update it to state the current change —
  not a "review feedback addressed" changelog of the churn. Keep the
  `Principle <N>.` citation in the title if you retitle.
- If the fork's workflows are awaiting approval, approve the run:
  `gh api -X POST repos/aethersdr/AetherSDR/actions/runs/<run_id>/approve`.

## 8. Answer and resolve every thread

**Reply first, resolve second — never resolve silently.** The reply is where
the evidence lives, and a bare resolve reads to the reviewer as their comment
being swept away.

Each reply carries: what changed (`file:line`), the commit SHA, and the proof
(test name and result, bridge output, or the reasoning plus why it could not
be tested). For a refuted item, the reply carries the evidence that refutes it
and stays collegial — you are correcting the claim, not the person.

```sh
gh api graphql -f query='mutation($tid:ID!,$body:String!){
  addPullRequestReviewThreadReply(input:{pullRequestReviewThreadId:$tid, body:$body}){comment{id}}}' \
  -f tid=PRRT_... -f body="$(cat reply.md)"
gh api graphql -f query='mutation($tid:ID!){
  resolveReviewThread(input:{threadId:$tid}){thread{isResolved}}}' -f tid=PRRT_...
```

Resolve **after** the push, so the reply can cite the SHA. Then re-run the
step-1 thread query: threads arrive while you work, and a new one blocks the
merge exactly like an old one. Answer top-level bot and reviewer comments that
carried findings too — they do not block, but leaving a bot's valid finding
unanswered is how it gets rediscovered on `main`.

## 9. Dismiss blocking reviews with evidence

For each review still in `CHANGES_REQUESTED`:

**Never dismiss a review whose concern you did not fix.** That single act is
what would turn this skill into a liability — it retires a human's objection
and nothing downstream will catch it. If the concern is unfixed, or fixed in a
way the reviewer might not accept, it is a decision: ask.

Reply in the reviewer's threads first (step 8) so they see the evidence, then:

```sh
gh api -X PUT repos/aethersdr/AetherSDR/pulls/<PR>/reviews/<REVIEW_ID>/dismissals \
  -f message="$(cat dismissal.md)" -f event=DISMISS
```

The dismissal message names each requested change, the commit that fixed it,
and the proof — the same standard as the thread replies. "Addressed" alone is
not a dismissal message. Where the maintainer overruled the request rather
than the fix satisfying it, say that plainly and attribute it.

Confirm afterwards: `gh pr view <PR> --json reviewDecision` should no longer
read `CHANGES_REQUESTED`.

## 10. Approve — after the last push, at the real head

**The authority for this step is the invocation itself.** `pr-review` leaves
approval to the operator "unless they have said otherwise for this PR";
running `/pr-land` on a named PR **is** that otherwise, for that PR, and is
what distinguishes this skill from a review. It does not generalise: it
authorises the approving review on the PR you were pointed at, and on no
other.

It authorises the *review*, not an identity. The approval must be posted by a
code owner for the paths the PR touches or it cannot satisfy `main`'s
CODEOWNERS requirement, and `GOVERNANCE.md` § AI Contributors keeps
`@AetherClaude` outside every code-owner team so that a bot can never approve
its own work or another agent's. Under a code-owner identity the approval
counts regardless of who authored the PR — a human code owner approving a
bot-authored PR is exactly what that section prescribes. Under a
non-code-owner identity nothing here makes an approval valid: finish the
remediation, resolve the threads, and hand the approval off (step 11's
`BLOCKED` fourth cause).

Three things decide the order here, and all three fail silently:

- **`dismiss_stale_reviews: true`** — any push dismisses an existing
  approval. Approve **after** the final push, and re-approve after any later
  merge-from-main. (`require_last_push_approval: false`, so your own approval
  of a branch you pushed to is valid.)
- **The head may have moved while you worked.** Re-run
  `gh pr view <PR> --json headRefOid` and compare it to the SHA you built and
  proved against. GitHub attaches a review to whatever head exists **at post
  time** — a mid-pass push by the author makes your review describe a tree you
  never read. If it moved, diff `proved..current`, re-verify which findings
  survive, and work the real head before approving.
- **`main` may have moved since the head's checks ran** — the one below.

### Is the green still current? (check every time, before approving)

**A green check proves the merge result was sound WHEN IT RAN, not now.** The
required checks are `pull_request`-triggered, so each one built `main +
this PR` as `main` stood at that moment. If `main` has moved since, every one of
them is describing a merge that is not the merge you are about to make.

`main` has `strict: false` deliberately, and `ci.yml`'s header says why:
requiring branches to be up to date forced a rerun on every open PR whenever
`main` moved, and the accepted trade-off is that a semantic conflict surfaces
on `main` post-merge instead. **That trade-off is priced for the fleet of open
PRs. It is not priced for the one PR you are about to merge**, where the
conflict becomes a red `main` that step 13 makes yours.

The measure is when the **run was created**, not when any job started. For a
`pull_request` event GitHub computes the merge commit and freezes `github.sha`
at run creation; no `actions/checkout` in `ci.yml` or `static-checks.yml`
overrides `ref`, so every required job builds that frozen tree no matter how
long it waited to start. `static-checks.yml:297` states this in the repo's own
words, and cites #4895 — a run whose pinned tree was 22 hours older than the
job that scanned it.

So a job's `started_at` is later than the tree it built, by the queue delay,
and using it re-opens the same optimistic bound in a narrower form: `main`
moves at T, a run created a minute before that builds the pre-move tree,
`build` starts three minutes after it, and "earliest required start is newer
than the tip" waves through exactly the merge that went red. Fork PRs held
for workflow approval widen that gap arbitrarily.

Taking the creation time also removes the need to single out the required
checks: all of one push's `pull_request` runs share the event that created
them, so there is no split to filter for, and a run from another event only
ever moves the bound *earlier* — toward re-running, which is the safe
direction. A hand re-run keeps its original `created_at` and only advances
`run_started_at`, which is correct: a re-run still builds the frozen tree.

```sh
HEAD=$(gh pr view <PR> --json headRefOid -q .headRefOid)

# When the merge base was frozen: the earliest run creation on that exact head.
# --paginate: a busy SHA truncates at one page. The per-page --jq streams, so
# the global minimum comes from `sort | head -1`, not from jq's `min`.
gh api --paginate "repos/aethersdr/AetherSDR/actions/runs?head_sha=$HEAD" \
  --jq '.workflow_runs[] | "\(.created_at)\t\(.name)"' \
  | sort | head -1

# main's tip, right now
gh api repos/aethersdr/AetherSDR/commits/main \
  --jq '.sha[0:12] + "  " + .commit.committer.date'
```

**Empty output is not a pass.** No run for this head means nothing has been
built against any `main`, which is strictly worse than a stale green — treat it
as stale and wait for the run. That the required contexts exist and are green
is a separate question, answered at step 0 and again at step 11; this bound
says only how old their tree is.

If `main`'s tip is **newer** than that creation time, the green is stale.
Nothing in `gh pr view` says so: `mergeStateStatus` reports `BEHIND` only
for the branch's own position, and a PR sits at `CLEAN` on a green that predates
`main`'s newest commit. The window does not have to be large — nine minutes
was enough to land #5516 on a green that never saw #5659's changed
`IRadioBackend::setKeying` signature, and `main` went red at the Build step.

**Default: update the branch, then wait for the fresh run.**

```sh
gh api -X PUT repos/aethersdr/AetherSDR/pulls/<PR>/update-branch
```

That appends a merge commit — harmless, since the repo squash-merges and it
never reaches `main` — and triggers CI against current `main`. One cycle, spent
on the one PR that is actually landing, which is exactly the cost the
fleet-wide setting was rejected to avoid and the only place it buys anything.
It is a push, so it dismisses an approval: update **before** you approve, never
after.

The single exemption: every intervening commit touches nothing under `src/`,
`tests/`, `tools/`, `.github/workflows/` or any CMake file. Workflows belong in
that list for the same reason as the rest — a `ci.yml` change on `main` reds
the lander without going near a source tree. Then say so in the report **with
the commit list**, so "I checked" and "I did not check" do not read alike.
Anything else is a re-run, not a judgment call — a changed signature on a
shared seam is the canonical case, and it is invisible to three-way merge
because no line of it conflicts.

Then post one approving review — body only, no new inline comments; anything
worth an inline comment at this point is a finding, and a finding means you
are not approving yet:

```sh
gh api repos/aethersdr/AetherSDR/pulls/<PR>/reviews --input approve.json
# approve.json: {"event":"APPROVE","body":"...","commit_id":"<headRefOid>"}
```

The body states what was fixed (one line per finding, with its SHA), what was
verified empirically versus reasoned, what the maintainer decided when a
decision was taken, and anything that could not be verified and why. Pin
`commit_id` to the head you verified so the review cannot silently re-anchor.

**If `gh api user` and the PR author are the same account**, GitHub returns
422 — you cannot approve your own PR, and the required approval will never
arrive. Put it to the maintainer: admin-merge now (`enforce_admins: false`
allows it), route it to a core dev for the approving review, or arm auto-merge
and leave it blocked pending that review.

## 11. Arm auto-merge — and confirm it can actually fire

```sh
gh pr merge <PR> --auto --squash      # squash is the only method enabled
gh pr view <PR> --json autoMergeRequest,mergeStateStatus,reviewDecision,isDraft,mergeable,statusCheckRollup
```

Arming is a request, not an outcome. Read `mergeStateStatus` after:

- `CLEAN` / `UNSTABLE` — will fire (`UNSTABLE` = a non-required check is red).
- `BLOCKED` with every required check green and `reviewDecision: APPROVED` —
  **an unresolved review thread**, almost always. `main` has
  `required_conversation_resolution: true`, so one thread marked "nit,
  non-blocking" holds the merge indefinitely and nothing in `gh pr view`
  names the cause. Go straight to the step-1 thread query; it will never
  clear on its own and polling has already cost half an hour once.
- `BLOCKED` with every thread resolved and `reviewDecision: REVIEW_REQUIRED`
  — **no code-owner approval yet**, which on this repo is the likelier of the
  two. `required_approving_review_count: 1` with CODEOWNERS review required
  means the approval has to come from an owner of every tier the PR touches;
  if step 10 could not post one — the 422 on your own PR, or a non-code-owner
  identity — this is where it surfaces, and it never clears on its own. No
  amount of re-running the thread query will find it. Name the owner it needs
  and put the route to the maintainer: admin-merge (`enforce_admins: false`
  allows it), request a review from a core dev, or leave it armed and blocked
  pending that review.
- `BEHIND` — `strict: false`, so being behind does not block the merge, and
  being behind is not by itself a reason to update. **It is also not the
  question.** `BEHIND` describes the branch's position; what decides whether
  this PR is safe to arm is whether `main` moved since the head's checks ran,
  which a `CLEAN` PR hides completely. Step 10's green-still-current check is
  the one that answers it, and it runs whatever `mergeStateStatus` says here.
- `DIRTY` — conflicts. Resolving them is a decision (gate): ask.
- `DRAFT` — mark ready first.
- `UNKNOWN` — GitHub is still computing; re-query before concluding anything.

If CI is red on a check unrelated to the PR, check the standing flake umbrella
(#4703) before filing anything new, re-run the job once, and if it persists,
ask. **Never push an empty retrigger commit** — it costs every contributor a
CI cycle and records nothing; a genuine flake gets an issue naming the root
cause.

Verify the arming stuck (`autoMergeRequest` non-null) rather than assuming the
command worked.

## 12. Report (markdown, to the operator)

```markdown
## PR #NNNN — <title> (@author) — landed pending CI

**Head verified:** `<sha>` — built, tested and driven at this SHA.

### Fixed
Numbered, one per finding: what was wrong → what changed (`file:line`) →
the commit → the proof (test name + result, bridge output, CI check).
Where it came from — review thread, blocking review, automated pass, CI, or
my own pass.

### Decided by you
Each decision put through AskUserQuestion, the answer, and where it landed in
the push. "None — nothing in this pass needed a call." if so.

### Refuted
Findings closed without a code change, and the evidence that closed each.

### Threads and reviews
N threads answered and resolved; M blocking reviews dismissed, each with the
SHA cited in its dismissal. Anything left open, and why.

### What I tried to break
The attacks on my own fixes that produced nothing: the edge cases walked, the
tests inverted, the bridge session (which build, which verbs, what state was
asserted). Two to five bullets. Without this, "all clear" and "I did not look"
are indistinguishable. Name anything unverified and why.

### Merge state
`reviewDecision`, `mergeStateStatus`, required checks, whether auto-merge is
armed and confirmed able to fire — plus the one thing still standing between
the PR and `main`, if there is one.

State the green-freshness answer here explicitly: the creation time of the
head's earliest CI run, `main`'s tip at arming time, and which way the
comparison went. If you
took the docs-only exemption, list the intervening commits. A reader cannot
otherwise tell a checked-and-current green from an unchecked one.

### Post-merge (Principle XI)
The squash commit's SHA and the verdict of `main`'s CI on it — the check that
actually demonstrates the fix. If the merge had not fired yet, say that
plainly and name what it is waiting on; never leave it implied.
```

State current state, not the churn: what the branch is now, not a narration of
what was tried and undone.

## 13. The merge is not the end — Principle XI

A fix is demonstrated by CI re-running on the **squash-merge commit on
`main`**, not on the PR branch, with the required checks green. Everything
above is the hypothesis; this is the independent check, and the Constitution
is explicit that agent self-grading does not substitute for it. Skipping it
because "PR CI was green on identical content" is named there as the
rationalization shape that leads to bypassed verification — the squash result
is a different commit from the head you tested.

Once auto-merge fires:

```sh
gh pr view <PR> --json mergedAt,mergeCommit --jq '{mergedAt, sha: .mergeCommit.oid}'
gh run list --branch main --limit 5 --json status,conclusion,headSha,workflowName
```

Wait for the runs on that SHA to conclude. Read `conclusion`, not
`statusCheckRollup` — the latter's `conclusion` is empty while a check is
still running, which reads as "not failing" and is not the same thing.

A red `main` traceable to this merge is **yours**: fix forward immediately or
escalate to the maintainer with the failing job. Do not close the pass out on
PR-branch green alone, and do not leave `main` red behind you.

If the merge has not fired by the time the session ends — CI still running, or
an approval still outstanding — say so in the report and name what the last
check is waiting on. An honest "armed, not yet merged, main unverified" is a
finished pass; an implied one is not.

## Never

- Approve while any finding is unfixed, any thread unanswered, or any fix
  unproven.
- Dismiss a review whose concern is not actually fixed.
- Resolve a thread without a reply carrying the evidence.
- Decide anything in the "Always ask" list without asking.
- Edit `.github/workflows/`, add a `CHANGELOG.md` entry, or key TX.
- Loosen an aetherd ratchet to get a check green — a grown baseline row or
  count, a raised freeze number, or a retagged vendor header. (Shrinking one
  the push earned is the opposite: canon asks for it.)
- Hand-edit the generated touchpoint manifest, or add a new `gui/`→engine
  include without asking.
- Touch the live FLEX-8600, or drive an AetherSDR instance that is not yours.
- Work, build or `git stash` in the invoking checkout.
- `git push origin HEAD:<branch>` on a fork PR.
- Push an empty commit to retrigger CI.
- Leave an item "for the author".
- Post an approving review from an identity that is not a code owner for the
  paths the PR touches.
- Call the pass finished on PR-branch green, or walk away from a `main` turned
  red by the merge you armed.
