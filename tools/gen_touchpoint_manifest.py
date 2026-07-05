#!/usr/bin/env python3
"""
AetherSDR aetherd-migration touchpoint manifest generator.

Scans the UI side of the tree (src/gui/ + src/main.cpp) for #include
dependencies on engine headers (src/core/, src/models/) and regenerates
the touchpoint burndown manifest used by the aetherd migration
(docs/aetherd-headless-engine-design.md, RFC step 2+).

The manifest is GENERATED — do not hand-edit the table. Two sidecar
files carry the human/agent-authored state and survive regeneration:

  docs/architecture/aetherd-touchpoint-tags.json
      Semantic tags per header: core-profile ("universal") vs
      vendor-specific vs mixed, with rationale. Produced by the
      classification pass; merged into the manifest when present.

  docs/architecture/aetherd-touchpoint-status.json
      Conversion status per header ("unconverted" | "in progress:<who>"
      | "converted:<PR#>"). Updated by conversion PRs. The check that
      the converted list only grows compares this file across commits.

Usage:
    python tools/gen_touchpoint_manifest.py            # regenerate
    python tools/gen_touchpoint_manifest.py --check    # exit 1 if stale

Exit 0 on success; --check exits 1 if the committed manifest differs
from what would be generated (used to keep the manifest honest in CI).

stdlib only; no third-party dependencies.
"""

import argparse
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
UI_ROOTS = [REPO / "src" / "gui", REPO / "src" / "main.cpp"]
ENGINE_DIRS = {"core": REPO / "src" / "core", "models": REPO / "src" / "models"}

MANIFEST_MD = REPO / "docs" / "architecture" / "aetherd-touchpoints.md"
TAGS_JSON = REPO / "docs" / "architecture" / "aetherd-touchpoint-tags.json"
STATUS_JSON = REPO / "docs" / "architecture" / "aetherd-touchpoint-status.json"

# Matches any quoted .h include; resolution (incl. core/models subdirs like
# core/aprs/AprsPacket.h) happens in resolve_engine_header.
INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"(?P<inc>[^"]+\.h)"')


def ui_files():
    for root in UI_ROOTS:
        if root.is_file():
            yield root
        elif root.is_dir():
            yield from sorted(
                p for p in root.rglob("*") if p.suffix in (".h", ".cpp")
            )


def resolve_engine_header(inc):
    """Return (module, relpath) if an include resolves into the engine, else
    None. Handles module-qualified paths WITH subdirectories
    (core/aprs/AprsPacket.h -> ("core", "aprs/AprsPacket.h")) and bare-name
    sibling includes (resolved against core/ and models/ only when no
    same-named gui header exists — gui files include siblings by bare name)."""
    p = re.sub(r"^(?:\.\./)+", "", inc)  # strip any leading ../
    if p.startswith(("core/", "models/")):
        mod, rest = p.split("/", 1)
        return (mod, rest) if (ENGINE_DIRS[mod] / rest).is_file() else None
    if "/" in p:  # a subdir path that isn't under core/ or models/
        return None
    if (REPO / "src" / "gui" / p).is_file():
        return None
    for m, d in ENGINE_DIRS.items():
        if (d / p).is_file():
            return (m, p)
    return None


def scan():
    touchpoints = {}  # (module, header) -> sorted set of includer rel-paths
    for f in ui_files():
        rel = f.relative_to(REPO).as_posix()
        try:
            text = f.read_text(errors="replace")
        except OSError as e:
            print(f"warning: unreadable {rel}: {e}", file=sys.stderr)
            continue
        for line in text.splitlines():
            m = INCLUDE_RE.match(line)
            if not m:
                continue
            hit = resolve_engine_header(m.group("inc"))
            if hit:
                touchpoints.setdefault(hit, set()).add(rel)
    return {k: sorted(v) for k, v in sorted(touchpoints.items())}


def load_json(path):
    if not path.is_file():
        return {}
    try:
        return json.loads(path.read_text())
    except (json.JSONDecodeError, ValueError) as e:
        print(f"warning: {path.name} is not valid JSON ({e}); treating as "
              "empty", file=sys.stderr)
        return {}


def render(touchpoints, tags, status):
    total = len(touchpoints)
    by_mod = {"core": 0, "models": 0}
    for (mod, _name) in touchpoints:
        by_mod[mod] += 1
    tagged = sum(1 for (m, n) in touchpoints if f"{m}/{n}" in tags)
    converted = sum(
        1
        for (m, n) in touchpoints
        if str(status.get(f"{m}/{n}", "")).startswith("converted")
    )

    lines = [
        "# aetherd migration — gui→engine touchpoint manifest",
        "",
        "<!-- GENERATED FILE — regenerate with"
        " `python tools/gen_touchpoint_manifest.py`."
        " Edit the tags/status JSON sidecars, never this table. -->",
        "",
        "Burndown manifest for the engine/UI decoupling"
        " ([RFC](../aetherd-headless-engine-design.md) §2, §10)."
        " One row per engine header the UI includes; converting a"
        " touchpoint means the UI reaches that surface through the"
        " versioned protocol instead of the header.",
        "",
        f"**Totals:** {total} touchpoint headers"
        f" ({by_mod['core']} core, {by_mod['models']} models) —"
        f" {tagged}/{total} tagged, {converted}/{total} converted.",
        "",
        "| Header | Includers | Tag | Status |",
        "|---|---:|---|---|",
    ]
    for (mod, name), includers in touchpoints.items():
        key = f"{mod}/{name}"
        tag = tags.get(key, {})
        tag_txt = tag.get("tag", "—")
        if tag.get("note"):
            tag_txt += f" — {tag['note']}"
        lines.append(
            f"| `{key}` | {len(includers)} |"
            f" {tag_txt} | {status.get(key, 'unconverted')} |"
        )
    lines += [
        "",
        "**Tag legend:** `universal` = core-profile surface every radio"
        " family has; `vendor` = FlexRadio/SmartSDR-specific, becomes a"
        " namespaced extension; `mixed` = header carries both (split"
        " candidates noted); `ui-support` = not radio state at all"
        " (settings, theming, app plumbing) — needs a home decision,"
        " not a protocol message.",
        "",
    ]
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="exit 1 if the committed manifest is stale")
    args = ap.parse_args()

    touchpoints = scan()
    out = render(touchpoints, load_json(TAGS_JSON), load_json(STATUS_JSON))

    if args.check:
        current = MANIFEST_MD.read_text() if MANIFEST_MD.is_file() else ""
        if current != out:
            print("::error::aetherd-touchpoints.md is stale — run"
                  " `python tools/gen_touchpoint_manifest.py`")
            return 1
        print("manifest up to date")
        return 0

    MANIFEST_MD.parent.mkdir(parents=True, exist_ok=True)
    MANIFEST_MD.write_text(out)
    print(f"wrote {MANIFEST_MD.relative_to(REPO)}: "
          f"{len(touchpoints)} touchpoint headers")
    return 0


if __name__ == "__main__":
    sys.exit(main())
