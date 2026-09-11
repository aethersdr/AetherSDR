# Node: quality — code-quality audit

Read `stance.md` first. Read for: correctness of the actual state
machine/lifecycle being touched (connect/disconnect, slice recreate, radio
swap are the recurring minefields), thread-safety (audio callback vs main
thread; AppSettings is thread-safe but `save()` does I/O — never on the
render callback), Qt object lifetime (`QPointer`/`WA_DeleteOnClose`,
parenting), error handling per house style (no exceptions; check returns;
`qWarning` with category), silent failure modes (unchecked writes, swallowed
errors), and whether comments explain *why* (constraints), not *what*.

Read hostilely: for each non-trivial hunk, construct the input, ordering, or
lifecycle event that makes it misbehave before you accept that it doesn't.
Walk the failure paths as carefully as the happy path — what the code does
when the write fails, the pointer is stale, the radio drops mid-call, or the
user does it twice. Each hunk you attacked and could not break goes in
`attacksSurvived` with the attack named.

You do not build or run anything. Where a suspicion needs the running app to
settle, return it as a finding with `needsRuntime: true` and a concrete
repro the drive node can execute against the demo backend; where it needs a
test inverted (break the code on purpose and confirm the test notices), name
the test and the mutation in `testsToInvert` for the build node. A
regression test that still passes with the fix reverted is a `blocker` in
its own right — it pins nothing and will read as coverage forever after.

If a `code-review` skill or bot comment exists on the PR, treat its output
as leads: verify each yourself and keep only what you confirmed, attributed
("automated pass found …" in `evidence`). Drop anything you can refute.

Prefer the claim the PR most depends on and would most like you to take on
faith — that is the one to push hardest.
