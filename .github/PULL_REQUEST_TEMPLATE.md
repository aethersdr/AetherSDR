<!--
Thanks for contributing to AetherSDR!

Before opening the PR, please:
- Read AGENTS.md if this is your first contribution (or your AI tool's
  first contribution) — it has the conventions every contributor agent
  is expected to follow.
- Read CONTRIBUTING.md for the commit-signing and branch-protection
  rules.
- If you're an AI agent: claim the originating issue first by assigning
  yourself via `gh issue edit <N> --add-assignee <handle>`. Double-
  assigning alongside the @aethersdr-agent triage bot is fine.
-->

## Summary

<!-- One-paragraph description of what changes and why.  If this fixes
an issue, lead with "Fixes #N" or "Closes #N". -->

## Constitution principle honored

<!-- Cite the principle by Roman numeral when relevant.  E.g.
"Principle V — new feature uses nested-JSON persistence."
"Principle II — MeterSmoother used for the new meter widget."
See CONSTITUTION.md for the full list. -->

## Test plan

- [ ] Local build passes (`cmake --build build`)
- [ ] Behavior verified on a real radio if applicable
- [ ] Existing tests pass (CI)
- [ ] Reproduction steps documented if user-reported bug

## Checklist

- [ ] Commits are signed (`docs/COMMIT-SIGNING.md`)
- [ ] No new flat-key `AppSettings` calls — use nested-JSON-under-one-key
      (Principle V)
- [ ] All meter UI uses `MeterSmoother` (Principle II)
- [ ] Documentation updated if user-visible behavior changed
- [ ] Security-sensitive changes reference a GHSA if applicable