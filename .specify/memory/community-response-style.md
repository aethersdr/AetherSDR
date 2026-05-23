# Responding to Community Members on AetherSDR Issues and PRs

## Purpose

This is the AetherSDR project's house style for replying to community
members on GitHub. Collaborators draft responses different ways — some
by hand, some AI-assisted, some delegated entirely to an AI. Following
the same shape across all replies keeps the project feeling welcoming,
keeps user data private, and gives contributors a reliable experience
no matter which collaborator catches their issue.

The document is tool-neutral. Any AI assistant (Claude, Codex, Copilot,
Cursor, Gemini, etc.) can be given this file as context before
drafting; humans can read it as a checklist.

---

## Before you respond

### 1. Read every comment on the thread, not just the body

People post follow-ups: "still happens on v0.7.10," "got around it by
doing X," confirmation that the symptom matches another issue. If you
reply based on the body alone, you risk repeating something a later
commenter already addressed or closing a still-open bug.

### 2. View any attached screenshots before analyzing

GitHub screenshots show state that text descriptions miss — UI
configuration, error icons, version banners, unexpected layouts. Fetch
and look before forming a diagnosis. A real example from the project:
an issue where the user had two panadapters open — visible only in
the screenshot — explained their blank-FFT symptom that nothing in
the text alone would have caught.

### 3. If it's a bug, verify it actually reproduces on current main

For "this is broken on X" issues, build current `main` and try to
reproduce before responding with a fix or a workaround. For "Fix X"
PRs, do the same — confirm the bug is real before judging the fix
correct. The reporter's version may already be obsolete and the
"fix" may be a no-op.

### 4. Scrub PII from anything you paste

When quoting logs, error output, or diagnostic dumps in a public
reply:

- **IP addresses** → `*.*.*.XX` (last octet OK for human triage) or `[redacted]`
- **Radio serial numbers** → `****-****-****-XXXX` (last 4 OK)
- **MAC addresses** → `**:**:**:**:**:XX`
- **Auth tokens / refresh tokens** → `[redacted]` always, no partial reveal
- **User callsigns in PII contexts** → leave as-is (callsigns are FCC
  public record), but use judgment if combined with location/IP

Users send us logs trusting we'll handle their data carefully. PII
in a public comment is a real harm — and it sticks around even after
deletion (search engines, archive.org, GitHub's email notifications
to subscribers).

---

## Tone

### Warm but specific

The project's default voice is welcoming, technical, and willing to
take time. Terse replies feel dismissive even when they're factually
correct. A reply that takes thirty seconds longer to write but
engages the user's specific situation is the difference between
"I'm in" and "I'll never come back."

**Less good:**
> Closed — duplicate of #2640.

**Better:**
> Thanks for filing this — you're seeing the same symptom
> @SomeoneElse reported in #2640, which we shipped a fix for in
> v26.5.1. Could you confirm you're still on that build or an
> earlier one? If you upgrade and still see it, please reopen and
> we'll dig in.

### Engage their specific point

If they mention a specific mode, frequency, OS, radio model, or
firmware version, acknowledge those details. It shows you read
carefully and treat their situation as a real one, not a checklist
item.

### Credit contributors prominently

If someone helped on the issue, in a related PR, or as the author
of a feature being discussed, name them by handle and call out what
they did. Recognition is how we pay community contributors when
there's no funding. Be specific:

- **Less good:** "see #1234"
- **Better:** "@handle's work in #1234 added the panadapter spectrum
  forwarding you're asking about"

---

## Structure

A typical reply has four moves:

```
<Optional opening: AI-assistance disclosure if your AI is posting
on your behalf — not needed if you're reviewing/posting yourself.>

<Acknowledge their specific situation in one sentence.>

<Substantive response: information, a fix, an ask for more detail,
or a pointer to where it's tracked. Redact anything PII-bearing
before pasting.>

<Sign-off: see below.>
```

### Sign-off

Most replies end with:

```
73,
<Your name> <callsign or GitHub handle>
```

`73` is amateur radio shorthand for "best regards" — standard
across ham radio communication. If you'd rather skip the radio
convention, a plain `Thanks,` line works too.

If your AI assistant is **fully drafting and posting on your behalf**
(rather than just helping you write), add an "& <AI name> (AI dev
partner)" tag so readers know an AI is in the loop. Otherwise,
signing as yourself is fine — the AI helped you draft, you reviewed
the result, you posted.

### Optional opening when AI-assisted

If your AI is the one actually posting (not just helping you draft),
prefix with something like:

```
<AI name> here — <opening sentence>
```

Examples: "Claude here," "Codex here," "Hi from Copilot — ." Vary
the wording so it sounds natural, not boilerplate. This is the same
transparency norm newsroom bylines follow — readers should know
who's accountable for the words.

---

## Reactions

### Heart the contribution, not yourself

When a community member files a useful issue, opens a PR, or thanks
you for something:

- Add a ❤️ reaction on the **author's** opening post (the issue/PR
  body or the comment they posted)
- Do not react to your own reply

Reacting to your own comment looks self-congratulatory. The heart
belongs on the contribution.

### How to react

GitHub UI: hover the post → smiley icon → pick ❤️.

CLI (any tool that wraps `gh`):
```sh
# On an issue or PR body:
gh api repos/aethersdr/AetherSDR/issues/N/reactions -f content=heart
# On a specific comment (need the comment ID):
gh api repos/aethersdr/AetherSDR/issues/comments/COMMENT_ID/reactions -f content=heart
```

---

## Merging community PRs

When the PR is good to merge as-is, use **squash** rather than
cherry-pick + close:

```sh
gh pr merge <N> --squash
```

Squash-merge shows as "Merged" (purple) on the contributor's profile
and on GitHub status indicators. Cherry-pick + close shows as
"Closed" (grey) — looks like a rejection even when it isn't.

If the PR needs adjustments, prefer pushing fixes onto the PR branch
over taking the work and closing the PR. The contributor's name stays
on the commits.

In the merge or approval comment, thank them by handle and call out
what their work changed:

```sh
gh pr review <N> --approve -b "Thanks @handle — this fixes the X
regression #NNNN reported. Shipping in v26.X.Y."
```

---

## When to escalate vs. answer in-thread

**Answer in-thread** if:
- It's a usage question and you know the answer
- You can reproduce and patch the bug
- You can point at an existing issue/PR that already tracks the work

**Escalate to the project lead** if:
- The user is asking about commercial terms, sponsorship, or
  licensing
- The report involves a potential security issue (point them at
  SECURITY.md for private disclosure — don't engage on the public
  issue with technical details)
- They're upset about a project decision and the response needs
  maintainer authority behind it

---

## Template you can copy/paste

```
Thanks for filing this, @<their-handle> — <one sentence engaging
their specific symptom, mode, hardware, or question>.

<Substantive reply. If quoting logs, redact first:
  IPs        → *.*.*.XX
  Serials    → ****-****-****-XXXX
  MACs       → **:**:**:**:**:XX
  Tokens     → [redacted]
>

<If relevant: @other-contributor's work in #NNNN is the related
prior art / related fix.>

73,
<your name> <callsign or handle>
```

---

## What to never do

1. **Paste a user's raw IP, serial, MAC, or token into a public
   comment.** No exceptions, even if it "feels" obviously theirs.
2. **Close an issue without reading every comment on it.** Users
   post follow-ups; you'll miss the part where the bug was confirmed
   to still happen.
3. **Reply with only "duplicate of #N."** Always include the
   symptom acknowledgement plus the version that fixed it (or the
   status of the open fix).
4. **React to your own comments.** Heart the contributor's post.
5. **Cherry-pick + close a community PR when squash-merge works.**
   Contributors care about the "merged" status.

---

## Why this exists

AetherSDR is volunteer-built and AI-augmented. The thing that keeps
community contributors coming back isn't an SLA (there isn't one) —
it's the feeling that their report or PR landed with someone who
cared enough to engage with their specific situation. Brief is fine;
brusque isn't.

These rules cost about thirty seconds per reply over raw instinct.
Compounded across the contributor flow, that thirty seconds is the
difference between a project people return to and one they don't.

---

## Feeding this to your AI

If your AI assistant has a memory or context-injection feature:
drop this file into it, or paste the contents into a system-prompt
slot, before drafting community responses. Most modern AI coding
assistants (Claude Code, Codex CLI, Cursor, Copilot Workspace,
Gemini Code Assist) will use it as context for any subsequent
GitHub reply you ask them to draft.

If your AI has no memory feature, paste the relevant section into
the conversation each time, or just keep this open as a reference
checklist alongside your editor.
