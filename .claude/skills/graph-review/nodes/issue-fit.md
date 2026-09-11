# Node: issue-fit — does the PR actually solve the linked issue?

Read `stance.md` first. Inputs you were given: the PR number, head/base SHAs,
the PR body, and the linked-issue numbers the coordinator found.

- Linked issues = `closingIssuesReferences` plus any `#NNNN` referenced in
  the PR title/body as "fixes/closes/resolves". Read each with
  `gh issue view <N> --json title,body,comments` and **skim the comments** —
  accepted repro steps, maintainer rulings, and scope changes live in the
  thread, which often redefines the ask.
- Build a short requirements list from the issue (symptom, repro, acceptance
  expectations, explicit non-goals), then map each requirement to the diff:
  which hunk addresses it? Return that mapping in `requirementMap`. Flag
  requirements the diff does not touch as findings; note diff changes that no
  requirement explains in `unexplained` (the scope node will rule on them —
  do not duplicate its verdicts, just hand it the list).
- Coverage: does admissible coverage fail without the fix and pass with it?
  Prefer the smallest socket-free behavioral seam. Missing coverage is a
  blocker only when the reported behavior has a deterministic,
  policy-compliant test seam or project canon explicitly makes that coverage
  merge-gating (quote it). If the only apparent approach is a synthetic
  firmware peer, do **not** request it — describe the honest coverage
  boundary and route positive convergence to bridge/`radiocert` evidence.
  Report an untestable gap as a nit; it is worth naming but not a reason to
  withhold a merge.
- Acceptable coverage by kind (from `AGENTS.md` "Test-layer boundary"):
  wire encoding, parsing, model tables, scheduling, DSP, capabilities, and
  safety policy → socket-free CTest; refusals, malformed/disconnected input,
  dropped messages, non-events, TX guards → socket-free transport or
  state-machine injection; race/lifetime behavior → sanitizer lane; positive
  session/RX/control/meter convergence → automation bridge plus `radiocert`
  against real firmware; a simulator closed loop → explicit opt-in only.
- Never count additions to a retired target, a bracket-commented test, or an
  unregistered target that lacks the `# not registered: <reason>` marker, as
  coverage. An `option()`-gated or `EXCLUDE_FROM_ALL` target carrying that
  marker does count if the PR says how to run it.
- If a test was added or changed, say whether it would pass with the fix
  reverted (reason from the assertions; the build node will try to invert it
  if you list it in `testsToInvert`).
- If there is NO linked issue: say so in `summary`, review the PR against its
  own stated intent, and note whether project process wanted an issue/RFC
  first (GOVERNANCE.md — architectural changes need an RFC; "bug fixes with a
  clear root cause" explicitly do not).
- Every runtime claim in the body ("no behavior change", "other radios
  unaffected", "the panel still remembers its geometry") goes into
  `runtimeClaims` with a concrete way to test it against the demo backend.

Return `verdict`: `yes` / `partially` / `no`, with the mapping as evidence.
