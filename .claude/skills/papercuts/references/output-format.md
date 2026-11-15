# Output format

Two layouts. Default is **priority/report**; use **capability** when the user
asks (`sort:capability`). Every issue number is a markdown link to
`https://github.com/aethersdr/AetherSDR/issues/<n>`.

## Today? legend (used in every table)

- ✅ clean — small surface, bridge proves it, no TX gate → ideal for today
- ➖ caveat — provable but has a wrinkle (TX-gated, restart cycle, platform-specific, multislice setup)
- ❌ no — not realistically closeable today (needs hardware, audio judgment, big design)
- ⏳ in-flight — an active non-AetherClaude assignee; don't double-book.
  `claude-active` is **not** this: it marks the automated triage bot, triage
  isn't exclusive, and it blocks nobody — never mark a row ⏳ for that label alone

## Layout A — priority / report (default)

Lead with a one-line note on the **report proxy** (e.g. "0 issues have 👍, so
comment count is the report proxy"). Then a table per priority tier, sorted by
comments descending within each tier:

```markdown
### 🔴 High
| # | Cmts | Opener | What | Today? |
|---|---|---|---|---|
| [#3714](…/issues/3714) | 5 | mkoechel | Crash adding 2nd panadapter | ➖ repro fast via `pan add`, fix may be deeper |

### 🟠 Medium
…
### 🟡 Low (most-discussed first)
…
### ⚪ No priority label
…
```

Then:

```markdown
## 🎯 Close-today shortlist (smallest surface × clean prove × no TX gate)
In the order I'd knock them out:
1. **[#3505](…)** — one-line what + the bridge verb that proves it. N reports.
…
**Best single bundle for today:** #X + #Y — why they share a fix site.
```

## Layout B — capability map (`sort:capability`)

Group by the bridge verb that proves each issue; flag verbs newly unlocked by a
recent PR with 🆕. Within each group: `# · Opener · What`.

```markdown
### 🆕 `drag` — handles, grips, scale drags
| # | Opener | What |
|---|---|---|
| [#2310](…) | rnash2 | Spectrum dBm-scale drag skews noise floor — upgraded from "hard to prove" |

### 📡 `key ptt` + `slice txant` — TX path & meters (authorized dummy load)
…
```

## Closing read (both layouts)

A short "what changed / what to do" paragraph or bullets:
- **Reporter concentration** — who filed several; worth keeping in the loop when fixes land.
- **Densest capability bucket** — highest-leverage place to batch work.
- **Bundling opportunities** — issues sharing a fix site (close 2+ in one worktree).
- **What a recent bridge PR unlocked or retired** — e.g. "`drag` retires the
  'can't drive a drag headlessly' caveat, promoting #2310."

## Voice

Terse and skimmable. The operator is triaging fast — the tables carry the
information; prose only adds the judgment a table can't. No preamble, no recap of
the request.
