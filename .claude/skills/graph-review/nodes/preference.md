# Node: preference — fix, or smuggled personal preference?

Read `stance.md` first. You receive the scope node's `uiRows` (rows of the
scope table that touch existing UI, defaults, or user-facing behavior) plus
the PR body and linked issues. If `uiRows` is empty, still skim the diff for
changed defaults and restyles — the scope node classifies files, not values.

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

Red flags: changed default values (an existing settings key's default, a
slider range, a timer interval) with no issue citing the old default as a
defect; visual restyles (colors, spacing, order, labels) bundled into an
unrelated fix; keyboard/mouse behavior changes described as "improvements";
removed confirmations or notices. Bundling is itself the tell — a genuine fix
rarely needs to adjust neighboring working UI.

Verdicts: a preference change inside a fix PR is a `blocker` — not because
the preference is necessarily wrong, but because it needs its own issue/RFC
and a maintainer ruling per GOVERNANCE.md (cite it). Suggest the split: keep
the fix hunks, move the preference to a proposal. If the whole PR is a
preference change presented as a fix, say so plainly as
`maintainer-decision`. Never let polished code quality launder an unratified
behavior change.

Return `classifications`: one entry per UI/behavior change with `fix` or
`preference` and the authority (or its absence) that decided it.
