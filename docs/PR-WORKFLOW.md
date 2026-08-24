# PR Workflow

How changes move through review and land on `main`: draft conventions,
the stale-branch policy, and recovery when post-merge CI goes red.

*Who* must approve *what* is policy and lives in
[`CONTRIBUTING.md`](../CONTRIBUTING.md#reviews-and-merging), backed by
[`.github/CODEOWNERS`](../.github/CODEOWNERS). This document covers the
mechanics around that gate, not the gate itself.

---

## Draft PR conventions

Draft status carries different meaning depending on who opened the PR:

- **Human-authored draft** — work-in-progress; reviewers should skip
  these until the author marks Ready for Review.
- **`@AetherClaude` / `aethersdr-agent[bot]` draft** — auto-generated
  from an issue and **awaiting human review**. The draft state holds
  the PR back from auto-merge; it is not "WIP". Treat it like a
  ready-to-review PR for triage purposes.

Triage scripts and review agents should include bot drafts in their
sweep and skip only human drafts.

---

## Stale-branch policy

We **do not** require PR branches to be up to date with `main` before
merging. The reasoning:

- Squash-merge already runs a fresh three-way merge against `main`, so
  textual conflicts are caught at merge time regardless of branch age.
- Forcing every PR to rebase after every other merge cost ~15–25 min
  of CI per stale PR per batch day, which adds up fast when AetherClaude
  is processing a queue of triaged issues.
- Post-merge CI on `main` runs on every commit (see
  [`.github/workflows/ci.yml`](../.github/workflows/ci.yml) and
  [`.github/workflows/codeql.yml`](../.github/workflows/codeql.yml)), so
  semantic conflicts that slip through three-way merge are caught on
  the merged result within ~10 min.

---

## Recovering from a red main

If post-merge CI on `main` fails after a merge:

1. Check the failing workflow run linked from the email/GitHub
   notification. Identify the offending merge commit.
2. **Prefer fix-forward** if the issue is small (one or two file edits):
   open a normal PR titled `fix(ci): repair main after <SHA>` and let
   it land through the usual flow.
3. **Use revert** if fix-forward isn't obvious or the regression is
   broad: `git revert -m 1 <merge-sha>` on a new branch, push, open a
   PR, merge. Never force-push `main`.
4. For agent-authored regressions: re-open the source issue, remove
   the `aetherclaude-eligible` label, then re-add it. The orchestrator's
   State Override C (`failed` → `implement` re-entry) creates a fresh
   worktree from current `main` and retries the implementation.
