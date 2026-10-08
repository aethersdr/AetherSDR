#!/usr/bin/env python3
"""Embed the documentation screenshots listed in docs/user/screens.json.

Usage:
    python3 tools/docs/embed_screenshots.py            # update the pages
    python3 tools/docs/embed_screenshots.py --check    # exit 1 if a page is
                                                       # out of step

tools/docs/capture_screenshots.py writes each shot in screens.json to
docs/user/static/img/screens/<id>.png. This script puts each one on its page
(the shot's "page", a file in docs/user/docs/) as

    <img src="/img/screens/<id>.png" width="<logical px>" alt="<alt>" />

    *<caption>*

The shots are captured on a HiDPI display, so each PNG is
device_pixel_ratio (screens.json) times its logical size; the width attribute
makes the browser show it at its logical size, sharp on HiDPI screens. (The
pages are CommonMark, format 'md', which Docusaurus renders HTML in;
tools/docs/build_pdf.py turns the tag back into a Markdown image.)

A shot's "anchor_heading" says where it goes: "intro" puts it at the end of
the page's introduction, just before its first "## " heading; a heading's
text puts it at the start of that section, after the section's opening
paragraphs.

It is idempotent. An image line already pointing at /img/screens/<id>.png is
replaced in place together with its caption, so re-shooting or rewording a
caption never duplicates an image, and a hand-moved image stays where it was
moved to. An embedded /img/screens/ image whose shot has been removed from
screens.json is removed from the page.

Stdlib only; Python 3.9+.
"""

from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SITE = REPO / "docs" / "user"
DOCS = SITE / "docs"
SCREENS_JSON = SITE / "screens.json"
SHOTS_DIR = SITE / "static" / "img" / "screens"
URL_PREFIX = "/img/screens/"

IMAGE_RE = re.compile(r"^(?:!\[.*\]\(" + re.escape(URL_PREFIX) + r"([A-Za-z0-9._-]+)\.png\)"
                      r"|<img src=\"" + re.escape(URL_PREFIX) + r"([A-Za-z0-9._-]+)\.png\"[^>]*/?>)\s*$")
CAPTION_RE = re.compile(r"^\*.*\*\s*$")
HEADING_RE = re.compile(r"^(#{1,6})\s+(.*?)\s*(?:\{#[^}]*\})?\s*$")
FENCE_RE = re.compile(r"^\s*(`{3,}|~{3,})")


def fail(msg: str) -> None:
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(2)


def escape_attr(text: str) -> str:
    return (text.replace("&", "&amp;").replace('"', "&quot;")
            .replace("<", "&lt;").replace(">", "&gt;"))


def png_width(path: Path) -> int:
    with path.open("rb") as f:
        head = f.read(24)
    if head[:8] != b"\x89PNG\r\n\x1a\n":
        fail(f"{path} is not a PNG")
    return struct.unpack(">I", head[16:20])[0]


def escape_caption(text: str) -> str:
    return text.replace("\\", "\\\\").replace("*", "\\*").replace("_", "\\_")


def block_for(shot: dict) -> list[str]:
    width = round(png_width(SHOTS_DIR / f"{shot['id']}.png") / shot["_dpr"])
    return [
        f'<img src="{URL_PREFIX}{shot["id"]}.png" width="{width}" '
        f'alt="{escape_attr(shot["alt"])}" />',
        "",
        f"*{escape_caption(shot['caption'])}*",
    ]


def code_mask(lines: list[str]) -> list[bool]:
    """True for each line inside (or opening/closing) a fenced code block."""
    mask, fence = [], None
    for line in lines:
        m = FENCE_RE.match(line)
        if fence is None and m:
            fence = m.group(1)[0] * len(m.group(1))
            mask.append(True)
        elif fence is not None:
            mask.append(True)
            if line.strip().startswith(fence):
                fence = None
        else:
            mask.append(False)
    return mask


def body_start(lines: list[str]) -> int:
    """Index of the first line after the front matter."""
    if lines and lines[0].strip() == "---":
        for i in range(1, len(lines)):
            if lines[i].strip() == "---":
                return i + 1
    return 0


def remove_block(lines: list[str], i: int) -> tuple[list[str], int]:
    """Remove the image at lines[i], its caption and the blank lines around
    it. Return the new lines and the index where the block was."""
    j = i + 1
    k = j
    while k < len(lines) and not lines[k].strip():
        k += 1
    if k < len(lines) and CAPTION_RE.match(lines[k]):
        j = k + 1
    while j < len(lines) and not lines[j].strip():
        j += 1
    start = i
    while start > 0 and not lines[start - 1].strip():
        start -= 1
    gap = [""] if start > 0 and j < len(lines) else []
    return lines[:start] + gap + lines[j:], start


def insertion_point(lines: list[str], shot: dict, page: str) -> int:
    mask = code_mask(lines)
    start = body_start(lines)
    section = shot.get("anchor_heading", "intro")
    if section == "intro":
        for i in range(start, len(lines)):
            if not mask[i] and lines[i].startswith("## "):
                return i
        fail(f"{page}: no '## ' heading to place {shot['id']} before")
    for i in range(start, len(lines)):
        m = HEADING_RE.match(lines[i]) if not mask[i] else None
        if m and m.group(2) == section:
            break
    else:
        fail(f"{page}: no heading {section!r} for shot {shot['id']}")
    # After the section's opening paragraphs: stop at the first list, table,
    # heading, fence, admonition or quote.
    j = i + 1
    while j < len(lines):
        line = lines[j]
        s = line.strip()
        if not s:
            j += 1
            continue
        if mask[j] or re.match(r"^(#|[-*+] |\d+[.)] |\||:::|>|!\[|<img )", s):
            break
        while j < len(lines) and lines[j].strip():
            j += 1
    # Back up over the blank lines so the block lands right after the text.
    while j > i + 1 and not lines[j - 1].strip():
        j -= 1
    return j


def place(lines: list[str], at: int, block: list[str]) -> list[str]:
    before, after = lines[:at], lines[at:]
    while before and not before[-1].strip():
        before.pop()
    while after and not after[0].strip():
        after.pop(0)
    return before + [""] + block + [""] + after


def embed(text: str, shots: list[dict], page: str) -> str:
    lines = text.split("\n")
    trailing_newline = text.endswith("\n")
    if trailing_newline:
        lines = lines[:-1]
    wanted = {s["id"]: s for s in shots}
    # Take every managed image out, remembering where each wanted one was.
    anchors: dict[str, int] = {}
    i = 0
    while i < len(lines):
        m = IMAGE_RE.match(lines[i])
        if not m or code_mask(lines)[i]:
            i += 1
            continue
        shot_id = m.group(1) or m.group(2)
        lines, at = remove_block(lines, i)
        if shot_id in wanted and shot_id not in anchors:
            anchors[shot_id] = at
        i = at
    # Put them back where they were, last first so the indexes hold.
    # Adjacent images share an anchor once removed; among those, re-insert
    # the later one first so the original order survives.
    order = {shot_id: n for n, shot_id in enumerate(anchors)}
    for shot_id, at in sorted(anchors.items(), key=lambda kv: (-kv[1], -order[kv[0]])):
        lines = place(lines, at, block_for(wanted[shot_id]))
    # Place the new ones. Last first: each goes to the top of its section,
    # so this keeps two shots in one section in manifest order.
    for shot in reversed(shots):
        if shot["id"] not in anchors:
            lines = place(lines, insertion_point(lines, shot, page), block_for(shot))
    out = "\n".join(lines)
    return out + "\n" if trailing_newline else out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--check", action="store_true",
                    help="report pages that are out of step, change nothing")
    args = ap.parse_args()

    config = json.loads(SCREENS_JSON.read_text(encoding="utf-8"))
    shots = config["shots"]
    dpr = float(config.get("device_pixel_ratio", 1.0))
    by_page: dict[str, list[dict]] = {}
    for shot in shots:
        shot["_dpr"] = dpr
        for key in ("id", "page", "alt", "caption"):
            if not shot.get(key):
                fail(f"screens.json: shot {shot.get('id', '?')} has no {key!r}")
        if not (SHOTS_DIR / f"{shot['id']}.png").is_file():
            fail(f"{SHOTS_DIR.relative_to(REPO)}/{shot['id']}.png is missing; "
                 "run tools/docs/capture_screenshots.py")
        by_page.setdefault(shot["page"], []).append(shot)

    stale = []
    for path in sorted(DOCS.rglob("*.md")):
        page = path.relative_to(DOCS).with_suffix("").as_posix()
        text = path.read_text(encoding="utf-8")
        page_shots = by_page.pop(page, [])
        if not page_shots and URL_PREFIX not in text:
            continue
        new = embed(text, page_shots, page)
        if new != text:
            stale.append(path.relative_to(REPO).as_posix())
            if not args.check:
                path.write_text(new, encoding="utf-8")
    if by_page:
        fail("screens.json names pages that do not exist: " + ", ".join(sorted(by_page)))

    if args.check:
        for p in stale:
            print(f"out of step: {p}")
        if stale:
            print("run: python3 tools/docs/embed_screenshots.py", file=sys.stderr)
            return 1
        print(f"screenshots are embedded ({len(shots)} shots)")
        return 0
    for p in stale:
        print(f"updated {p}")
    print(f"{len(stale)} page(s) updated, {len(shots)} shots")
    return 0


if __name__ == "__main__":
    sys.exit(main())
