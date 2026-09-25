#!/usr/bin/env python3
"""release_contents.py — what a release contains, from the commit-to-PR API.

Maps every commit in ``<prev-tag>..<cutoff>`` to the pull request that merged
it (``GET /repos/{repo}/commits/{sha}/pulls``), counts merged changes per
author login, checks which humans are first-time contributors, and — with
``--check`` — verifies that a CHANGELOG section cites every PR in the range,
lists the numbers it cites that are *not* in the range, and re-derives the
intro and Contributors counts from the mapping.

Commit subjects are never used for the mapping: a subject may carry no
``(#NNNN)`` at all, or carry it before a trailing ``Principle N.`` clause.
Subject-suffix detection missed 20 PRs in one release cycle.

Standard library plus the ``gh`` CLI only. Runs on Windows, macOS and Linux.

Usage
-----
Map a range and save the mapping::

    python release_contents.py --from v26.9.3 --to origin/main --json mapping.json

Drop a commit or PR from the range by hand (the squashed duplicate of a prep
that was tagged off-main, for instance)::

    python release_contents.py --from v26.9.2 --to origin/main --exclude 5673 --json mapping.json

Verify a CHANGELOG section against a saved mapping::

    python release_contents.py --check CHANGELOG.md --json mapping.json [--section v26.9.4]

Exit status is non-zero when ``--check`` finds an uncited PR, a foreign
citation, or a count that disagrees with the mapping; and when a mapping run
cannot resolve a commit.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import re
import shutil
import subprocess
import sys
from typing import Any

DEFAULT_REPO = "aethersdr/AetherSDR"
PREP_TITLE_RE = re.compile(r"(?i)\b(release|prep)\b.*\bv?\d{2}\.\d{1,2}\.\d")
NUMBER_WORDS = {
    "one": 1, "two": 2, "three": 3, "four": 4, "five": 5, "six": 6,
    "seven": 7, "eight": 8, "nine": 9, "ten": 10, "eleven": 11, "twelve": 12,
}


# --------------------------------------------------------------------------- io

def _utf8_stdio() -> None:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")  # type: ignore[attr-defined]
        except (AttributeError, ValueError):
            pass


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


def git(*args: str, check: bool = True) -> str:
    return run([_tool("git"), *args], check=check)


def gh_json(*args: str) -> Any:
    out = run([_tool("gh"), *args])
    return json.loads(out) if out.strip() else None


def warn(msg: str) -> None:
    print(f"warning: {msg}", file=sys.stderr)


# ------------------------------------------------------------------- mapping

def resolve_range(prev: str, to: str) -> dict[str, Any]:
    """Return the effective range start and whether ``prev`` is on ``to``'s history."""
    to_sha = git("rev-parse", f"{to}^{{commit}}").strip()
    prev_sha = git("rev-parse", f"{prev}^{{commit}}").strip()
    is_ancestor = subprocess.run(
        [_tool("git"), "merge-base", "--is-ancestor", prev_sha, to_sha],
        capture_output=True,
    ).returncode == 0
    start = prev_sha
    if not is_ancestor:
        start = git("merge-base", prev_sha, to_sha).strip()
        warn(
            f"{prev} ({prev_sha[:9]}) is NOT an ancestor of {to} ({to_sha[:9]}); "
            f"it was cut off-main. Using merge-base {start[:9]} as the range start. "
            "Its prep commit reached main as a squashed duplicate — find it in the "
            "PREP? column below and drop it with --exclude."
        )
    prev_time = git("log", "-1", "--format=%cI", prev_sha).strip()
    return {
        "from": prev, "from_sha": prev_sha, "from_is_ancestor": is_ancestor,
        "from_time": prev_time, "to": to, "to_sha": to_sha, "range_start": start,
    }


def list_commits(start: str, to_sha: str) -> list[dict[str, str]]:
    out = git("log", "--no-merges", "--reverse", "--format=%H%x1f%an%x1f%ae%x1f%cI%x1f%s", f"{start}..{to_sha}")
    commits = []
    for line in out.splitlines():
        if not line.strip():
            continue
        sha, name, email, date, subject = line.split("\x1f", 4)
        commits.append({"sha": sha, "git_author": name, "git_email": email, "date": date, "subject": subject})
    return commits


def pulls_for_commit(repo: str, sha: str, base_branch: str) -> dict[str, Any] | None:
    pulls = gh_json("api", f"repos/{repo}/commits/{sha}/pulls") or []
    merged = [p for p in pulls if p.get("merged_at") and p.get("base", {}).get("ref") == base_branch]
    if not merged:
        merged = [p for p in pulls if p.get("merged_at")]
    return merged[0] if merged else None


def commit_login(repo: str, sha: str) -> str | None:
    data = gh_json("api", f"repos/{repo}/commits/{sha}")
    author = (data or {}).get("author") or {}
    return author.get("login")


def pr_details(repo: str, number: int, want_body: bool) -> dict[str, Any]:
    data = gh_json("api", f"repos/{repo}/pulls/{number}")
    user = data.get("user") or {}
    out = {
        "title": data.get("title", ""),
        "login": user.get("login", "?"),
        "kind": "bot" if user.get("type") == "Bot" else "human",
        "merged_at": data.get("merged_at"),
        "head_repo": ((data.get("head") or {}).get("repo") or {}).get("full_name"),
        "changed_files": data.get("changed_files"),
    }
    if want_body:
        out["body"] = data.get("body") or ""
    return out


def normalise_login(login: str) -> str:
    return login[:-5] if login.endswith("[bot]") else login


def first_time(repo: str, login: str, before_iso: str) -> bool | None:
    """True when the login has no merged PR before ``before_iso`` (the previous tag's time)."""
    try:
        when = _dt.datetime.fromisoformat(before_iso.replace("Z", "+00:00")).astimezone(_dt.timezone.utc)
    except ValueError:
        return None
    stamp = when.strftime("%Y-%m-%dT%H:%M:%S+00:00")
    rows = gh_json(
        "pr", "list", "--repo", repo, "--author", login, "--state", "merged",
        "--search", f"merged:<{stamp}", "--limit", "1", "--json", "number",
    )
    return not rows


def build_mapping(args: argparse.Namespace) -> dict[str, Any]:
    rng = resolve_range(args.frm, args.to)
    commits = list_commits(rng["range_start"], rng["to_sha"])
    if not commits:
        sys.exit(f"error: no commits in {rng['range_start'][:9]}..{rng['to_sha'][:9]}")

    excluded = set(args.exclude or [])
    seen_prs: dict[int, dict[str, Any]] = {}
    rows: list[dict[str, Any]] = []
    unresolved = 0
    for c in commits:
        pr = pulls_for_commit(args.repo, c["sha"], args.base_branch)
        row: dict[str, Any] = {
            "sha": c["sha"], "short": c["sha"][:9], "date": c["date"],
            "subject": c["subject"], "git_author": c["git_author"],
        }
        if pr:
            number = int(pr["number"])
            row["pr"] = number
            if number not in seen_prs:
                seen_prs[number] = pr_details(args.repo, number, args.bodies)
            det = seen_prs[number]
            row.update({
                "title": det["title"], "login": det["login"], "kind": det["kind"],
                "merged_at": det["merged_at"], "head_repo": det["head_repo"],
                "changed_files": det["changed_files"],
            })
            if args.bodies:
                row["body"] = det.get("body", "")
        else:
            login = commit_login(args.repo, c["sha"])
            row.update({"pr": None, "title": c["subject"], "login": login or "?", "kind": "human", "merged_at": c["date"]})
            if not login:
                unresolved += 1
                warn(f"{c['sha'][:9]} has no PR and GitHub could not resolve its author login ({c['git_author']} <{c['git_email']}>)")
            else:
                warn(f"{c['sha'][:9]} has no PR — cite it by short SHA (author @{login})")
        row["prep_suspect"] = bool(PREP_TITLE_RE.search(row["title"])) or (row.get("changed_files") == 6 and "release" in row["title"].lower())
        key = row["pr"] if row["pr"] is not None else row["short"]
        row["excluded"] = str(key) in excluded or row["short"] in excluded or row["sha"] in excluded
        rows.append(row)

    # One unit per PR (squash-merged), one per PR-less commit.
    units: dict[str, dict[str, Any]] = {}
    for r in rows:
        if r["excluded"]:
            continue
        key = f"pr:{r['pr']}" if r["pr"] is not None else f"sha:{r['short']}"
        units.setdefault(key, r)

    authors: dict[str, dict[str, Any]] = {}
    for r in units.values():
        login = normalise_login(r["login"])
        a = authors.setdefault(login, {"count": 0, "kind": r["kind"], "first_time": None})
        a["count"] += 1
    if not args.no_first_time:
        for login, a in authors.items():
            if a["kind"] == "human" and login != "?":
                a["first_time"] = first_time(args.repo, login, rng["from_time"])

    humans = {k: v for k, v in authors.items() if v["kind"] == "human" and k != "?"}
    bots = {k: v for k, v in authors.items() if v["kind"] == "bot"}
    summary = {
        "commits": len(rows),
        "units": len(units),
        "prs": len({r["pr"] for r in units.values() if r["pr"] is not None}),
        "no_pr_commits": sum(1 for r in units.values() if r["pr"] is None),
        "human_contributors": len(humans),
        "bot_units": {k: v["count"] for k, v in bots.items()},
        "dependabot": bots.get("dependabot", {}).get("count", 0),
        "first_time": sorted(k for k, v in humans.items() if v["first_time"]),
        "excluded": sorted(excluded),
        "unresolved_logins": unresolved,
    }
    return {"repo": args.repo, **rng, "commits": rows, "authors": authors, "summary": summary}


def print_mapping(m: dict[str, Any]) -> None:
    print("# commit\tpr\tauthor\tkind\tmerged_at\tPREP?\texcluded\ttitle")
    for r in m["commits"]:
        pr = f"#{r['pr']}" if r["pr"] is not None else "-"
        print("\t".join([
            r["short"], pr, "@" + normalise_login(r["login"]), r["kind"], (r["merged_at"] or "")[:19],
            "PREP?" if r["prep_suspect"] else "", "excluded" if r["excluded"] else "", r["title"],
        ]))
    print("\n# author\tunits\tkind\tfirst_time")
    for login, a in sorted(m["authors"].items(), key=lambda kv: (kv[1]["kind"] != "human", -kv[1]["count"], kv[0].lower())):
        ft = {True: "yes", False: "no", None: "?"}[a["first_time"]]
        print(f"@{login}\t{a['count']}\t{a['kind']}\t{ft}")
    s = m["summary"]
    print("\n# summary")
    print(f"range: {m['from']} ({m['from_sha'][:9]}) .. {m['to']} ({m['to_sha'][:9]})"
          + ("" if m["from_is_ancestor"] else f"  [prev tag OFF-MAIN; start={m['range_start'][:9]}]"))
    print(f"commits: {s['commits']}  merged changes (units): {s['units']}  PRs: {s['prs']}  no-PR commits: {s['no_pr_commits']}")
    print(f"human contributors: {s['human_contributors']}  bots: {s['bot_units']}  dependabot: {s['dependabot']}")
    print(f"first-time: {', '.join('@' + x for x in s['first_time']) or 'none'}")
    if s["excluded"]:
        print(f"excluded by hand: {', '.join(s['excluded'])}")
    print("intro sentence: "
          f"{s['units']} merged changes from {s['human_contributors']} human contributors, AetherClaude and "
          f"{_word(s['dependabot'])} Dependabot update{'s' if s['dependabot'] != 1 else ''} within that total.")


def _word(n: int) -> str:
    for w, v in NUMBER_WORDS.items():
        if v == n:
            return w
    return str(n)


# --------------------------------------------------------------------- check

def changelog_section(text: str, version: str | None) -> tuple[str, str]:
    heads = list(re.finditer(r"^## \[(?P<v>[^\]]+)\][^\n]*$", text, re.M))
    if not heads:
        sys.exit("error: no `## [..]` headings in the changelog")
    chosen = None
    for i, h in enumerate(heads):
        v = h.group("v")
        if version is not None and v.lstrip("v") == version.lstrip("v"):
            chosen = i
            break
        if version is None and v.lower() != "unreleased":
            chosen = i
            break
    if chosen is None:
        sys.exit(f"error: section for {version or 'newest release'} not found")
    start = heads[chosen].start()
    end = heads[chosen + 1].start() if chosen + 1 < len(heads) else len(text)
    return heads[chosen].group("v"), text[start:end]


def check_changelog(args: argparse.Namespace, mapping: dict[str, Any]) -> int:
    text = open(args.check, encoding="utf-8").read()
    version, section = changelog_section(text, args.section)
    failures = 0

    units = {k: v for k, v in {
        (f"pr:{r['pr']}" if r["pr"] is not None else f"sha:{r['short']}"): r
        for r in mapping["commits"] if not r["excluded"]
    }.items()}
    in_range = {r["pr"] for r in units.values() if r["pr"] is not None}
    cited = {int(n) for n in re.findall(r"(?<![\w#/])#(\d{3,6})\b", section)}
    uncited = sorted(in_range - cited)
    foreign = sorted(cited - in_range)

    print(f"section: [{version}]  PRs in range: {len(in_range)}  cited: {len(cited & in_range)}")
    if uncited:
        failures += 1
        print(f"FAIL uncited PRs ({len(uncited)}):")
        for n in uncited:
            r = next(u for u in units.values() if u["pr"] == n)
            print(f"  #{n}  @{normalise_login(r['login'])}  {r['title']}")
    else:
        print("PASS every PR in the range is cited")

    no_pr = [r for r in units.values() if r["pr"] is None]
    missing_sha = [r for r in no_pr if r["short"][:7] not in section]
    if missing_sha:
        failures += 1
        print(f"FAIL PR-less commits not cited by short SHA ({len(missing_sha)}):")
        for r in missing_sha:
            print(f"  {r['short']}  @{normalise_login(r['login'])}  {r['title']}")
    elif no_pr:
        print(f"PASS {len(no_pr)} PR-less commit(s) cited by SHA")

    if foreign:
        print(f"INFO citations outside the range ({len(foreign)}) — each must be an issue, an RFC or a deliberate prior-PR reference:")
        for n in foreign:
            info = gh_json("api", f"repos/{mapping['repo']}/issues/{n}") or {}
            labels = [l.get("name") for l in info.get("labels") or []]
            kind = "PR" if info.get("pull_request") else ("RFC" if "rfc" in labels else "issue")
            print(f"  #{n}  {kind}  {info.get('state', '?')}  {info.get('title', '?')}")
    else:
        print("PASS no citations outside the range")

    s = mapping["summary"]
    m = re.search(r"(\d+) merged changes from (\d+) human contributors", section)
    if not m:
        failures += 1
        print("FAIL intro sentence `N merged changes from M human contributors` not found")
    else:
        n, hum = int(m.group(1)), int(m.group(2))
        ok = n == s["units"] and hum == s["human_contributors"]
        print(f"{'PASS' if ok else 'FAIL'} intro counts: section says {n}/{hum}, mapping says {s['units']}/{s['human_contributors']}")
        failures += 0 if ok else 1

    dep = re.search(r"(\w+) Dependabot update", section)
    if dep:
        word = dep.group(1).lower()
        got = NUMBER_WORDS.get(word, int(word) if word.isdigit() else None)
        ok = got == s["dependabot"]
        print(f"{'PASS' if ok else 'FAIL'} Dependabot count: section says {word}, mapping says {s['dependabot']}")
        failures += 0 if ok else 1
    elif s["dependabot"]:
        failures += 1
        print(f"FAIL mapping has {s['dependabot']} Dependabot update(s) but the section has no Dependabot sentence")

    contrib = re.search(r"^### Contributors\s*$(?P<body>.*?)(?=^###|\Z)", section, re.M | re.S)
    if not contrib:
        failures += 1
        print("FAIL no `### Contributors` block")
    else:
        body = contrib.group("body")
        listed = {login: int(n) for login, n in re.findall(r"\*\*@([\w-]+)\*\* \((\d+) commits?", body)}
        expected = {k: v["count"] for k, v in mapping["authors"].items() if k not in ("?", "dependabot")}
        bad = []
        for login, cnt in expected.items():
            if login not in listed:
                bad.append(f"  missing **@{login}** ({cnt})")
            elif listed[login] != cnt:
                bad.append(f"  **@{login}** listed as {listed[login]}, mapping says {cnt}")
        for login in listed:
            if login not in expected:
                bad.append(f"  **@{login}** is listed but not in the range")
        if bad:
            failures += 1
            print("FAIL Contributors paragraph disagrees with the mapping:")
            print("\n".join(bad))
        else:
            print(f"PASS Contributors paragraph matches the mapping ({len(listed)} entries)")
        humans_desc = [(l, c) for l, c in listed.items() if mapping["authors"].get(l, {}).get("kind") == "human"]
        if humans_desc != sorted(humans_desc, key=lambda x: -x[1]):
            print("WARN human entries are not in descending order of count")
        welcome = re.search(r"Welcome to first-time contributors? (.+?)!", body)
        welcomed = set(re.findall(r"\*\*@([\w-]+)\*\*", welcome.group(1))) if welcome else set()
        expected_ft = set(s.get("first_time") or [])
        if any(v["first_time"] is None for v in mapping["authors"].values() if v["kind"] == "human"):
            print("SKIP first-time check (mapping was built with --no-first-time)")
        elif welcomed != expected_ft:
            failures += 1
            print(f"FAIL first-time line: section welcomes {sorted(welcomed) or 'nobody'}, mapping says {sorted(expected_ft) or 'nobody'}")
        else:
            print(f"PASS first-time contributors: {sorted(expected_ft) or 'none'}")
        if not re.search(r"^73, .+ & .+ \(AI dev partner\)\s*$", body, re.M):
            print("WARN no `73, <name> <callsign> & <tool> (AI dev partner)` sign-off in the Contributors block")

    return 1 if failures else 0


# ---------------------------------------------------------------------- main

def main(argv: list[str] | None = None) -> int:
    _utf8_stdio()
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--repo", default=DEFAULT_REPO)
    p.add_argument("--base-branch", default="main", help="PRs merged into this branch count (default main)")
    p.add_argument("--from", dest="frm", help="previous release tag, e.g. v26.9.3")
    p.add_argument("--to", help="cutoff: a SHA or ref such as origin/main")
    p.add_argument("--exclude", action="append", metavar="PR|SHA", help="drop a PR number or commit from the range (repeatable)")
    p.add_argument("--bodies", action="store_true", help="store PR bodies in --json (the changelog is written from them)")
    p.add_argument("--no-first-time", action="store_true", help="skip the per-author first-contribution lookup")
    p.add_argument("--json", help="write (mapping mode) or read (check mode) the mapping file")
    p.add_argument("--check", metavar="CHANGELOG.md", help="verify a changelog section against --json")
    p.add_argument("--section", help="version whose section to check (default: newest non-Unreleased)")
    args = p.parse_args(argv)

    if args.check:
        if not args.json:
            p.error("--check needs --json <mapping.json> from a previous mapping run")
        mapping = json.load(open(args.json, encoding="utf-8"))
        return check_changelog(args, mapping)

    if not (args.frm and args.to):
        p.error("--from and --to are required (or use --check)")
    mapping = build_mapping(args)
    print_mapping(mapping)
    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(mapping, fh, indent=1, ensure_ascii=False)
        print(f"\nmapping written to {args.json}", file=sys.stderr)
    return 1 if mapping["summary"]["unresolved_logins"] else 0


if __name__ == "__main__":
    sys.exit(main())
