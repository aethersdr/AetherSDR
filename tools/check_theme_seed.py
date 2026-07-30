#!/usr/bin/env python3
"""
AetherSDR theme-seed drift checker.

ThemeManager::seedBuiltinDefaults() compiles a copy of the default theme into
the binary so the UI is usable with zero theme files on disk. Those values are
kept in sync with resources/themes/default-dark.json BY HAND, and the code says
so twice:

    "Kept in sync with the JSON resource manually"           (ThemeManager.cpp)
    "KEEP IN SYNC: ... If those primitives shift, update
     both sites or the seeded look will drift from the
     JSON-defined look on bundled themes (silently — both
     layers resolve, the JSON wins, but the visible vs.
     seeded values diverge for pre-PR user themes)."

The drift it predicts has already happened. This checker makes it visible, and
--strict makes it fail.

WHY THIS IS SUBTLE: on a normal run the JSON wins, so a drifted seed is
invisible. It only surfaces for a user whose theme predates a token (the seed
supplies the value the JSON would otherwise override) or when no theme file
loads at all. Both are exactly the situations where nobody is looking.

This is a stopgap. The real fix is to GENERATE the seed table from the resource
at build time so the two cannot disagree; see the tracking issue. Until then
this pins the current state and stops it widening.

SCOPE — COLOUR TOKENS ONLY. The parser captures QString("#hex") inserts and
nothing else. Seeds that are not hex colours never enter the comparison and
can drift silently: the seven font.family.*, four font.size.*, five sizing.*
values, and color.meter.bar.fillGradient (inserted via QVariant::fromValue(g),
no literal at all, so it is invisible even to the completeness guard below).
Three of those non-colour tokens exist on BOTH sides today (font.size.normal,
sizing.panel.padding, sizing.panel.cornerRadius) and would drift exactly the
way the colours did. If the parser is ever widened to cover them, switch the
completeness guard from hex-literal equality to counting tokens.insert(
occurrences — the two invariants are only correct one at a time (see the
review reconciliation on PR #4570).

Usage:
    python tools/check_theme_seed.py            # report drift, exit 0
    python tools/check_theme_seed.py --strict   # exit 1 on NEW drift or a
                                                #   stale KNOWN_SEED_DRIFT entry
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

# Drift that exists TODAY and is not this checker's job to fix. Each entry is
# (scope, token). The list may only SHRINK: --strict fails on any pair not
# listed here, so a new divergence blocks while these are burnt down.
#
# All nine are documented in the tracking issue. The eight slice colours come
# from the seed's own "Preliminary values — a dedicated slice-colour audit may
# tune these in a follow-up" comment: the JSON was later tuned, the seed was
# not.
KNOWN_SEED_DRIFT = {
    ("root", "color.accent.dim"),
    ("root", "color.slice.a"),
    ("root", "color.slice.b"),
    ("root", "color.slice.c"),
    ("root", "color.slice.d"),
    ("root", "color.slice.e"),
    ("root", "color.slice.f"),
    ("root", "color.slice.g"),
    ("root", "color.slice.h"),
}

REPO = Path(__file__).resolve().parent.parent
THEME_JSON = REPO / "resources" / "themes" / "default-dark.json"
THEME_CPP = REPO / "src" / "core" / "ThemeManager.cpp"

HEX = r"#[0-9a-fA-F]{3,8}"


def normalise_hex(value: str) -> str:
    """Canonicalise hex colour spellings so equal colours compare equal.

    #fff -> #ffffff, #fabc -> #ffaabbcc, and a fully-opaque leading alpha is
    dropped (#ffRRGGBB -> #RRGGBB — Qt's alpha is LEADING). Non-hex values
    pass through untouched so a genuine type mismatch still reads as drift.
    """
    v = str(value).strip().lower()
    if not re.fullmatch(HEX, v):
        return v
    digits = v[1:]
    if len(digits) in (3, 4):                      # short form: double each
        digits = "".join(ch * 2 for ch in digits)
    if len(digits) == 8 and digits.startswith("ff"):
        digits = digits[2:]                        # opaque alpha adds nothing
    return "#" + digits


def flatten(node, prefix: str = "") -> dict:
    """Nested dict -> dotted keys. Leaves are scalars."""
    out = {}
    if isinstance(node, dict):
        for key, value in node.items():
            out.update(flatten(value, f"{prefix}.{key}" if prefix else key))
    else:
        out[prefix] = node
    return out


def resolve_aliases(value, primitives: dict, depth: int = 0):
    """Expand {color.red.500} against the primitives palette."""
    if isinstance(value, str) and depth < 8:
        match = re.fullmatch(r"\{([^}]+)\}", value.strip())
        if match:
            return resolve_aliases(primitives.get(match.group(1), value),
                                   primitives, depth + 1)
    return value


def json_tokens() -> dict:
    """(scope, token) -> resolved value, for every scope in the bundled theme."""
    data = json.loads(THEME_JSON.read_text(encoding="utf-8"))
    primitives = flatten(data.get("primitives", {}))
    resolved = {}

    def walk(node, scope: str):
        for token, value in flatten(node.get("tokens", {})).items():
            resolved[(scope, token)] = resolve_aliases(value, primitives)
        for name, child in node.get("scopes", {}).items():
            walk(child, name if scope == "root" else f"{scope}/{name}")

    walk(data["scopes"]["root"], "root")
    return resolved


def cpp_tokens() -> dict:
    """(scope, token) -> literal hex, as compiled into seedBuiltinDefaults()."""
    source = THEME_CPP.read_text(encoding="utf-8")
    body = re.search(r"void ThemeManager::seedBuiltinDefaults\(\).*?\n\}\n",
                     source, re.S)
    if not body:
        raise SystemExit("check_theme_seed: seedBuiltinDefaults() not found — "
                         "has ThemeManager.cpp been restructured?")
    body = body.group(0)

    seeded = {}
    # Root-level: m_tokens.insert("color.x", QString("#rrggbb"));
    for match in re.finditer(
            rf'm_tokens\.insert\(\s*"([^"]+)"\s*,\s*QString\("({HEX})"\)', body):
        seeded[("root", match.group(1))] = match.group(2)

    # Scoped: scopeOrCreate(QStringLiteral("applet/tx")); s->tokens.insert(...)
    for block in re.finditer(
            r'scopeOrCreate\(QStringLiteral\("([^"]+)"\)\);(.*?)\n    \}',
            body, re.S):
        scope = block.group(1)
        for match in re.finditer(
                rf'tokens\.insert\("([^"]+)",\s*QString\("({HEX})"\)',
                block.group(2)):
            seeded[(scope, match.group(1))] = match.group(2)

    # A PARTIAL parse is the dangerous failure: renaming m_tokens or reindenting
    # a scoped block drops tokens while `shared` stays non-zero, so main()'s
    # zero-token guard never trips and the missing tokens surface as "stale"
    # baseline entries. Every hex literal in the body must have been captured.
    expected = len(re.findall(rf'QString\("{HEX}"\)', body))
    if len(seeded) != expected:
        raise SystemExit(f"check_theme_seed: captured {len(seeded)} of {expected} "
                         "seeded colour literals — the parser is out of date with "
                         "seedBuiltinDefaults(). Refusing to report a clean run.")

    return seeded


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Detect drift between the compiled theme seed and the "
                    "bundled default-dark.json")
    parser.add_argument("--strict", action="store_true",
                        help="exit 1 on any drift not in KNOWN_SEED_DRIFT")
    args = parser.parse_args()

    from_json = json_tokens()
    from_cpp = cpp_tokens()
    shared = sorted(set(from_json) & set(from_cpp))

    if not shared:
        raise SystemExit("check_theme_seed: parsed 0 shared tokens — the "
                         "parser is broken, not the theme. Refusing to report "
                         "a clean run.")

    drift = [(scope, token, from_cpp[(scope, token)], from_json[(scope, token)])
             for scope, token in shared
             if normalise_hex(from_cpp[(scope, token)])
             != normalise_hex(from_json[(scope, token)])]

    known = [d for d in drift if (d[0], d[1]) in KNOWN_SEED_DRIFT]
    new = [d for d in drift if (d[0], d[1]) not in KNOWN_SEED_DRIFT]
    stale = sorted(KNOWN_SEED_DRIFT - {(d[0], d[1]) for d in drift})

    # "colour" is load-bearing in these labels: non-colour seeds (fonts,
    # sizing, the meter gradient) are out of scope — see the module docstring.
    print("=== theme seed vs default-dark.json (colour tokens) ===")
    print(f"  colour tokens compared     : {len(shared)}")
    print(f"  colour seeds only in C++   : {len(set(from_cpp) - set(from_json))}")
    print(f"  colours present only in JSON: {len(set(from_json) - set(from_cpp))}")
    print(f"  drifted (known)            : {len(known)}")
    print(f"  drifted (NEW)              : {len(new)}")

    for scope, token, cpp_value, json_value in known:
        print(f"  known  [{scope}] {token}: C++={cpp_value} JSON={json_value}")
    for scope, token, cpp_value, json_value in new:
        print(f"  NEW    [{scope}] {token}: C++={cpp_value} JSON={json_value}")

    if stale:
        print("\n  These are listed in KNOWN_SEED_DRIFT but no longer drift — "
              "remove them so the list keeps shrinking:")
        for scope, token in stale:
            print(f"    {scope} / {token}")

    failed = False
    if args.strict and new:
        print(f"\nFAIL: {len(new)} new seed/JSON divergence(s). Update BOTH "
              f"sites, or add to KNOWN_SEED_DRIFT with a reason.")
        failed = True
    # Stale entries FAIL strict runs too. A ratchet whose baseline can hold
    # dead entries silently stops ratcheting — and worse, a parser regression
    # that drops tokens surfaces AS stale entries, so a green run that says
    # "empty the baseline" would be actively misleading. Fix the entry (or the
    # parser) in the same change.
    if args.strict and stale:
        print(f"\nFAIL: {len(stale)} stale KNOWN_SEED_DRIFT entr"
              f"{'y' if len(stale) == 1 else 'ies'} — remove them (or fix the "
              f"parser, if tokens went missing rather than converged).")
        failed = True

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
