---
name: triage
description: Triage all open GitHub issues — label, respond, prioritize, and flag for AetherClaude agent
user_invocable: true
---

# Issue Triage

Perform a full triage of all open GitHub issues on the AetherSDR repository. Execute each step thoroughly.

## Step 1: Fetch Issues

Run `gh issue list --state open --limit 100 --json number,title,labels,author,createdAt,updatedAt` to get all open issues.

Compare against the last known triage state. Focus on issues that are new or have new comments since the last triage. If unsure, triage all open issues.

## Step 2: Read Each Issue

For EVERY open issue:
1. Read the issue body AND all comments using `gh issue view <number> --json body,comments,labels,author`
2. If screenshots are referenced (GitHub image URLs), fetch and view them
3. Check if the issue might already be fixed in main by searching the codebase or recent commits

## Step 3: Apply Labels

Apply appropriate labels from our label set:
- **Type:** `bug`, `enhancement`, `New Feature`, `question`, `duplicate`, `wontfix`
- **Category:** `GUI`, `audio`, `spectrum`, `external devices`, `CW`, `protocol`, `SmartLink`, `multi-pan`
- **Platform:** `macOS`, `Windows`
- **Priority:** `priority: high`, `priority: medium`, `priority: low`
- **Build:** `github_actions`, `dependencies`
- **Safety:** `safety`, `upstream`
- **Agent:** `aetherclaude-eligible` (see criteria below)

## Step 4: Respond to Issues

For each issue that has no response from us, post a comment:
- Start with "Claude here — happy [day/holiday]!" or just "Claude here —" as appropriate
- Be warm, thoughtful, and specific to their issue
- State our plan for addressing it, or ask for missing information
- If requesting info, be specific about what we need (support bundle, OS, radio model, firmware version, steps to reproduce, screenshots)
- End with: `73, Jeremy KK7GWY & Claude (AI dev partner)`
- NEVER post user IP addresses or radio serial numbers
- Give prominent credit to contributors when their work is relevant

## Step 5: Close Resolved Issues

Close issues that are:
- Already fixed in the current release or main branch
- Duplicates (link to the original)
- Not actionable (out of scope, user misunderstanding)
- Confirmed resolved by the reporter

Always explain why when closing and thank the reporter.

## Step 6: Flag for AetherClaude Agent

Apply `aetherclaude-eligible` label ONLY to issues that meet ALL criteria:
- Clear, well-defined bug with obvious fix
- No protocol investigation or radio interaction needed
- No architectural decisions required
- No user interaction needed (we have enough info)
- Fix is localized (1-3 files, straightforward pattern)
- Examples: missing AppSettings persistence, missing guard checks, one-line fixes, simple UI bugs

Do NOT flag: protocol issues, audio pipeline bugs, multi-threading issues, feature requests, issues needing user clarification, anything touching RadioModel/AudioEngine core logic.

## Step 7: Report Back

Present a prioritized summary table grouped by:

1. **Critical** — fix next release (crashes, data loss, core functionality broken)
2. **Medium** — bugs to fix in next 1-2 releases
3. **Build/CI** — build system and CI issues
4. **Enhancements** — feature requests and improvements
5. **Major Features** — long-term/architectural features
6. **Awaiting Info** — issues where we need user response

For each issue include: number, title, assigned labels, and one-line plan.
