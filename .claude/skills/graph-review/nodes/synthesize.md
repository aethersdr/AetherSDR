# Node: synthesize — write the one GitHub review and the operator report

Read `stance.md` first. You receive every confirmed finding (with verifier
notes), the refuted findings (for the "what I tried to break" section — do
not resurrect them), the scope table, the issue-fit verdict and requirement
map, the build and inversion results, the drive results with their evidence,
and the union of `attacksSurvived`. You post nothing; you return a payload
the coordinator posts verbatim, so get the shape exactly right.

## `review` — the GitHub review payload

`{event, body, comments: [{path, line, side: "RIGHT", body}, …]}`; use
`start_line` + `line` for multi-line anchors. Rules:

- **Inline comments anchor to diff lines only** (`inDiff: true`). A finding
  about untouched code goes in the review body with a `file:line`
  reference. An out-of-scope added file anchors at line 1 of that file.
- Where the fix is a concrete small edit, embed a fenced ` ```suggestion `
  block so the author can one-click apply. Suggestions must be drop-in
  correct — matching indentation, compiling in context — never pseudocode.
- **`event`:** any `blocker` → `REQUEST_CHANGES`. No blockers → `COMMENT`
  (nits inline, verdict in the body). Approval stays the operator's call.
- The **body**, in this order: the issue-fit verdict (one short paragraph);
  the **scope table** (always — write "everything in the diff is explained by
  the issue" when clean); the numbered blockers, each cross-referencing its
  inline comment; `maintainer-decision` items addressed to the maintainer by
  name; then nits marked explicitly non-blocking. State what was verified
  empirically versus read, and — briefly — what you tried to break that held
  up, so the author can see the review was adversarial and can correct you
  if the wrong thing was attacked. Where a finding came from the drive node,
  paste its evidence with it: the state JSON, the log line, the PNG path.
- Tone: collegial, evidence-first, no moralizing. Break the code, not the
  author.

## `report` — markdown for the operator

```markdown
## PR #NNNN — <title> (@author)

**Issue:** #MMMM — one-paragraph summary of the problem as reported.
**Proposed fix:** one-paragraph summary of the approach the diff takes.
**Does it solve the issue?** Yes / Partially / No — with the requirement→hunk
mapping and anything unaddressed.

### Scope
The scope table, then one line on whether the body's own checklist holds up.
Never omit the section: "checked and clean" and "did not check" must not
look the same.

### Blockers
Numbered. What's wrong, the evidence, the rule (if governance), the fix.
"None." if none.

### Nits
Bulleted, explicitly non-blocking.

### What I tried to break
The attacks that did NOT produce a finding: the body claims tested and held,
the edge cases walked, the tests inverted, what was built and run, and the
bridge session (which build, against the demo, which tools, what state was
asserted). Include the findings the verify nodes refuted, in one line each,
so the operator can see the graph's rejection rate. Name anything that could
not be tested and why.

### Recommendation
**Approve** / **Approve with nits** / **Request changes** /
**Needs maintainer decision** — plus 2–3 sentences: reasoning, what was
verified empirically, the concrete next step.
```

Also return `socketNotice`: if the coordinator's preflight flagged a socket
test, one paragraph recording the test, socket type, target, whether CI runs
it, and that the operator was notified — it goes in the review body too.
