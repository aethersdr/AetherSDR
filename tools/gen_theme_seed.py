#!/usr/bin/env python3
"""
Generate ThemeManager's compiled-in seed table from the bundled default theme.

WHY THIS EXISTS
---------------
`ThemeManager::seedBuiltinDefaults()` compiles a copy of the default theme into
the binary so the UI is usable with no theme files on disk. It was maintained BY
HAND against `resources/themes/default-dark.json`, and the source said so twice,
including a warning that a drifted seed diverges "silently". Both halves of that
warning came true (#3184):

  * 9 tokens had already drifted — `color.accent.dim` and all eight
    `color.slice.a..h`. The seed's own comment reads "Preliminary values — a
    dedicated slice-colour audit may tune these in a follow-up"; the JSON was
    tuned, the seed never was.
  * 24 tokens were never seeded AT ALL — `slice.dim.a..h`, `highlight.*`,
    `button.*.disabled`, and the six `waterfall.colormap.*` gradients. Those
    resolve TRANSPARENT on a theme predating them, which is a strictly worse
    failure than a wrong colour.

Generating the table removes the whole class: the JSON becomes authoritative by
construction and the two cannot disagree.

DESIGN (agreed on #3184)
------------------------
COMMIT-TIME, not build-time. The generated file is checked in and readable, and
`tools/check_theme_seed.py --strict` fails when it is stale. `seedBuiltinDefaults()`
is ~200 lines people demonstrably read and comment in — the "preliminary values"
comment is HOW this drift got diagnosed — so keeping it greppable is worth a
regeneration step. This also matches EB1/EB2/EB3: a checked-in baseline plus a
gate that fails when it drifts.

Alias resolution: `{color.red.500}` references are resolved against the
primitives palette and the CONCRETE value is emitted, because the seed runs
before any primitives are loaded (older user themes have no primitives section).
Resolution refuses to degrade: an alias naming a primitive that does not exist
is a hard error, not a passthrough. Passing it through would emit the literal
string `"{color.teal.500}"` into the C++, which QColor reads as transparent —
the exact silent failure this generator exists to remove, re-entering through
the generator itself.

Usage:
    python tools/gen_theme_seed.py            # write the generated source
    python tools/gen_theme_seed.py --check    # exit 1 if it would change
    python tools/gen_theme_seed.py --stdout   # print, don't write
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
THEME_JSON = REPO / "resources" / "themes" / "default-dark.json"
OUT_FILE = REPO / "src" / "core" / "ThemeSeedGenerated.cpp"

BANNER = "// GENERATED FILE — DO NOT EDIT."


def is_compound(value) -> bool:
    """A gradient or font object, as opposed to a nested token namespace.

    The type/family keys must hold STRINGS. The `font` node itself has a
    `family` key whose value is the {ui, mono, segment7, ...} namespace, so a
    bare `"family" in value` test wrongly claims `font` is a compound and then
    trips over `size` being a dict.
    """
    if not isinstance(value, dict):
        return False
    return (isinstance(value.get("type"), str)
            or isinstance(value.get("family"), str))


def flatten(node, prefix: str = "") -> dict:
    """Nested dict -> dotted keys, stopping at compound objects and scalars."""
    out = {}
    for key, value in (node or {}).items():
        path = f"{prefix}.{key}" if prefix else key
        if isinstance(value, dict) and not is_compound(value):
            out.update(flatten(value, path))
        else:
            out[path] = value
    return out


def resolve(value, primitives: dict, depth: int = 0):
    """Expand {color.red.500} against the primitives palette, recursively.

    Hard-fails on an alias with no matching primitive rather than passing the
    literal `{...}` string through — see the module docstring.

    `depth` budgets the ALIAS CHAIN only, so descending into a gradient's stop
    list doesn't spend it. Lists are walked because gradient stops live in one:
    an alias there would otherwise survive to `QColor("{color.blue.500}")`.
    """
    if isinstance(value, str):
        match = re.fullmatch(r"\{([^}]+)\}", value.strip())
        if match:
            if depth >= 8:
                raise SystemExit(f"FAIL: alias chain deeper than 8 at {value!r}")
            if match.group(1) not in primitives:
                raise SystemExit(
                    f"FAIL: {value!r} names a primitive that does not exist in "
                    f"{THEME_JSON.name}. The seed would compile in the literal "
                    f"string, which QColor reads as transparent — exactly the "
                    f"failure this generator exists to remove.")
            return resolve(primitives[match.group(1)], primitives, depth + 1)
    if isinstance(value, list):
        return [resolve(v, primitives, depth) for v in value]
    if isinstance(value, dict):
        return {k: resolve(v, primitives, depth) for k, v in value.items()}
    return value


def collect(node, scope: str = "root", acc: dict | None = None) -> dict:
    """(scope, token) -> raw value, over the whole scope tree."""
    acc = acc if acc is not None else {}
    for token, value in flatten(node.get("tokens", {})).items():
        acc[(scope, token)] = value
    for name, child in node.get("scopes", {}).items():
        collect(child, name if scope == "root" else f"{scope}/{name}", acc)
    return acc


def cpp_string(text: str) -> str:
    return '"' + str(text).replace("\\", "\\\\").replace('"', '\\"') + '"'


def emit_gradient(token: str, value: dict, indent: str, target: str) -> list[str]:
    lines = [f"{indent}{{"]
    lines.append(f"{indent}    ThemeGradient g;")
    radial = value.get("type") == "radial-gradient"
    lines.append(f"{indent}    g.type = ThemeGradient::"
                 f"{'Radial' if radial else 'Linear'};")
    lines.append(f"{indent}    g.angle = {float(value.get('angle', 180.0))};")
    if radial:
        centre = value.get("center", [0.5, 0.5])
        if isinstance(centre, list) and len(centre) >= 2:
            lines.append(f"{indent}    g.center = QPointF({float(centre[0])}, "
                         f"{float(centre[1])});")
        lines.append(f"{indent}    g.radius = {float(value.get('radius', 0.5))};")
    for stop in value.get("stops", []):
        lines.append(f"{indent}    g.stops.append({{{float(stop.get('at', 0.0))}, "
                     f"QColor({cpp_string(stop.get('color', '#000000'))})}});")
    lines.append(f"{indent}    {target}{'' if target.endswith(', ') else '('}"
                 f"{cpp_string(token)}, QVariant::fromValue(g));")
    lines.append(f"{indent}}}")
    return lines


def emit_font(token: str, value: dict, indent: str, target: str) -> list[str]:
    lines = [f"{indent}{{"]
    lines.append(f"{indent}    ThemeFont f;")
    lines.append(f"{indent}    f.family = QStringLiteral({cpp_string(value.get('family', ''))});")
    if value.get("size"):
        lines.append(f"{indent}    f.size = {int(value['size'])};")
    if value.get("color"):
        lines.append(f"{indent}    f.color = QColor({cpp_string(value['color'])});")
    lines.append(f"{indent}    {target}{'' if target.endswith(', ') else '('}"
                 f"{cpp_string(token)}, QVariant::fromValue(f));")
    lines.append(f"{indent}}}")
    return lines


def emit_scalar(token: str, value, indent: str, target: str) -> list[str]:
    if isinstance(value, bool):
        literal = "true" if value else "false"
    elif isinstance(value, int):
        literal = str(value)
    elif isinstance(value, float):
        literal = repr(value)
    else:
        literal = f"QString({cpp_string(value)})"
    opener = "" if target.endswith(", ") else "("
    return [f"{indent}{target}{opener}{cpp_string(token)}, {literal});"]


def generate() -> str:
    data = json.loads(THEME_JSON.read_text(encoding="utf-8"))
    primitives = flatten(data.get("primitives", {}))
    tokens = collect(data["scopes"]["root"])

    by_scope: dict[str, list] = {}
    for (scope, token), value in tokens.items():
        by_scope.setdefault(scope, []).append((token, resolve(value, primitives)))

    out: list[str] = []
    out.append(BANNER)
    out.append("//")
    out.append("// Regenerate with:  python tools/gen_theme_seed.py")
    out.append("// Source of truth:  resources/themes/default-dark.json")
    out.append("//")
    out.append("// ThemeManager::seedBuiltinDefaults() used to be a hand-maintained copy of")
    out.append("// that JSON. It drifted (9 tokens) and was incomplete (24 tokens never")
    out.append("// seeded at all, resolving TRANSPARENT on themes that predate them) — see")
    out.append("// #3184. Generating it makes the resource authoritative by construction.")
    out.append("//")
    out.append("// `{primitive}` aliases are resolved to concrete values here on purpose: the")
    out.append("// seed runs before any primitives palette exists, and older user themes have")
    out.append("// no primitives section at all.")
    out.append("")
    out.append('#include "ThemeManager.h"')
    out.append("")
    out.append("#include <QColor>")
    out.append("#include <QPointF>")
    out.append("#include <QVariant>")
    out.append("")
    out.append("namespace AetherSDR {")
    out.append("")
    out.append("void ThemeManager::seedGeneratedDefaults()")
    out.append("{")

    for token, value in sorted(by_scope.get("root", [])):
        if is_compound(value) and "type" in value:
            out += emit_gradient(token, value, "    ", "m_tokens.insert")
        elif is_compound(value):
            out += emit_font(token, value, "    ", "m_tokens.insert")
        else:
            out += emit_scalar(token, value, "    ", "m_tokens.insert")

    for scope in sorted(k for k in by_scope if k != "root"):
        rows = sorted(by_scope[scope])
        if not rows:
            continue
        out.append("")
        out.append(f"    // scope: {scope}")
        # seedScopedToken() rather than scopeOrCreate() + s->tokens: ThemeScope's
        # definition is private to ThemeManager.cpp, so this translation unit
        # must never name it.
        target = (f"seedScopedToken(QStringLiteral({cpp_string(scope)}), ")
        for token, value in rows:
            if is_compound(value) and "type" in value:
                out += emit_gradient(token, value, "    ", target)
            elif is_compound(value):
                out += emit_font(token, value, "    ", target)
            else:
                out += emit_scalar(token, value, "    ", target)

    out.append("}")
    out.append("")
    out.append("} // namespace AetherSDR")
    out.append("")
    return "\n".join(out)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate the compiled theme seed from the bundled JSON")
    parser.add_argument("--check", action="store_true",
                        help="exit 1 if the generated file is stale")
    parser.add_argument("--stdout", action="store_true",
                        help="print instead of writing")
    args = parser.parse_args()

    text = generate()

    if args.stdout:
        sys.stdout.write(text)
        return 0

    if args.check:
        # Completeness, checked independently of byte-equality.
        #
        # Byte-equality alone would happily pass a generator that silently
        # dropped a token family — the file would match its own (wrong) output.
        # This asserts every token in the bundled JSON actually reaches the
        # emitted table, which is the property that was violated before the
        # generator existed: 24 tokens had no compiled default at all and
        # resolved TRANSPARENT on themes predating them. (#3184)
        #
        # Compares the (scope, token) SETS, not the counts: a fault that
        # duplicates one token and drops another leaves the count unchanged,
        # and a count can only say "something is missing", not what.
        data = json.loads(THEME_JSON.read_text(encoding="utf-8"))
        expected = set(collect(data["scopes"]["root"]))
        emitted = {("root", m.group(1)) for m in
                   re.finditer(r'm_tokens\.insert\("([^"]+)"', text)}
        emitted |= {(m.group(1), m.group(2)) for m in re.finditer(
            r'seedScopedToken\(QStringLiteral\("([^"]+)"\), "([^"]+)"', text)}
        if emitted != expected:
            print(f"FAIL: the emitted table does not match the theme's token set.\n"
                  f"      missing:    {sorted(expected - emitted)}\n"
                  f"      unexpected: {sorted(emitted - expected)}")
            return 1

        if not OUT_FILE.exists():
            print(f"FAIL: {OUT_FILE.relative_to(REPO)} does not exist — "
                  f"run: python tools/gen_theme_seed.py")
            return 1
        current = OUT_FILE.read_text(encoding="utf-8")
        if current != text:
            print(f"FAIL: {OUT_FILE.relative_to(REPO)} is STALE — the bundled theme "
                  f"changed without regenerating.\n"
                  f"      Run: python tools/gen_theme_seed.py")
            return 1
        print(f"OK: {OUT_FILE.relative_to(REPO)} matches "
              f"{THEME_JSON.relative_to(REPO)}")
        return 0

    OUT_FILE.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {OUT_FILE.relative_to(REPO)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
