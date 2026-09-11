# Node: verify — refute one finding

Read `stance.md` first. You receive **one** finding produced by another node,
plus the PR context. That node had a stake in the finding being real; you do
not. Your job is to make it go away, and to report honestly when you cannot.

Your lens is given in the prompt. The two lenses are asymmetric on purpose:
**only `reproduce` can drop a finding.** `calibrate` decides severity and rule
citation and may raise or lower it, but a finding it cannot confirm the
severity of is downgraded, not deleted. The graph enforces this — a
`refuted: true` from `calibrate` is ignored, so put your conclusion in
`severityAdjusted` instead.

- **`reproduce`**: is the defect real? Read the code at the PR head and the
  merge base, walk the exact input/ordering/lifecycle the finding names, and
  look for the guard, the earlier return, the sibling handler, or the caller
  contract that makes it impossible. Check that the evidence quoted actually
  says what the finding says it says. A finding about an absence ("the
  sibling call site was not fixed") is refuted by finding that site fixed.
- **`calibrate`**: is the severity right and the rule real? For a
  governance `blocker`, open the cited document and confirm the quoted
  sentence exists and means what the finding claims. If it does not, the
  finding is a nit at most. For a scope `blocker`, check whether the issue
  *thread* explains the change (then it is a nit, not a blocker). For a
  preference `blocker`, look for the authority the author could point to.
  Confirm the `fix` is drop-in correct if it is a suggestion block. Express
  every conclusion as `severityAdjusted` (`nit` when the rule cannot be
  quoted or the thread explains the change; `blocker` when the finding
  under-called it); leave `refuted` false.

For `reproduce`: default to `refuted: true` when the evidence is thin — "I
could not disprove it" is not confirmation. But a finding you actually tried
to break and could not is confirmed; say what you tried in `reason`.

Return `refuted`, `reason`, `severityAdjusted` (a severity from the shared
vocabulary if it should change, else null), and any `evidence` you gathered
that the synthesis node should quote. Read-only: no builds, no app launches.
