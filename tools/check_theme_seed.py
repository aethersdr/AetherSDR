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
so the two cannot disagree; see the tracking issue. Until then this pins the
current state and stops it widening.

SCOPE — COLOUR TOKENS ONLY. A seed enters the comparison when its value carries
a hex colour literal, in ANY spelling (QString, QStringLiteral, QColor, or a
bare string). Non-colour seeds are out of scope and can still drift silently:

  * the nine numeric seeds that exist on BOTH sides today — font.size.{tiny,
    small,normal,large} and sizing.{panel.padding,panel.spacing,
    panel.cornerRadius,border.subtle,border.strong}. These would drift exactly
    the way the colours did; widening the parser to cover them is worthwhile.
  * the seven font.family.* seeds, which CANNOT be compared key-for-key: the
    JSON stores them as compound objects ({family, size, color}, flattened here
    to font.family.ui.family etc.) while the C++ side inserts a flat string, so
    the keys never intersect. Covering these needs a shape-aware rule, not a
    wider value pattern.
  * color.meter.bar.fillGradient, inserted via QVariant::fromValue(g) with no
    literal in the insert at all. Its five QColor("#…") gradient stops ARE
    accounted for by the completeness guard (see cpp_tokens) — they are simply
    not token seeds.

HOW THIS RESISTS BEING FOOLED. A parser-based gate is only as good as its
failure mode, so three invariants are enforced rather than assumed:

  1. Comments are stripped before matching, so commented-out code can neither
     inject a phantom divergence nor supply a value that masks a live one.
  2. Every insert() call in the body must parse. The scan matches parens rather
     than indentation, so reformatting cannot silently drop a token.
  3. Every hex literal in the body must be accounted for — either inside a
     parsed insert or outside every insert. A hex that belongs to neither means
     the parser lost track, and that hard-fails instead of reporting clean.

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
HEX_LITERAL = re.compile(rf'"({HEX})"')


def fail(message: str) -> "NoReturn":  # noqa: F821 - str annotation, py3.8 safe
    raise SystemExit(f"check_theme_seed: {message}")


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


def is_hex_colour(value) -> bool:
    """True when a value is a hex colour literal (either side of the compare)."""
    return isinstance(value, str) and re.fullmatch(HEX, value.strip()) is not None


def strip_comments(source: str) -> str:
    """Blank out // and /* */ comments, preserving offsets and line structure.

    String- and char-literal aware, so a "//" inside a literal survives. Comment
    bytes become spaces (newlines kept) so every span/offset in the caller stays
    valid and reported line numbers do not shift.
    """
    out = list(source)
    i, n = 0, len(source)
    while i < n:
        ch = source[i]
        if ch in ('"', "'"):                       # skip over a literal
            quote = ch
            i += 1
            while i < n:
                if source[i] == "\\":
                    i += 2
                    continue
                if source[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        if ch == "/" and i + 1 < n:
            if source[i + 1] == "/":
                while i < n and source[i] != "\n":
                    out[i] = " "
                    i += 1
                continue
            if source[i + 1] == "*":
                end = source.find("*/", i + 2)
                end = n if end == -1 else end + 2
                for j in range(i, end):
                    if out[j] != "\n":
                        out[j] = " "
                i = end
                continue
        i += 1
    return "".join(out)


def seed_body(source: str) -> str:
    """Extract seedBuiltinDefaults()'s body by brace matching, not by regex.

    A trailing-brace regex breaks on any reformat; brace matching only breaks if
    the function genuinely stops existing, which is worth a hard failure.
    """
    signature = re.search(r"void\s+ThemeManager::seedBuiltinDefaults\s*\(\s*\)",
                          source)
    if not signature:
        fail("seedBuiltinDefaults() not found — has ThemeManager.cpp been "
             "restructured? Refusing to report a clean run.")
    start = source.find("{", signature.end())
    if start == -1:
        fail("seedBuiltinDefaults() has no body brace.")
    depth = 0
    for i in range(start, len(source)):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return source[start:i + 1]
    fail("seedBuiltinDefaults() body is unbalanced — refusing to guess.")


def _split_args(text: str) -> list:
    """Split a call's argument text on TOP-LEVEL commas only."""
    args, depth, current, i = [], 0, [], 0
    while i < len(text):
        ch = text[i]
        if ch in ('"', "'"):
            quote = ch
            current.append(ch)
            i += 1
            while i < len(text):
                current.append(text[i])
                if text[i] == "\\":
                    current.append(text[i + 1] if i + 1 < len(text) else "")
                    i += 2
                    continue
                if text[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            args.append("".join(current))
            current = []
            i += 1
            continue
        current.append(ch)
        i += 1
    args.append("".join(current))
    return args


def _insert_calls(body: str) -> list:
    """Every ...insert(...) call in `body`, paren-matched.

    Returns (receiver, args_text, span_start, span_end). Indentation-independent
    by construction, so reformatting cannot drop a token.
    """
    calls = []
    for match in re.finditer(r"(\w+)\s*(?:->|\.)\s*insert\s*\(", body):
        receiver = match.group(1)
        depth, i, n = 0, match.end() - 1, len(body)
        while i < n:
            ch = body[i]
            if ch in ('"', "'"):
                quote = ch
                i += 1
                while i < n:
                    if body[i] == "\\":
                        i += 2
                        continue
                    if body[i] == quote:
                        break
                    i += 1
            elif ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if depth == 0:
                    calls.append((receiver, body[match.end():i],
                                  match.start(), i + 1))
                    break
            i += 1
        else:
            fail(f"unterminated insert() call near offset {match.start()} — "
                 "refusing to report a clean run.")
    return calls


def cpp_tokens() -> dict:
    """(scope, token) -> hex literal, as compiled into seedBuiltinDefaults()."""
    body = seed_body(strip_comments(THEME_CPP.read_text(encoding="utf-8")))

    # Scope for a given offset: m_tokens is always root; anything else belongs
    # to the most recent scopeOrCreate(QStringLiteral("path")) above it. Reading
    # the scope this way is immune to how the enclosing block is indented or
    # braced — the failure the previous `\n    \}` terminator was prone to.
    scope_marks = [(m.start(), m.group(1)) for m in re.finditer(
        r'scopeOrCreate\s*\(\s*QStringLiteral\s*\(\s*"([^"]+)"\s*\)', body)]

    def scope_at(offset: int, receiver: str) -> str:
        if receiver == "m_tokens":
            return "root"
        current = "root"
        for mark_at, path in scope_marks:
            if mark_at < offset:
                current = path
            else:
                break
        return current

    seeded, consumed_spans, seen_keys = {}, [], set()
    for receiver, args_text, span_start, span_end in _insert_calls(body):
        args = _split_args(args_text)
        if len(args) < 2:
            fail(f"insert() with {len(args)} argument(s) at offset {span_start} "
                 "— parser is out of date with seedBuiltinDefaults().")
        name = re.search(r'"([^"]+)"', args[0])
        if not name:
            fail(f"insert() at offset {span_start} has no literal token name — "
                 "parser is out of date with seedBuiltinDefaults().")
        consumed_spans.append((span_start, span_end))

        # A colour seed is one whose VALUE carries a hex literal, whatever it is
        # wrapped in. Matching the wrapper (the old QString("#…") pattern) made
        # the completeness guard tautological: respelling a seed as
        # QStringLiteral("#…") dropped it from both the capture and the expected
        # count in lockstep, so real drift vanished silently.
        value_hex = HEX_LITERAL.findall(args[1])
        if not value_hex:
            continue                               # non-colour seed, out of scope
        if len(value_hex) > 1:
            fail(f'token "{name.group(1)}" seeds {len(value_hex)} hex literals '
                 "in one value — refusing to guess which is the colour.")
        key = (scope_at(span_start, receiver), name.group(1))
        if key in seen_keys:
            fail(f"token {key[1]} is seeded twice in scope {key[0]} — the "
                 "later insert silently wins; remove one.")
        seen_keys.add(key)
        seeded[key] = value_hex[0]

    # INVARIANT: every hex literal in the body is either inside an insert we
    # parsed, or outside every insert (the meter gradient's QColor stops, which
    # are not token seeds). One that is inside an UNparsed insert means the scan
    # lost a token, and that must hard-fail rather than report clean.
    outside = list(body)
    for start, end in consumed_spans:
        for j in range(start, end):
            outside[j] = " "
    accounted = len(seeded) + sum(
        1 for _ in HEX_LITERAL.finditer("".join(outside)))
    total = len(HEX_LITERAL.findall(body))
    if accounted != total:
        fail(f"accounted for {accounted} of {total} hex literals in "
             "seedBuiltinDefaults() — the parser is out of date. Refusing to "
             "report a clean run.")

    if not seeded:
        fail("parsed 0 colour seeds — the parser is broken, not the theme.")
    return seeded


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
        fail("parsed 0 shared tokens — the parser is broken, not the theme. "
             "Refusing to report a clean run.")

    drift = [(scope, token, from_cpp[(scope, token)], from_json[(scope, token)])
             for scope, token in shared
             if normalise_hex(from_cpp[(scope, token)])
             != normalise_hex(from_json[(scope, token)])]

    known = [d for d in drift if (d[0], d[1]) in KNOWN_SEED_DRIFT]
    new = [d for d in drift if (d[0], d[1]) not in KNOWN_SEED_DRIFT]
    stale = sorted(KNOWN_SEED_DRIFT - {(d[0], d[1]) for d in drift})

    # Both "only in" counters are restricted to COLOUR tokens, so they describe
    # the set this checker actually governs. Counting every flattened JSON key
    # here overstated the gap: it swept in font.size.*, sizing.*, the
    # font.family.* sub-keys and each waterfall colormap's type/angle/stops.
    json_colours = {k for k, v in from_json.items() if is_hex_colour(v)}
    print("=== theme seed vs default-dark.json (colour tokens) ===")
    print(f"  colour tokens compared      : {len(shared)}")
    print(f"  colour seeds only in C++    : {len(set(from_cpp) - json_colours)}")
    print(f"  JSON colours with no seed   : {len(json_colours - set(from_cpp))}")
    print(f"  drifted (known)             : {len(known)}")
    print(f"  drifted (NEW)               : {len(new)}")

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
