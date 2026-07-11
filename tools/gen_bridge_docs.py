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
_ADD_RE = re.compile(
    r'\badd\(\s*"(?P<name>[^"]+)"\s*,\s*'
    r'\{(?P<aliases>[^}]*)\}\s*,\s*'
    r'"(?P<help>(?:[^"\\]|\\.)*)"',
    re.DOTALL,
)
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
        verbs.append((m.group("name"), aliases, _unescape(m.group("help"))))

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

    if args.check:
        problems = []
        if want != md:
            problems.append(
                f"docs verb table is STALE ({len(verbs)} verbs in the registry) "
                "— run tools/gen_bridge_docs.py")
        if dups:
            problems.append("duplicate detail-section heading(s): "
                            + ", ".join(sorted(set(dups))))
        if problems:
            for p in problems:
                print("FAIL:", p, file=sys.stderr)
            return 1
        print(f"ok: docs verb table matches the registry ({len(verbs)} verbs), "
              "no duplicate headings")
        return 0

    if dups:
        print("warning: duplicate detail-section heading(s): "
              + ", ".join(sorted(set(dups))), file=sys.stderr)
    if want != md:
        with open(DOCS, "w", encoding="utf-8") as f:
            f.write(want)
        print(f"updated {os.path.relpath(DOCS, REPO)} ({len(verbs)} verbs)")
    else:
        print(f"already up to date ({len(verbs)} verbs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
