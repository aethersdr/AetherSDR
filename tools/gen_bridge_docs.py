#!/usr/bin/env python3
"""
Keep docs/automation-bridge.md in sync with the AutomationServer verb registry
(#4174 Phase 3). Two jobs, both cheap and app-free:

  1. Auto-generated verb table — the canonical {verb, aliases, help} list is
     derived from the `add(...)` registrations in AutomationServer.cpp and
     written into docs between the GENERATED markers. This kills the recurring
     "a verb landed but never made it into the docs table" drift (the banner
     and error strings are already registry-derived in-code; this extends the
     same discipline to the docs).

  2. Duplicate-heading lint — flags two `###`/`####` sections with the same
     title, the silent-bad-merge class that slipped two `### rightClick`
     sections past review during the 2026-07 cycle.

Usage:
    tools/gen_bridge_docs.py           # rewrite the generated block in place
    tools/gen_bridge_docs.py --check   # CI: exit 1 if the block is stale or
                                       #     a duplicate heading exists

No app, no Qt, no build required — pure static parse.
"""

import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
CPP = os.path.join(REPO, "src", "core", "AutomationServer.cpp")
DOCS = os.path.join(REPO, "docs", "automation-bridge.md")

BEGIN = "<!-- BEGIN GENERATED VERB TABLE (tools/gen_bridge_docs.py) -->"
END = "<!-- END GENERATED VERB TABLE -->"

# add("name", {aliases}, "help …",  — the three fields may wrap across lines,
# and aliases may be bare "x" or QStringLiteral("x"); either way the alias
# spelling is the quoted string inside the {…} block. DOTALL so the help can
# sit on the line after the aliases (7 verbs do this).
# The help capture accepts ADJACENT STRING LITERALS ("a" "b" — how every long
# help string in AutomationServer.cpp is written): the old single-literal
# capture silently truncated six verbs' help at the first literal boundary,
# and --check compared the doc against the same truncated render, so the gate
# could never fail on that class of loss (PR #4964 review, K6OZY).
_ADD_RE = re.compile(
    r'\badd\(\s*"(?P<name>[^"]+)"\s*,\s*'
    r'\{(?P<aliases>[^}]*)\}\s*,\s*'
    r'(?P<help>"(?:[^"\\]|\\.)*"(?:\s*"(?:[^"\\]|\\.)*")*)',
    re.DOTALL,
)


def _join_literals(raw):
    # "abc" "def"  ->  abcdef (per-literal unescape happens in _unescape)
    return "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', raw, re.DOTALL))
_ALIAS_RE = re.compile(r'"([^"]+)"')
# Loose "there's a registration here" probe: just `add("name"`, independent of
# the full-shape match above. Used to cross-check that the strict parser didn't
# silently skip a registration formatted in an unexpected way.
_NAME_RE = re.compile(r'\badd\(\s*"([^"]+)"')


def _unescape(s):
    # Turn C string escapes into display text WITHOUT touching multi-byte UTF-8
    # (unicode_escape would corrupt the → arrows in the help strings).
    return (s.replace('\\"', '"').replace("\\n", " ")
             .replace("\\t", " ").replace("\\\\", "\\").strip())


def extract_registry(cpp_path):
    """Return [(name, [aliases], help), …] in registration order.

    Raises if a registration exists (an `add("name"` site) that the strict
    parser didn't capture — otherwise a future oddly-formatted `add(...)` would
    vanish from the docs *silently*, defeating the whole point of the drift
    check (the generated table would just be missing a verb, and `--check`
    would still pass against that wrong table).
    """
    with open(cpp_path, encoding="utf-8") as f:
        src = f.read()
    verbs = []
    for m in _ADD_RE.finditer(src):
        aliases = _ALIAS_RE.findall(m.group("aliases"))
        verbs.append((m.group("name"), aliases,
                      _unescape(_join_literals(m.group("help")))))

    parsed = {name for name, _, _ in verbs}
    sites = set(_NAME_RE.findall(src))
    missed = sites - parsed
    if missed:
        raise ValueError(
            f"gen_bridge_docs: {len(missed)} registration(s) matched "
            f"add(\"name\" but not the full add(name, {{aliases}}, \"help\", …) "
            f"shape — {sorted(missed)}. They'd silently drop from the docs; "
            "fix the parser or the registration formatting.")
    return verbs


def render_table(verbs):
    rows = ["| Verb | Aliases | Description |", "|---|---|---|"]
    for name, aliases, help_text in verbs:
        al = ", ".join(f"`{a}`" for a in aliases) if aliases else "—"
        # Escape pipes so a help string can't break the table.
        help_cell = help_text.replace("|", "\\|").strip()
        rows.append(f"| `{name}` | {al} | {help_cell} |")
    return "\n".join(rows)


def generated_block(verbs):
    return (f"{BEGIN}\n"
            f"<!-- Do not edit by hand — run tools/gen_bridge_docs.py. "
            f"{len(verbs)} verbs. -->\n\n"
            f"{render_table(verbs)}\n\n"
            f"{END}")


# A verb's ACTION set is its own drift surface. #5102 was filed against a
# working feature because two hand-maintained copies of `slice`'s list
# disagreed; the in-code copies were collapsed into sliceActionList(), and the
# docs table a reader actually consults is pinned to it here. The same shape
# now covers any verb with a `QString <verb>ActionList()` single source and a
# `### \`<verb>\`` section whose table's first column names the actions.
_ACTION_LIST_RE = re.compile(
    r'QString\s+(?P<stem>\w+?)ActionList\(\)\s*\{\s*return\s+QStringLiteral\('
    r'(?P<body>(?:\s*"(?:[^"\\]|\\.)*")+)\s*\)\s*;',
    re.DOTALL)

# verb -> (C++ ActionList() stem, doc heading). Add a row here, a
# `<stem>ActionList()` function in AutomationServer.cpp, and a first-column
# `` `action` `` table under the heading, and the drift check covers it.
SUBACTION_VERBS = {
    "slice": ("slice", "### `slice`"),
    "pan": ("pan", "### `pan`"),
    "audioCapture": ("audioCapture", "### `audioCapture`"),
}

# Deliberately documented, deliberately NOT in the advertised list — accepted
# aliases the *ActionList() strings omit on purpose (e.g. `slice source` is an
# alias of `slice rxsource`). Naming them here is the difference between a lint
# that encodes a known exception and one someone turns off.
DOCUMENTED_ALIASES = {
    "slice": {"source"},
    "pan": set(),
    "audioCapture": set(),
}


def extract_actions(cpp_path, stem):
    """The action names `<stem>ActionList()` advertises, in code order."""
    with open(cpp_path, encoding="utf-8") as f:
        src = f.read()
    for m in _ACTION_LIST_RE.finditer(src):
        if m.group("stem") == stem:
            joined = _join_literals(m.group("body"))
            return [a for a in joined.split("|") if a]
    raise SystemExit(
        f"error: could not parse {stem}ActionList() from {cpp_path} — the "
        "docs-vs-code action lint cannot run, and silently skipping it is how "
        "the list drifted in the first place.")


def documented_actions(md_text, heading):
    r"""Action names in the first column of `heading`'s *action* table(s).

    A verb's section can carry more than one table (``### `audioCapture` `` also
    has a "Capture points" table). Only rows of a table whose header's first
    cell names "action" are counted, so a neighbouring ``| point | ... |``
    table does not feed phantom actions into the drift check. Header rows are
    skipped structurally — the row before the ``|---|`` separator.
    """
    lines = md_text.splitlines()
    start = None
    for i, ln in enumerate(lines):
        if ln.strip() == heading:
            start = i
            break
    if start is None:
        raise SystemExit(
            f"error: no '{heading}' section in the docs — its action lint "
            "cannot run.")
    found = set()
    prev_line = ""
    in_action_table = False
    for ln in lines[start + 1:]:
        # Stop at the next section of the same level; #### subsections belong
        # to this verb and may legitimately carry rows.
        if ln.startswith("### ") and ln.strip() != heading:
            break
        if not ln.startswith("|"):
            in_action_table = False
            prev_line = ln
            continue
        if re.fullmatch(r'\|[\s|:-]+', ln.strip()):
            # Separator: prev_line was this table's header. Count its rows only
            # when the first header cell is about actions.
            header_first = prev_line.split("|")[1] if "|" in prev_line else ""
            header_first = header_first.replace("`", "").strip().lower()
            in_action_table = "action" in header_first
            continue
        if in_action_table:
            first_cell = ln.split("|")[1]
            # Allow digits after the first letter so `probeNr2Stereo` counts.
            found.update(re.findall(r'`([a-zA-Z][a-zA-Z0-9]*)`', first_cell))
        prev_line = ln
    return found


def find_duplicate_headings(md_text):
    # Track fenced code blocks so a ``` … ### foo … ``` example isn't mistaken
    # for a real heading — otherwise this lint could block CI on doc content
    # it was never meant to police.
    seen, dups = {}, []
    in_fence = False
    for ln in md_text.splitlines():
        if ln.lstrip().startswith("```"):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        m = re.match(r'^(#{3,4})\s+(.*\S)\s*$', ln)
        if not m:
            continue
        title = m.group(2).strip()
        if title in seen:
            dups.append(title)
        seen[title] = seen.get(title, 0) + 1
    return dups


def splice(md_text, block):
    """Replace the marker block, or append a new section if absent."""
    if BEGIN in md_text and END in md_text:
        pre = md_text[:md_text.index(BEGIN)]
        post = md_text[md_text.index(END) + len(END):]
        return pre + block + post
    # First run: append a new appendix section before the trailing newline.
    section = ("\n---\n\n"
               "## Verb registry (auto-generated)\n\n"
               "The complete registry, generated from the `add(...)` table in "
               "`AutomationServer.cpp` by `tools/gen_bridge_docs.py`. CI fails "
               "if this drifts from the code.\n\n"
               + block + "\n")
    return md_text.rstrip("\n") + "\n" + section


def main():
    ap = argparse.ArgumentParser(description="Sync the bridge verb docs table.")
    ap.add_argument("--check", action="store_true",
                    help="exit 1 if the docs block is stale or a heading is duplicated")
    args = ap.parse_args()

    verbs = extract_registry(CPP)
    if not verbs:
        print(f"error: no verbs parsed from {CPP}", file=sys.stderr)
        return 2

    with open(DOCS, encoding="utf-8") as f:
        md = f.read()

    want = splice(md, generated_block(verbs))
    # Lint the on-disk doc (`md`), not the regenerated `want`: the generated
    # block is a table with no ###/#### headings, so scanning either is
    # equivalent — but linting `md` keeps "does the committed doc have dups?"
    # honest regardless of what the generator would produce.
    dups = find_duplicate_headings(md)

    # {verb: (advertised_actions, undocumented, stale_docs)}, in registry order.
    action_audit = {}
    total_actions = 0
    for verb, (stem, heading) in SUBACTION_VERBS.items():
        advertised = extract_actions(CPP, stem)
        documented = documented_actions(md, heading)
        undoc = [a for a in advertised if a not in documented]
        # ...and the other direction: an action the docs still describe after
        # the code stopped advertising it. A reader cannot tell a removed action
        # from a working one; the drift that produced #5102 ran this way too.
        stale = sorted(documented - set(advertised) - DOCUMENTED_ALIASES[verb])
        action_audit[verb] = (advertised, undoc, stale)
        total_actions += len(advertised)

    if args.check:
        problems = []
        if want != md:
            problems.append(
                f"docs verb table is STALE ({len(verbs)} verbs in the registry) "
                "— run tools/gen_bridge_docs.py")
        if dups:
            problems.append("duplicate detail-section heading(s): "
                            + ", ".join(sorted(set(dups))))
        for verb, (_adv, undoc, stale) in action_audit.items():
            if undoc:
                problems.append(
                    f"`{verb}` action(s) advertised by {verb}ActionList() but "
                    f"absent from the `### {verb}` action table in the docs: "
                    + ", ".join(undoc)
                    + " — document them (this table cannot be generated; the "
                      "per-action prose is hand-written on purpose)")
            if stale:
                problems.append(
                    f"`{verb}` action(s) documented but NOT advertised by "
                    f"{verb}ActionList(): " + ", ".join(stale)
                    + " — remove the row, or add the action back to the code "
                      f"(known aliases live in DOCUMENTED_ALIASES['{verb}'])")
        if problems:
            for p in problems:
                print("FAIL:", p, file=sys.stderr)
            return 1
        print(f"ok: docs verb table matches the registry ({len(verbs)} verbs), "
              f"no duplicate headings, all {total_actions} sub-actions across "
              f"{len(SUBACTION_VERBS)} verbs documented")
        return 0

    if dups:
        print("warning: duplicate detail-section heading(s): "
              + ", ".join(sorted(set(dups))), file=sys.stderr)
    for verb, (_adv, undoc, stale) in action_audit.items():
        if undoc:
            print(f"warning: `{verb}` action(s) missing from the docs table: "
                  + ", ".join(undoc), file=sys.stderr)
        if stale:
            print(f"warning: `{verb}` action(s) documented but not advertised: "
                  + ", ".join(stale), file=sys.stderr)
    if want != md:
        with open(DOCS, "w", encoding="utf-8") as f:
            f.write(want)
        print(f"updated {os.path.relpath(DOCS, REPO)} ({len(verbs)} verbs)")
    else:
        print(f"already up to date ({len(verbs)} verbs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
