#!/usr/bin/env python3
"""release_notes.py — derive the tag message and release body from the CHANGELOG.

Given a version, the previous tag and a CHANGELOG (a file, or the file at a
git ref), writes ``tagmsg.txt``, ``notes.md`` and ``title.txt`` in the shape
documented in ``../references/release-notes-format.md``. With ``--check`` it
fetches the published release through ``gh`` and diffs its title and body
against the derivation, so a hand edit shows.

Standard library plus ``git``; ``gh`` only for ``--check`` and
``--hotfix-base``. Runs on Windows, macOS and Linux.

Usage::

    python release_notes.py --version v26.9.5 --prev v26.9.4 \
        [--changelog CHANGELOG.md | --ref <sha>] --cut-by "Jeremy KK7GWY" \
        [--signoff "73, Jeremy KK7GWY & Claude (AI dev partner)"] \
        [--out-dir DIR] [--check]

    python release_notes.py --version v26.9.5.1 --prev v26.9.5 --hotfix-base v26.9.5 \
        --fix-file fix.md --cut-by "Jeremy KK7GWY" [--check]

``--prev`` for a hotfix is the previous *full* release (the base's previous
tag); ``--hotfix-base`` is the release the hotfix sits on. ``fix.md`` is the
``## The fix`` body: its first line is the bold lead sentence, the remaining
paragraphs the explanation. The blockquote's "who should upgrade" clause comes
from ``--affects`` (default: "use the affected feature").
"""

from __future__ import annotations

import argparse
import difflib
import json
import re
import shutil
import subprocess
import sys
import textwrap
from pathlib import Path
from typing import Any

REPO = "aethersdr/AetherSDR"
REPO_URL = f"https://github.com/{REPO}"

DOWNLOADS = (
    "### Downloads\n\n"
    "Tag-triggered CI publishes Linux AppImages (x86-64 and aarch64), macOS DMGs "
    "(Apple Silicon and Intel), and Windows installers and portable ZIPs as jobs "
    "finish. Assets may still be building when this release first appears. macOS "
    "signing and notarization, and release checksum and signature generation, run "
    "in their respective workflows.\n"
)

SIGNOFF_RE = re.compile(r"^73, .+\(AI dev partner\)\s*$")
HEADING_RE = re.compile(r"^## \[(?P<v>[^\]]+)\][^\n]*$", re.M)
INTRO_RE = re.compile(
    r"^(?P<n>\d+) merged changes from (?P<m>\d+) (?:human )?contributors?\b(?P<rest>.*)$",
    re.S,
)


# --------------------------------------------------------------------------- io

def _utf8_stdio() -> None:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")  # type: ignore[attr-defined]
        except (AttributeError, ValueError):
            pass  # not a TextIOWrapper (a captured or redirected stream); leave its encoding alone


def _tool(name: str) -> str:
    path = shutil.which(name)
    if not path:
        sys.exit(f"error: `{name}` is not on PATH")
    return path


def run(argv: list[str], *, check: bool = True) -> str:
    proc = subprocess.run(argv, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if check and proc.returncode != 0:
        sys.exit(f"error: {' '.join(argv)}\n{proc.stderr.strip()}")
    return proc.stdout


def gh_json(*args: str) -> Any:
    out = run([_tool("gh"), *args])
    return json.loads(out) if out.strip() else None


# ---------------------------------------------------------------- changelog

def load_changelog(args: argparse.Namespace) -> str:
    if args.ref:
        return run([_tool("git"), "show", f"{args.ref}:{args.changelog}"])
    path = Path(args.changelog)
    if not path.exists():
        sys.exit(f"error: {path} not found (pass --changelog or --ref)")
    return path.read_text(encoding="utf-8")


def changelog_section(text: str, version: str) -> str:
    heads = list(HEADING_RE.finditer(text))
    if not heads:
        sys.exit("error: no `## [..]` headings in the changelog")
    for i, h in enumerate(heads):
        if h.group("v").lstrip("v") == version.lstrip("v"):
            end = heads[i + 1].start() if i + 1 < len(heads) else len(text)
            return text[h.start():end]
    sys.exit(f"error: no `## [v{version.lstrip('v')}]` section in the changelog")


def split_section(section: str) -> tuple[str, str, str, str]:
    """Return (heading line, headline text, body without heading/headline/sign-off, sign-off)."""
    lines = section.rstrip("\n").split("\n")
    heading = lines[0]
    rest = lines[1:]
    headline = None
    for i, line in enumerate(rest):
        if line.startswith("### "):
            headline = line[4:].strip()
            rest = rest[:i] + rest[i + 1:]
            break
    if headline is None:
        sys.exit("error: the section has no `### <headline>` line")
    signoff = ""
    for i in range(len(rest) - 1, -1, -1):
        if rest[i].strip():
            if SIGNOFF_RE.match(rest[i].strip()):
                signoff = rest[i].strip()
                rest = rest[:i]
            break
    body = "\n".join(rest).strip("\n")
    return heading, headline, body, signoff


def first_clause(headline: str) -> str:
    return headline.split(" · ")[0].strip()


def intro_counts(body: str) -> tuple[int, int, str]:
    intro = body.split("\n\n", 1)[0].replace("\n", " ")
    m = INTRO_RE.match(intro)
    if not m:
        sys.exit("error: the intro paragraph does not start with `N merged changes from M human contributors`")
    return int(m.group("n")), int(m.group("m")), intro


def footer(prev: str, version: str, signoff: str, hotfix_base: str | None = None) -> str:
    v = "v" + version.lstrip("v")
    if hotfix_base:
        base = "v" + hotfix_base.lstrip("v")
        diff = (f"**Full diff:** [{base}...{v}]({REPO_URL}/compare/{base}...{v}) (this hotfix) · "
                f"[{prev}...{v}]({REPO_URL}/compare/{prev}...{v}) (since the last full release).")
    else:
        diff = f"**Full diff:** [{prev}...{v}]({REPO_URL}/compare/{prev}...{v})."
    return (f"{DOWNLOADS}\n{diff} See [verification instructions]"
            f"({REPO_URL}/blob/{v}/docs/VERIFYING-RELEASES.md).\n\n{signoff}\n")


# ------------------------------------------------------------------- derive

def derive_release(args: argparse.Namespace) -> tuple[str, str, str]:
    """Return (title, body, tag message) for a normal release."""
    v = "v" + args.version.lstrip("v")
    section = changelog_section(load_changelog(args), v)
    _, headline, body, signoff = split_section(section)
    signoff = args.signoff or signoff
    if not signoff:
        sys.exit("error: no `73, …` sign-off in the section; pass --signoff")
    title = f"AetherSDR {v} — {first_clause(headline)}"
    notes = f"{body}\n\n{footer(args.prev, v, signoff)}"
    n, m, intro = intro_counts(body)
    summary = intro
    sentence_end = re.search(r"within that total\.\s*|\.\s+", summary)
    if sentence_end:
        summary = summary[sentence_end.end():].strip()
    para = textwrap.fill(summary, width=76)
    tagmsg = (f"{title}\n\n{para}\n\n"
              f"{n} merged changes from {m} human contributors, AetherClaude and Dependabot.\n"
              f"Cut by {args.cut_by}.\n")
    return title, notes, tagmsg


def derive_hotfix(args: argparse.Namespace) -> tuple[str, str, str]:
    v = "v" + args.version.lstrip("v")
    base = "v" + args.hotfix_base.lstrip("v")
    fix = Path(args.fix_file).read_text(encoding="utf-8").strip("\n") if args.fix_file else ""
    if not fix:
        sys.exit("error: --fix-file is required for a hotfix body")
    lead = fix.split("\n", 1)[0].strip()
    section = changelog_section(load_changelog(args), v)
    _, headline, _, signoff = split_section(section)
    signoff = args.signoff or signoff
    if not signoff:
        sys.exit("error: no `73, …` sign-off in the section; pass --signoff")
    rel = gh_json("api", f"repos/{REPO}/releases/tags/{base}")
    base_body = (rel.get("body") or "").replace("\r\n", "\n")
    cut = base_body.find("### Downloads")
    if cut < 0:
        cut = base_body.find("**Downloads**")
    base_notes = base_body[:cut].rstrip("\n") if cut >= 0 else base_body.rstrip("\n")
    # a retired-shape base carries its sign-off and a rule above its Downloads
    # block; the hotfix body signs off once, at the end
    base_lines = base_notes.split("\n")
    while base_lines and (not base_lines[-1].strip() or base_lines[-1].strip() == "---" or SIGNOFF_RE.match(base_lines[-1].strip())):
        base_lines.pop()
    base_notes = "\n".join(base_lines)
    title = f"AetherSDR {v} — {first_clause(headline)}"
    quote = (f"> **Hotfix release.** This is [{base}]({REPO_URL}/releases/tag/{base}) plus one fix and "
             f"nothing else. If you are on {base} and {args.affects}, update. If you are on {args.prev} or "
             f"earlier, this is your upgrade target — the full {base} notes follow below.")
    notes = (f"{quote}\n\n## The fix\n\n{fix}\n\n---\n\n# Everything in {base}\n\n{base_notes}\n\n"
             f"{footer(args.prev, v, signoff, hotfix_base=base)}")
    lead_text = re.sub(r"\*\*|\s*\([^)]*#\d+[^)]*\)\s*$", "", lead).strip()
    para = textwrap.fill(f"One fix on top of {base}: {lead_text}", width=76)
    tagmsg = f"{title}\n\n{para}\n\nCut by {args.cut_by}.\n"
    return title, notes, tagmsg


# -------------------------------------------------------------------- check

def _norm(text: str) -> list[str]:
    return [line.rstrip() for line in text.replace("\r\n", "\n").strip("\n").split("\n")]


def check(args: argparse.Namespace, title: str, notes: str) -> int:
    v = "v" + args.version.lstrip("v")
    rel = gh_json("api", f"repos/{REPO}/releases/tags/{v}")
    rc = 0
    if rel.get("name") != title:
        print(f"FAIL release title\n     published: {rel.get('name')!r}\n     derived:   {title!r}")
        rc = 1
    else:
        print(f"PASS release title equals the tag's first line\n     {title}")
    pub, der = _norm(rel.get("body") or ""), _norm(notes)
    if pub == der:
        print(f"PASS release body equals the derivation ({len(der)} lines)")
    else:
        rc = 1
        diff = list(difflib.unified_diff(der, pub, "derived", "published", lineterm="", n=1))
        print(f"FAIL release body differs from the derivation ({len(diff)} diff lines)")
        for line in diff[:120]:
            print("     " + line[:200])
        if len(diff) > 120:
            print(f"     … {len(diff) - 120} more")
    tag_obj = gh_json("api", f"repos/{REPO}/git/ref/tags/{v}")
    if tag_obj and tag_obj.get("object", {}).get("type") == "tag":
        tag = gh_json("api", f"repos/{REPO}/git/tags/{tag_obj['object']['sha']}")
        first = (tag.get("message") or "").split("\n", 1)[0].strip()
        if first == title:
            print("PASS tag message first line equals the release title")
        else:
            print(f"FAIL tag message first line differs\n     tag:   {first!r}\n     title: {title!r}")
            rc = 1
    else:
        print("WARN tag is not an annotated tag object (or not pushed yet); first-line check skipped")
    return rc


# --------------------------------------------------------------------- main

def main(argv: list[str] | None = None) -> int:
    _utf8_stdio()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--version", required=True, help="the version being cut, with or without v")
    ap.add_argument("--prev", required=True, help="the previous tag (previous full release for a hotfix)")
    ap.add_argument("--changelog", default="CHANGELOG.md", help="path to CHANGELOG.md (default: CHANGELOG.md)")
    ap.add_argument("--ref", help="read the changelog from this git ref instead of the working tree")
    ap.add_argument("--cut-by", default="", help='"<name> <callsign>" for the tag message (required unless --check only)')
    ap.add_argument("--signoff", help="override the `73, …` sign-off line (default: the one in the section)")
    ap.add_argument("--hotfix-base", help="for a hotfix: the base release tag whose notes are repeated")
    ap.add_argument("--fix-file", help="for a hotfix: markdown for `## The fix` (first line is the bold lead)")
    ap.add_argument("--affects", default="use the affected feature",
                    help='for a hotfix: who should upgrade, as in "use TCI"')
    ap.add_argument("--out-dir", default=".", help="where to write tagmsg.txt, notes.md and title.txt")
    ap.add_argument("--check", action="store_true", help="diff the published release against the derivation")
    ap.add_argument("--no-write", action="store_true", help="derive and print only; write nothing")
    args = ap.parse_args(argv)

    if not args.cut_by and not args.check:
        ap.error("--cut-by is required to write tagmsg.txt (or pass --check)")
    args.cut_by = args.cut_by or "<name> <callsign>"
    args.prev = "v" + args.prev.lstrip("v")

    title, notes, tagmsg = derive_hotfix(args) if args.hotfix_base else derive_release(args)

    if not args.no_write:
        out = Path(args.out_dir)
        out.mkdir(parents=True, exist_ok=True)
        (out / "tagmsg.txt").write_text(tagmsg, encoding="utf-8", newline="\n")
        (out / "notes.md").write_text(notes, encoding="utf-8", newline="\n")
        (out / "title.txt").write_text(title + "\n", encoding="utf-8", newline="\n")
        print(f"wrote {out / 'tagmsg.txt'}, {out / 'notes.md'}, {out / 'title.txt'}\n")
    print("--- title.txt\n" + title + "\n--- tagmsg.txt\n" + tagmsg + "--- notes.md (first and last 6 lines)")
    lines = notes.rstrip("\n").split("\n")
    print("\n".join(lines[:6]) + "\n…\n" + "\n".join(lines[-6:]) + "\n")

    if args.check:
        return check(args, title, notes)
    return 0


if __name__ == "__main__":
    sys.exit(main())
