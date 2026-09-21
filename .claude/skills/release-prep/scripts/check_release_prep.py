#!/usr/bin/env python3
"""check_release_prep.py — the release-prep validation list as one command.

Run from the root of the prep worktree after the six files are edited. Every
check prints ``PASS``, ``FAIL``, ``WARN``, ``INFO`` or ``SKIP`` with the detail
a reviewer would ask for; the exit status is non-zero when any check FAILs.

Standard library plus ``git`` (and ``appstreamcli`` or ``xmllint`` when
present) only. Runs on Windows, macOS and Linux.

Usage::

    python check_release_prep.py [--version 26.9.4] [--prev v26.9.3]
                                 [--base origin/main] [--mapping mapping.json]

``--version`` defaults to the ``project(AetherSDR VERSION …)`` line;
``--prev`` to the highest ``v*`` tag below it; ``--base`` to ``origin/main``.
``--mapping`` is the file written by ``release_contents.py --json`` and
enables the citation and count checks (run through that script's
``--check``); without it those checks are SKIPped, not passed.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

VERSION_RE = r"\d{2}\.\d{1,2}\.\d+(?:\.\d+)?"
METAINFO = Path("packaging/linux/io.github.aethersdr.aethersdr.metainfo.xml")
SIX = [
    "CMakeLists.txt", "README.md", "AGENTS.md", "CHANGELOG.md", "ROADMAP.md",
    METAINFO.as_posix(),
]
FORBIDDEN_PREFIXES = ("src/", "tests/", "third_party/", ".github/workflows/")

results: list[tuple[str, str, str]] = []


def report(status: str, name: str, detail: str = "") -> None:
    results.append((status, name, detail))
    line = f"{status:<4} {name}"
    if detail:
        line += f"\n     {detail.replace(chr(10), chr(10) + '     ')}"
    print(line)


def run(argv: list[str], check: bool = False) -> subprocess.CompletedProcess[str]:
    proc = subprocess.run(argv, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if check and proc.returncode != 0:
        sys.exit(f"error: {' '.join(argv)}\n{proc.stderr.strip()}")
    return proc


def git(*args: str, check: bool = True) -> str:
    return run([shutil.which("git") or "git", *args], check=check).stdout


def read(path: str | Path) -> str:
    return Path(path).read_text(encoding="utf-8")


def vkey(v: str) -> tuple[int, ...]:
    return tuple(int(x) for x in v.lstrip("v").split("."))


# ------------------------------------------------------------------ helpers

def cmake_version() -> str | None:
    m = re.search(r"project\(AetherSDR VERSION (" + VERSION_RE + ")", read("CMakeLists.txt"))
    return m.group(1) if m else None


def previous_tag(version: str) -> str | None:
    tags = [t for t in git("tag", "--sort=-v:refname").split() if t.startswith("v")]
    for t in tags:
        try:
            if vkey(t) < vkey(version):
                return t
        except ValueError:
            continue
    return None


def changelog_sections(text: str) -> list[tuple[str, str | None, int, int]]:
    """[(version, date, start, end)] for every `## [..]` heading."""
    heads = list(re.finditer(r"^## \[(?P<v>[^\]]+)\](?:\s+[—-]+\s+(?P<d>\d{4}-\d{2}-\d{2}))?[^\n]*$", text, re.M))
    out = []
    for i, h in enumerate(heads):
        end = heads[i + 1].start() if i + 1 < len(heads) else len(text)
        out.append((h.group("v"), h.group("d"), h.start(), end))
    return out


def slugify(heading: str) -> str:
    """GitHub's anchor slug: lowercase, drop punctuation, spaces to hyphens."""
    text = re.sub(r"[`*_]", "", heading.strip()).lower()
    text = re.sub(r"[^\w\s-]", "", text)
    return re.sub(r"\s+", "-", text).strip("-")


def headings(text: str) -> list[str]:
    return [m.group(1) for m in re.finditer(r"^#{1,6}\s+(.+?)\s*$", text, re.M)]


# ------------------------------------------------------------------- checks

def check_versions(version: str) -> str | None:
    """All five version spots plus the ROADMAP cycle heading. Returns the CHANGELOG date."""
    spots = {
        "CMakeLists.txt": cmake_version(),
        "README.md": (re.search(r"\*\*Current version: (" + VERSION_RE + r")\*\*", read("README.md")) or [None, None])[1],
        "AGENTS.md": (re.search(r"^Current version: \*\*(" + VERSION_RE + r")\*\*\.", read("AGENTS.md"), re.M) or [None, None])[1],
        "ROADMAP.md (cycle heading)": (re.search(r"^## Current cycle: post-v(" + VERSION_RE + ")", read("ROADMAP.md"), re.M) or [None, None])[1],
    }
    sections = changelog_sections(read("CHANGELOG.md"))
    released = [s for s in sections if s[0].lower() != "unreleased"]
    spots["CHANGELOG.md (newest section)"] = released[0][0].lstrip("v") if released else None
    date = released[0][1] if released else None
    try:
        root = ET.parse(METAINFO).getroot()
        rel = root.find("releases")
        first = rel[0] if rel is not None and len(rel) else None
        spots[METAINFO.as_posix() + " (first <release>)"] = first.get("version") if first is not None else None
        meta_date = first.get("date") if first is not None else None
    except ET.ParseError as e:
        spots[METAINFO.as_posix()] = f"unparseable ({e})"
        meta_date = None
    bad = {k: v for k, v in spots.items() if v != version}
    if bad:
        report("FAIL", f"all six version spots say {version}", "\n".join(f"{k}: {v}" for k, v in bad.items()))
    else:
        report("PASS", f"all six version spots say {version}", ", ".join(spots))
    if date and meta_date:
        if date == meta_date:
            report("PASS", "CHANGELOG heading date equals metainfo release date", date)
        else:
            report("FAIL", "CHANGELOG heading date equals metainfo release date", f"CHANGELOG {date} vs metainfo {meta_date}")
    else:
        report("FAIL", "release date present in CHANGELOG heading and metainfo", f"CHANGELOG {date}, metainfo {meta_date}")
    return date


def check_metainfo() -> None:
    tool = shutil.which("appstreamcli")
    if tool:
        p = run([tool, "validate", "--no-net", METAINFO.as_posix()])
        if p.returncode != 0:
            p = run([tool, "validate", METAINFO.as_posix()])
        out = (p.stdout + p.stderr).strip()
        report("PASS" if p.returncode == 0 else "FAIL", "appstreamcli validate", out.splitlines()[-1] if out else "")
    elif shutil.which("xmllint"):
        p = run([shutil.which("xmllint") or "xmllint", "--noout", METAINFO.as_posix()])
        report("PASS" if p.returncode == 0 else "FAIL", "xmllint --noout (appstreamcli not installed)", (p.stderr or "").strip())
    else:
        try:
            ET.parse(METAINFO)
            report("PASS", "metainfo XML parses (neither appstreamcli nor xmllint installed)")
        except ET.ParseError as e:
            report("FAIL", "metainfo XML parses", str(e))
    try:
        rel = ET.parse(METAINFO).getroot().find("releases")
        entries = [(r.get("version"), r.get("date")) for r in (rel if rel is not None else [])]
    except ET.ParseError:
        return
    dates = [d for _, d in entries if d]
    versions = [v for v, _ in entries if v]
    if dates == sorted(dates, reverse=True) and len(set(versions)) == len(versions):
        report("PASS", "metainfo <release> list is date-descending with unique versions", f"{len(entries)} entries, newest {entries[0] if entries else '-'}")
    else:
        report("FAIL", "metainfo <release> list is date-descending with unique versions", str(entries[:5]))


def check_changelog_shape(version: str, prev: str, base: str) -> str:
    text = read("CHANGELOG.md")
    sections = changelog_sections(text)
    names = [s[0] for s in sections]
    if not names or names[0].lower() != "unreleased":
        report("FAIL", "`## [Unreleased]` is the first section", f"first heading: {names[:1]}")
    else:
        body = text[sections[0][2]:sections[0][3]].split("\n", 1)[1]
        if body.strip():
            report("FAIL", "`## [Unreleased]` is empty", "entries under it must be folded into the new section or dropped — a maintainer call")
        else:
            report("PASS", "`## [Unreleased]` stays and stays empty")
    if len(names) > 1 and names[1].lstrip("v") == version:
        report("PASS", f"`## [v{version}]` sits directly under `[Unreleased]`")
    else:
        report("FAIL", f"`## [v{version}]` sits directly under `[Unreleased]`", f"second heading: {names[1:2]}")
    if len(names) > 2 and names[2] == prev:
        report("PASS", f"the previous section is `[{prev}]`")
    else:
        report("WARN", f"the section below the new one is `[{prev}]`", f"found {names[2:3]} — check --prev")

    old = git("show", f"{base}:CHANGELOG.md")
    new_sec = next((s for s in sections if s[0].lstrip("v") == version), None)
    section_text = text[new_sec[2]:new_sec[3]] if new_sec else ""
    if new_sec:
        head_new = text[:new_sec[2]]
        tail_new = text[new_sec[3]:]
        if old == head_new + tail_new:
            report("PASS", "CHANGELOG diff is exactly the inserted section", f"{section_text.count(chr(10))} lines inserted, history below byte-identical to {base}")
        else:
            report("FAIL", "CHANGELOG diff is exactly the inserted section", f"lines outside the new section differ from {base}; run `git diff {base} -- CHANGELOG.md`")
    # An `[Unreleased]` block on the base that carried entries must be folded in
    # whole — v26.8.1 dropped one (#4687) while claiming everything was folded.
    old_sections = changelog_sections(old)
    if old_sections and old_sections[0][0].lower() == "unreleased":
        old_unreleased = old[old_sections[0][2]:old_sections[0][3]]
        carried = {int(n) for n in re.findall(r"(?<![\w#/])#(\d{3,6})\b", old_unreleased)}
        if carried:
            now = {int(n) for n in re.findall(r"(?<![\w#/])#(\d{3,6})\b", section_text)}
            lost = sorted(carried - now)
            if lost:
                report("FAIL", "every PR cited in the base's `[Unreleased]` block survives the fold-in",
                       "dropped: " + ", ".join(f"#{n}" for n in lost) + " — keeping or dropping one is a maintainer call")
            else:
                report("PASS", "every PR cited in the base's `[Unreleased]` block survives the fold-in", f"{len(carried)} carried over")
    hl = re.search(r"^### (.+)$", section_text, re.M)
    if hl and " · " in hl.group(1) or (hl and len(hl.group(1)) > 20):
        report("PASS", "new section opens with a `### <headline>`", hl.group(1)[:120])
    else:
        report("FAIL", "new section opens with a `### <headline>`", "no `###` headline found before the first topical section")
    if re.search(r"^\d+ merged changes from \d+ human contributors, AetherClaude and", section_text, re.M):
        report("PASS", "intro sentence has the house shape")
    else:
        report("FAIL", "intro sentence has the house shape", "`N merged changes from M human contributors, AetherClaude and K Dependabot updates within that total.`")
    if "### Contributors" in section_text:
        report("PASS", "new section has a `### Contributors` block")
    else:
        report("FAIL", "new section has a `### Contributors` block")
    return section_text


def check_markdown_hygiene(section_text: str) -> None:
    for name, body in (("README.md", read("README.md")), ("ROADMAP.md", read("ROADMAP.md")), ("CHANGELOG.md (new section)", section_text)):
        fences = len(re.findall(r"^\s*```", body, re.M))
        if fences % 2:
            report("FAIL", f"code fences balance in {name}", f"{fences} fence lines")
        else:
            report("PASS", f"code fences balance in {name}")
        lines = body.split("\n")
        dup_headers, dup_rules = [], []
        for i in range(1, len(lines)):
            a, b = lines[i - 1].strip(), lines[i].strip()
            if a and a == b and a.startswith("|"):
                dup_headers.append(i + 1)
            if re.fullmatch(r"\|(\s*:?-+:?\s*\|)+", b) and re.fullmatch(r"\|(\s*:?-+:?\s*\|)+", a):
                dup_headers.append(i + 1)
        stripped = [l.strip() for l in lines]
        for i in range(len(stripped)):
            if stripped[i] == "---":
                j = i + 1
                while j < len(stripped) and stripped[j] == "":
                    j += 1
                if j < len(stripped) and stripped[j] == "---":
                    dup_rules.append(i + 1)
        if dup_headers or dup_rules:
            report("FAIL", f"no duplicated table headers or doubled `---` in {name}",
                   f"duplicated table lines at {dup_headers or '-'}, doubled rules at {dup_rules or '-'}")
        else:
            report("PASS", f"no duplicated table headers or doubled `---` in {name}")


def check_links() -> None:
    for name in ("README.md", "ROADMAP.md"):
        text = read(name)
        own = {slugify(h) for h in headings(text)}
        broken = []
        for m in re.finditer(r"\[[^\]]*\]\(([^)\s]+)\)", text):
            target = m.group(1)
            if re.match(r"^[a-z]+:", target) or target.startswith("mailto:"):
                continue
            path, _, anchor = target.partition("#")
            if not path:
                if anchor and anchor not in own:
                    broken.append(f"#{anchor} (no such heading in {name})")
                continue
            p = Path(path)
            if not p.exists():
                broken.append(target)
                continue
            if anchor and p.suffix.lower() == ".md":
                if anchor not in {slugify(h) for h in headings(read(p))}:
                    broken.append(f"{target} (anchor missing)")
        if broken:
            report("FAIL", f"every local link in {name} resolves", "\n".join(broken))
        else:
            report("PASS", f"every local link in {name} resolves")


def check_removed_anchors(base: str) -> None:
    """A heading removed from README/ROADMAP may be linked from elsewhere in the tree (#5673 left one dead)."""
    dead = []
    for name in ("README.md", "ROADMAP.md"):
        old = {slugify(h) for h in headings(git("show", f"{base}:{name}"))}
        new = {slugify(h) for h in headings(read(name))}
        for gone in sorted(old - new):
            hits = git("grep", "-n", "-F", f"{name}#{gone}", "--", ".", ":!CHANGELOG.md", check=False).strip()
            if hits:
                dead.append(f"{name}#{gone} removed but still linked:\n{hits}")
    if dead:
        report("FAIL", "no heading removed from README/ROADMAP is still linked elsewhere", "\n".join(dead))
    else:
        report("PASS", "no heading removed from README/ROADMAP is still linked elsewhere")


def check_roadmap_markers(version: str) -> None:
    text = read("ROADMAP.md")
    m = re.search(r"^### Recently shipped\s*$(?P<body>.*?)(?=^## |\Z)", text, re.M | re.S)
    if not m:
        report("FAIL", "ROADMAP has a `### Recently shipped` section")
        return
    body = m.group("body")
    markers = re.findall(r"\((v" + VERSION_RE + r")\)", body)
    if not markers:
        report("FAIL", "Recently shipped entries carry `(vX.Y.Z)` markers")
        return
    keys = [vkey(x) for x in markers]
    if keys != sorted(keys, reverse=True):
        report("FAIL", "Recently shipped `(vX)` markers run newest-first", " ".join(markers[:12]) + " …")
    elif markers[0].lstrip("v") != version:
        report("FAIL", "Recently shipped leads with the version being cut", f"first marker {markers[0]}, expected v{version}")
    else:
        report("PASS", "Recently shipped `(vX)` markers run newest-first and lead with the new version",
               f"{markers.count('v' + version)} entries marked (v{version}); versions present: {', '.join(dict.fromkeys(markers))}")
    distinct = list(dict.fromkeys(markers))
    same_month = [d for d in distinct if vkey(d)[:2] == vkey(version)[:2]]
    patches = [vkey(d)[2] for d in same_month if len(vkey(d)) == 3]
    if patches and patches != list(range(patches[0], patches[-1] - 1, -1)):
        report("WARN", "no gap in the current cycle's `(vX)` markers", f"patches present: {patches}")
    cycle = f"v{vkey(version)[0]}.{vkey(version)[1]}.x"
    intro = body.strip().split("\n\n", 1)[0]
    if cycle in intro:
        report("PASS", f"Recently shipped intro names the current cycle ({cycle})")
    else:
        report("WARN", f"Recently shipped intro names the current cycle ({cycle})", f"intro: {intro.strip()[:140]} — month rollover text is a maintainer call")


def check_old_version_mentions(version: str, prev: str, base: str) -> None:
    prev_v = prev.lstrip("v")
    diff = git("diff", "--unified=0", base, "--", ".", ":!CHANGELOG.md")
    removed = [l[1:] for l in diff.splitlines() if l.startswith("-") and not l.startswith("---") and prev_v in l]
    expected = [
        re.compile(r"project\(AetherSDR VERSION " + re.escape(prev_v)),
        re.compile(r"\*\*Current version: " + re.escape(prev_v) + r"\*\*"),
        re.compile(r"^Current version: \*\*" + re.escape(prev_v) + r"\*\*\."),
        re.compile(r"^## Current cycle: post-v" + re.escape(prev_v)),
    ]
    stray = [l for l in removed if not any(p.search(l) for p in expected)]
    hit = [l for l in removed if any(p.search(l) for p in expected)]
    if stray:
        report("FAIL", f"only the four current-version lines lose the `{prev_v}` string outside CHANGELOG",
               "historical mentions must stay:\n" + "\n".join(stray))
    elif len(hit) < 4:
        report("WARN", f"only the four current-version lines lose the `{prev_v}` string outside CHANGELOG", f"only {len(hit)} of the four expected lines changed")
    else:
        report("PASS", f"only the four current-version lines lose the `{prev_v}` string outside CHANGELOG")


def check_scope(base: str) -> None:
    files = [f for f in git("diff", "--name-only", base).split("\n") if f]
    forbidden = [f for f in files if f.startswith(FORBIDDEN_PREFIXES)]
    extra = [f for f in files if f not in SIX and f not in forbidden]
    missing = [f for f in SIX if f not in files]
    if forbidden:
        report("FAIL", "diff touches no src/, tests/, third_party/ or .github/workflows/ path", "\n".join(forbidden))
    else:
        report("PASS", "diff touches no src/, tests/, third_party/ or .github/workflows/ path")
    if missing:
        report("FAIL", "all six release files are in the diff", "missing: " + ", ".join(missing))
    else:
        report("PASS", "all six release files are in the diff")
    if extra:
        report("WARN", "files beyond the six (each needs a reason in the PR body's scope table)", "\n".join(extra))
    p = run([shutil.which("git") or "git", "diff", "--check", base])
    report("PASS" if p.returncode == 0 else "FAIL", "git diff --check", p.stdout.strip()[:400])


def check_family_claims(section_text: str) -> None:
    """Maturity wording per radio family across the three prose files — printed for the eye, not judged."""
    families = ["Hermes-Lite 2", "Icom", "ANAN", "RTL-SDR"]
    words = r"(experimental|early|supported|receive-only|unverified|verified|transmit confirmation|on the air|Supported)"
    rows = []
    for fam in families:
        for name, body in (("README", read("README.md")), ("ROADMAP", read("ROADMAP.md")), ("CHANGELOG", section_text)):
            found = set()
            for line in body.split("\n"):
                if fam.lower() in line.lower():
                    found.update(w.lower() for w in re.findall(words, line))
            rows.append(f"{fam:<14} {name:<9} {', '.join(sorted(found)) or '-'}")
    report("INFO", "maturity wording per family across README / ROADMAP / new CHANGELOG section — read these against each other",
           "\n".join(rows))


def check_citations(mapping: str | None) -> None:
    if not mapping:
        report("SKIP", "every PR in the range is cited (needs --mapping from release_contents.py --json)")
        return
    script = Path(__file__).with_name("release_contents.py")
    p = run([sys.executable, str(script), "--check", "CHANGELOG.md", "--json", mapping])
    out = (p.stdout + p.stderr).strip()
    report("PASS" if p.returncode == 0 else "FAIL", "release_contents.py --check (citations, intro and Contributors counts)", out)


# --------------------------------------------------------------------- main

def main(argv: list[str] | None = None) -> int:
    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(encoding="utf-8", errors="replace")  # type: ignore[attr-defined]
        except (AttributeError, ValueError):
            pass
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--version", help="version being cut, e.g. 26.9.4 (default: CMakeLists.txt)")
    ap.add_argument("--prev", help="previous tag, e.g. v26.9.3 (default: highest v* tag below --version)")
    ap.add_argument("--base", default="origin/main", help="ref the prep diff is measured against")
    ap.add_argument("--mapping", help="mapping.json from release_contents.py --json")
    args = ap.parse_args(argv)

    root = git("rev-parse", "--show-toplevel").strip()
    os.chdir(root)
    version = (args.version or cmake_version() or "").lstrip("v")
    if not version:
        sys.exit("error: could not read the version from CMakeLists.txt; pass --version")
    prev = args.prev or previous_tag(version)
    if not prev:
        sys.exit("error: no previous v* tag found; pass --prev")
    print(f"release-prep check: v{version}  previous {prev}  base {args.base}  tree {root}\n")
    if run([shutil.which("git") or "git", "merge-base", "--is-ancestor", prev, args.base]).returncode != 0:
        report("WARN", f"{prev} is an ancestor of {args.base}",
               "previous tag was cut off-main — its prep landed on main as a squashed duplicate; drop it from the range with release_contents.py --exclude")
    else:
        report("PASS", f"{prev} is an ancestor of {args.base}")

    check_versions(version)
    check_metainfo()
    section = check_changelog_shape(version, prev, args.base)
    check_citations(args.mapping)
    check_markdown_hygiene(section)
    check_links()
    check_removed_anchors(args.base)
    check_roadmap_markers(version)
    check_old_version_mentions(version, prev, args.base)
    check_scope(args.base)
    check_family_claims(section)

    fails = [r for r in results if r[0] == "FAIL"]
    warns = [r for r in results if r[0] == "WARN"]
    print(f"\n{len(results)} checks: {len(fails)} FAIL, {len(warns)} WARN")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
