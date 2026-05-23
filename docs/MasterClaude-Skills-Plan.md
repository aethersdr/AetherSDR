# MasterClaude Skills Implementation Plan

## Overview

Eight new skills for MasterClaude, extending it from issue-fix-only to a
full community support agent. All skills operate within the existing security
architecture (MCP token isolation, nftables, sandboxed Claude Code).

**Priority order:** Skills are listed from highest community impact to lowest.
Implement in this order.

---

## Architecture Changes (shared across all skills)

### 1. Event Poller (replaces issue-only fetcher)

The current orchestrator only checks for labeled/assigned issues. We need a
unified event poller that detects new PRs, new issues, stale issues, and CI
failures — then dispatches to the appropriate skill.

**New file:** `/home/MasterClaude/bin/run-agent.sh` (rewrite)

```
run-agent.sh
  ├── poll_new_issues()         → Skill 1 (Issue Triage) + Skill 7 (Bug Report Quality)
  ├── poll_new_prs()            → Skill 2 (PR Review) + Skill 3 (First-Time Welcome)
  ├── poll_stale_issues()       → Skill 4 (Stale Issue Triage) — weekly only
  ├── poll_ci_failures()        → Skill 8 (CI Failure Explainer)
  ├── poll_discussions()        → Skill 5 (Discussion Responder)
  └── process_eligible_issues() → Existing: code fix + PR creation
```

Each poller tracks its last-seen state in a JSON file
(`/home/MasterClaude/state/last-poll.json`) to avoid processing the same
event twice.

### 2. MCP Server Extensions

Add new operations to `github-mcp-server.js`:

```
Existing:
  - read_issue
  - list_issue_comments
  - comment_on_issue
  - create_pull_request

New:
  - create_pr_review        (POST /pulls/{n}/reviews)
  - list_pull_request_files (GET /pulls/{n}/files — see changed files)
  - get_pull_request_diff   (GET /pulls/{n} with Accept: diff — raw diff)
  - search_issues           (GET /search/issues — for duplicate detection)
  - get_ci_run_logs         (GET /actions/runs/{id}/logs — CI failure analysis)
  - graphql_query           (POST /graphql — for Discussions, gated to read-only queries)
  - add_discussion_comment  (POST /graphql mutation — reply to Discussions)
```

All operations go through the same token isolation and audit logging.

### 3. GitHub App Permission Updates

The GitHub App (MasterSDR Bot) needs additional permissions:

| Permission | Current | Needed | Why |
|------------|---------|--------|-----|
| Issues | Read/Write | Read/Write | No change |
| Pull requests | Read/Write | Read/Write | No change — covers PR reviews |
| Discussions | None | **Read/Write** | Discussion responses |
| Actions | None | **Read** | CI failure log access |
| Contents | Read | Read | No change |

Update at: GitHub → Settings → Developer settings → GitHub Apps → MasterSDR Bot → Permissions

### 4. Prompt Templates

Each skill gets a dedicated prompt template stored in
`/home/MasterClaude/prompts/`. The orchestrator loads the appropriate template
and fills in variables (issue number, PR number, author, etc.).

---

## Skill 1: Community PR Review

**Trigger:** New PR opened on `MasterSDR/MasterSDR` (polled every 30 min)

**Skip if:**
- PR author is `ten9876` (maintainer) or `MasterClaude` (self)
- PR already has a review from `MasterSDR-agent[bot]`
- PR is a draft

**What it does:**
1. Fetch PR diff via `get_pull_request_diff`
2. Fetch changed file list via `list_pull_request_files`
3. Feed to Claude Code with the upstream CLAUDE.md conventions
4. Claude Code analyzes for:
   - `QSettings` usage (forbidden — must use `AppSettings`)
   - Naked `new`/`delete` (must use RAII)
   - Missing firmware version comments on protocol code
   - Files outside expected scope (does the PR touch what it claims to?)
   - Obvious bugs, missing null checks, resource leaks
5. Post a PR review via `create_pr_review` with event `COMMENT`
   (never `REQUEST_CHANGES` — that's the maintainer's call)

**Prompt template key points:**
- "You are reviewing a community contribution. Be constructive and specific."
- "If the code is good, say so briefly. Do not nitpick style unless it
  violates CLAUDE.md conventions."
- "Do NOT approve or request changes — post as COMMENT only."
- "Thank the contributor for their work."

**Claude Code tools needed:** Read, Grep, Glob (read the codebase for context),
MCP operations (read PR, post review)

**No Bash needed.** Read-only analysis.

**Estimated implementation:** 3-4 hours

---

## Skill 2: First-Time Contributor Welcome

**Trigger:** New PR or issue where `author_association` is
`FIRST_TIME_CONTRIBUTOR` or `FIRST_TIMER`

**Skip if:**
- Author already has a welcome comment from `MasterSDR-agent[bot]`

**What it does:**
1. Detect first-time status from the `author_association` field (already
   present in API responses — no extra call needed)
2. Post a welcome comment via `comment_on_issue`:

   ```
   Welcome to MasterSDR, @username! Thanks for your first contribution.

   A few things that might help:
   - Our [CONTRIBUTING.md](link) covers the coding conventions and PR process
   - CI will run automatically — if it fails, I'll post a comment explaining what went wrong
   - Jeremy (KK7GWY) reviews all PRs before merge

   If you have questions, feel free to ask here or in [Discussions](link).

   — MasterClaude (automated agent for MasterSDR)
   ```

3. If it's a PR, also trigger Skill 1 (PR Review)

**No Claude Code invocation needed.** This is a static template in the
orchestrator bash script — no AI processing, just string interpolation.
Fastest possible response time.

**Estimated implementation:** 30 minutes

---

## Skill 3: Stale Issue Triage

**Trigger:** Weekly cron (separate systemd timer, e.g., Sundays at 10:00 AM)

**Skip if:**
- Issue has label `pinned`, `long-term`, or `wontfix`
- Issue already has a stale ping from `MasterSDR-agent[bot]` in last 30 days
- Issue is assigned to someone (they're working on it)

**What it does:**
1. Search API: `repo:MasterSDR/MasterSDR is:issue is:open updated:<30_DAYS_AGO`
2. For each stale issue:
   a. Read the issue body and all comments
   b. Feed to Claude Code for context-aware triage
   c. Claude Code determines one of:
      - **Still relevant:** Post a polite check-in asking if still an issue
      - **Likely fixed:** Check if recent commits addressed it, comment accordingly
      - **Needs info:** Ask for specific missing information
   d. Post the appropriate comment via MCP

**Prompt template key points:**
- "This issue has had no activity for 30+ days."
- "Do NOT close the issue. Only comment."
- "Be respectful — the reporter may have moved on, but the bug may still exist."
- "If recent commits seem to address this issue, mention the specific commit
  and ask the reporter to verify."
- "Never use the word 'stale' — it sounds bureaucratic."

**Rate limiting:** Process max 5 stale issues per weekly run. Don't spam.

**Systemd timer:**
```ini
[Timer]
OnCalendar=Sun 10:00
Persistent=true
```

**Estimated implementation:** 2-3 hours

---

## Skill 4: Release Notes Compiler

**Trigger:** Manual invocation (`systemctl start MasterClaude-release-notes`)
or via a labeled issue (`release-notes-draft`)

**What it does:**
1. Find the latest release tag: `git describe --tags --abbrev=0`
2. List all merged PRs since that tag:
   `gh pr list --repo MasterSDR/MasterSDR --state merged --search "merged:>DATE"`
3. For each PR, extract: number, title, author, labels
4. Feed the list to Claude Code with instructions to:
   a. Group by category (Bug Fix, Feature, Enhancement, Internal)
   b. Write a human-readable summary for each item
   c. Credit every contributor by GitHub username
   d. Note any breaking changes
5. Output the draft to a file and comment it on the triggering issue

**Prompt template key points:**
- "Write release notes for a ham radio software project."
- "Every contributor gets credited by name, even for small fixes."
- "Group: New Features, Enhancements, Bug Fixes, Internal/CI."
- "Keep each item to 1-2 lines. Link PR numbers."
- "Do not include internal refactors unless they affect users."

**Claude Code tools needed:** Read (codebase context), Bash(git log/tag),
MCP (comment on issue)

**Estimated implementation:** 2 hours

---

## Skill 5: Discussion Responder

**Trigger:** New Discussion with no replies (polled every 30 min via GraphQL)

**Skip if:**
- Discussion already has a reply
- Discussion category is "Announcements" (maintainer-only)
- Discussion is locked

**What it does:**
1. Poll Discussions via GraphQL (new MCP operation)
2. Read the discussion body and any existing comments
3. Feed to Claude Code with the full CLAUDE.md as context
4. Claude Code determines if it can answer:
   - **Yes:** Post a helpful reply via `add_discussion_comment`
   - **Partially:** Post what it knows, flag for maintainer
   - **No:** Skip (don't post "I don't know" responses)

**Prompt template key points:**
- "You are answering a community question about MasterSDR."
- "Only answer if you are confident. Silence is better than a wrong answer."
- "Reference specific documentation, files, or settings when possible."
- "If the question is about radio hardware, firmware, or FlexRadio protocol
  behavior you're not sure about, say so and suggest they ask in the
  FlexRadio community forum."
- "End with: 'Jeremy can correct me if I got anything wrong here.'"

**Requires:** GitHub App `discussions: read/write` permission + GraphQL MCP
operations

**Estimated implementation:** 4 hours (GraphQL integration is the main work)

---

## Skill 6: Duplicate Issue Detection

**Trigger:** Every new issue (runs as part of new issue polling)

**Skip if:**
- Issue is a feature request (label `enhancement`)
- Issue has fewer than 20 words in the body

**What it does:**
1. Extract key terms from the new issue title and body
2. Search existing open AND closed issues via `search_issues` MCP operation:
   `repo:MasterSDR/MasterSDR is:issue "keyword1" "keyword2"`
3. If candidates found (>0 results with different issue numbers):
   a. Feed the new issue + candidates to Claude Code
   b. Claude Code determines if any are true duplicates
   c. If duplicate found, post a comment:
      ```
      This looks similar to #456 — is this the same issue, or something
      different? If it's the same, we can track it there.

      — MasterClaude (automated agent for MasterSDR)
      ```
   d. If not a duplicate, skip silently

**Prompt template key points:**
- "Only flag as duplicate if you are genuinely confident."
- "Similar is not the same as duplicate. Two issues about 'waterfall' are
  not duplicates unless they describe the same bug."
- "Never close or label the issue. Just ask the question."
- "If the existing issue is closed/fixed, mention that: 'This was addressed
  in #456 (fixed in v0.8.2). Can you check if it still happens on the
  latest version?'"

**Estimated implementation:** 2 hours

---

## Skill 7: Bug Report Quality Assistant

**Trigger:** New issue missing key information (detected during new issue polling)

**Skip if:**
- Issue uses the bug report template and all fields are filled
- Issue is a feature request
- Issue already has a comment from `MasterSDR-agent[bot]`

**What it does:**
1. Parse the issue body for presence of:
   - Radio model & firmware version
   - OS & MasterSDR version
   - Steps to reproduce
   - Expected vs actual behavior
   - Logs or screenshots
2. If 2+ fields are missing, feed to Claude Code to draft a polite request
3. Post via `comment_on_issue`:
   ```
   Thanks for reporting this, @user. To help us track it down, could you
   share a few more details?

   - What radio model and firmware version are you using?
   - What's your OS and MasterSDR version?
   - What steps reproduce the issue?

   If you can attach logs (Help → Support → File an Issue), that would
   be especially helpful.

   — MasterClaude (automated agent for MasterSDR)
   ```

**Can be mostly template-based** with Claude Code only invoked if the issue
body is ambiguous about what's missing.

**Estimated implementation:** 1-2 hours

---

## Skill 8: CI Failure Explainer

**Trigger:** CI failure on a PR from a non-maintainer (polled with PR checks)

**Skip if:**
- PR author is `ten9876` or `MasterClaude`
- A CI explainer comment already exists on this PR
- CI is still running (only trigger on completed + failed)

**What it does:**
1. Detect failed check runs via `GET /commits/{sha}/check-runs`
2. Download failed job logs via `get_ci_run_logs` MCP operation
3. Extract the relevant error section (last 100 lines of failed step)
4. Feed to Claude Code with context about the build system (CMakeLists.txt)
5. Post a helpful PR comment:
   ```
   The CI build failed — here's what happened:

   **Error:** `src/gui/NewWidget.cpp:42: error: 'QSettings' was not declared`

   **Fix:** MasterSDR uses `AppSettings` instead of `QSettings` (see
   `src/core/AppSettings.h`). Replace `QSettings` with
   `AppSettings::instance()`.

   The [CONTRIBUTING.md](link) has more details on coding conventions.

   — MasterClaude (automated agent for MasterSDR)
   ```

**Prompt template key points:**
- "Explain the build error in plain language."
- "Suggest a specific fix, not just 'check the docs'."
- "If the error is in a dependency or CI infrastructure (not the contributor's
  code), say so — don't blame the contributor."
- "Link to relevant documentation or source files."

**Requires:** GitHub App `actions: read` permission

**Estimated implementation:** 3 hours

---

## Implementation Sequence

### Phase 1 — Quick Wins (Week 1)
| # | Skill | Effort | Dependencies |
|---|-------|--------|-------------|
| 1 | First-Time Welcome | 30 min | None (template in orchestrator) |
| 2 | Bug Report Quality | 1-2 hr | None (template + basic parsing) |
| 3 | Duplicate Detection | 2 hr | `search_issues` MCP operation |

### Phase 2 — Core Skills (Week 2)
| # | Skill | Effort | Dependencies |
|---|-------|--------|-------------|
| 4 | PR Review | 3-4 hr | `create_pr_review`, `get_pull_request_diff` MCP ops |
| 5 | CI Failure Explainer | 3 hr | `get_ci_run_logs` MCP op, GitHub App `actions: read` |
| 6 | Stale Issue Triage | 2-3 hr | `search_issues` MCP op, weekly timer |

### Phase 3 — Extended Skills (Week 3)
| # | Skill | Effort | Dependencies |
|---|-------|--------|-------------|
| 7 | Discussion Responder | 4 hr | GraphQL MCP ops, GitHub App `discussions: write` |
| 8 | Release Notes | 2 hr | None (git log + existing MCP) |

### Total estimated effort: ~18-22 hours

---

## Orchestrator Rewrite Summary

The current `run-agent.sh` handles one flow: fetch eligible issues → Claude
Code → PR. The rewrite adds a dispatcher:

```bash
#!/bin/bash
# MasterClaude Agent Orchestrator v2

source /home/MasterClaude/.env
STATE_FILE="/home/MasterClaude/state/last-poll.json"

# Initialize state file if missing
[ -f "$STATE_FILE" ] || echo '{}' > "$STATE_FILE"

# --- Quick checks (no Claude Code needed) ---
check_first_time_contributors    # Skill 2: welcome message
check_bug_report_quality         # Skill 7: missing info request

# --- Claude Code skills ---
process_eligible_issues          # Existing: code fix + PR
review_new_prs                   # Skill 1: PR review
detect_duplicates                # Skill 6: duplicate detection
explain_ci_failures              # Skill 8: CI failure help
respond_to_discussions           # Skill 5: discussion answers

# --- State update ---
update_last_poll_state
```

The weekly stale triage (Skill 3) and on-demand release notes (Skill 4) run
on separate timers/triggers.

---

## Security Considerations

All new skills operate within the existing security architecture:

- **MCP token isolation:** New MCP operations added to the same server, same
  audit logging, same credential leak detection
- **No new Bash access:** PR review and discussion response are read-only
  analysis — no shell commands needed beyond git
- **Prompt injection risk:** PR diffs and discussion bodies are untrusted
  input, same as issue bodies. Same sanitization applies.
- **Rate limiting:** Each skill has a max-per-run cap to prevent runaway
  API usage
- **Audit trail:** All MCP operations logged to `mcp-audit.log`

### New attack surfaces to monitor

| Skill | New Risk | Mitigation |
|-------|----------|-----------|
| PR Review | Malicious diff content injection | Input sanitization, read-only analysis |
| Discussion Responder | Injection via discussion body | Same sanitization as issues |
| CI Failure Explainer | Injection via crafted build output | Truncate logs to last 100 lines, sanitize |
| Duplicate Detection | False positive spam | Confidence threshold, max 1 duplicate flag per issue |

---

*Document version: 1.0 — 2026-04-05*
*Author: Jeremy Fielder (KK7GWY) & Claude (AI dev partner)*
