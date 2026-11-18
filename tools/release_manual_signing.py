#!/usr/bin/env python3
"""Where a release's PDF manual gets its signature, and whether a run that
Docs triggered has to sign again.

From RFC #6964 the manual is listed in SHA256SUMS.txt, and SHA256SUMS.txt.asc
is its only signature. Releases whose Docs workflow still signs the manual
apart keep that shape, and so does a signing dispatch for such a tag. The
answer comes from the tag's own .github/workflows/docs.yml, which is the copy
that ran for that tag. This file is the only place the rule is written:
sign-release.yml runs it from the default branch, and
.claude/skills/tag-release/scripts/check_release_assets.py imports it.

    release_manual_signing.py shape --tree DIR
        Prints "sums" when the manual belongs in SHA256SUMS.txt, "apart" when
        the tag's Docs workflow signs it on its own, and "none" when the tag
        has no manual.

    release_manual_signing.py plan --tree DIR --assets JSON --name NAME
                                   [--pdf FILE] [--sums FILE]
        For a run that Docs triggered. JSON is `gh release view --json
        assets`. Prints "resign: <why>" or "skip: <why>". Exit 0 either way.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from datetime import datetime
from pathlib import Path

# What sign-release.yml downloads and signs, besides the manual.
SIGNABLE_SUFFIXES = (".AppImage", ".tar.zst", ".exe", ".zip")


def shape_of(docs_yml: str | None, has_site: bool) -> str:
    if not has_site:
        return "none"
    for line in (docs_yml or "").splitlines():
        code = line.strip()
        # A comment that mentions the old flow decides nothing.
        if code.startswith("#"):
            continue
        if "--detach-sign" in code and "AetherSDR-Manual" in code:
            return "apart"
    return "sums"


def shape(tree: Path) -> str:
    has_site = (tree / "tools/docs/build_pdf.py").is_file() and (tree / "docs/user/sidebars.js").is_file()
    docs = tree / ".github/workflows/docs.yml"
    text = docs.read_text(encoding="utf-8", errors="replace") if docs.is_file() else None
    return shape_of(text, has_site)


def sums_entry(sums_text: str, name: str) -> str | None:
    """The SHA-256 SHA256SUMS.txt lists for NAME, if any (sha256sum's text and
    binary forms)."""
    for line in sums_text.splitlines():
        parts = line.split(None, 1)
        if len(parts) == 2 and parts[1].lstrip("*") == name:
            return parts[0].lower()
    return None


def _when(s: str) -> datetime:
    return datetime.fromisoformat(s.replace("Z", "+00:00"))


def attached_after_signing(assets: list[dict], sums: dict) -> list[str]:
    """The build artifacts attached after SHA256SUMS.txt was, by name. The
    source tarball is not one: the signing run makes it and uploads it just
    after the sums. tools/offload_release_assets.py waits on this too."""
    signed_at = _when(sums["updatedAt"])
    return sorted(a["name"] for a in assets
                  if a["name"].endswith(SIGNABLE_SUFFIXES) and _when(a["updatedAt"]) > signed_at)


def plan(tag_shape: str, assets: list[dict], name: str, pdf_sha: str | None,
         sums_text: str | None) -> str:
    """Whether a Docs-triggered run signs, from what the release holds now.

    The run shares the per-tag concurrency group with the build-triggered
    runs, and GitHub keeps one pending run per group. So this run can stand
    in for a build-triggered run it displaced. It therefore asks whether
    anything signable is uncovered, not only whether the manual is."""
    if tag_shape != "sums":
        return f"skip: this tag's manual is {'signed on its own' if tag_shape == 'apart' else 'absent'}"
    by_name = {a["name"]: a for a in assets}
    signables = [a for a in assets if a["name"].endswith(SIGNABLE_SUFFIXES)]
    sums = by_name.get("SHA256SUMS.txt")
    if sums is None or sums_text is None:
        if signables:
            return "resign: build artifacts are attached and nothing has signed them"
        return "skip: no build has attached anything yet; its own signing run lists the manual"
    newer = attached_after_signing(assets, sums)
    if newer:
        return "resign: attached after the last signing: " + ", ".join(newer)
    if pdf_sha is not None:
        listed = sums_entry(sums_text, name)
        if listed is None:
            return "resign: SHA256SUMS.txt does not list the manual"
        if listed != pdf_sha.lower():
            return "resign: the manual was replaced after signing"
    return "skip: SHA256SUMS.txt covers everything attached"


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("shape")
    s.add_argument("--tree", type=Path, default=Path("."))
    p = sub.add_parser("plan")
    p.add_argument("--tree", type=Path, required=True)
    p.add_argument("--assets", type=Path, required=True)
    p.add_argument("--name", required=True)
    p.add_argument("--pdf", type=Path)
    p.add_argument("--sums", type=Path)
    args = ap.parse_args(argv)
    if args.cmd == "shape":
        print(shape(args.tree))
        return 0
    assets = json.loads(args.assets.read_text(encoding="utf-8"))["assets"]
    pdf_sha = _sha256(args.pdf) if args.pdf and args.pdf.is_file() else None
    sums = args.sums.read_text(encoding="utf-8") if args.sums and args.sums.is_file() else None
    print(plan(shape(args.tree), assets, args.name, pdf_sha, sums))
    return 0


if __name__ == "__main__":
    sys.exit(main())
