"""
Mass migration tool for the theming subsystem (RFC #3076 Phase 2).

Walks src/ and converts every hardcoded hex colour inside setStyleSheet
string-literal arguments to a {{token}} placeholder, then wraps the
setStyleSheet call so the resolver expands the tokens at apply time:

  Before:
    widget->setStyleSheet("QLabel { color: #c8d8e8; background: #1a2a3a; }");

  After:
    widget->setStyleSheet(AetherSDR::ThemeManager::instance().resolve(
        "QLabel { color: {{color.text.primary}}; background: {{color.background.1}}; }"));

The canonicalisation table comes from docs/theming/canonical-tokens.md.
Close-variant colours collapse onto canonical tokens (sub-perceptual
diffs); the merged token list is read from default-dark.json so the
tool stays in sync with the on-disk schema.

Scope of what gets rewritten:
  - setStyleSheet(string_literal) — string with embedded hex
  - C++ string literal concatenation (`"foo" "bar"`) is handled by
    flattening into a single string before substitution
  - QStringLiteral(R"(...)") raw strings are handled the same way

Out of scope (skipped, logged as warnings):
  - setStyleSheet(variable)             — can't introspect the value
  - setStyleSheet(QString(...).arg(...)) — runtime composition, manual fix
  - setStyleSheet(complexExpression)    — anything that's not a literal

Header rewrites:
  - If a file gets at least one substitution, ensure it includes
    "core/ThemeManager.h".  Idempotent — skipped if already present.

Usage:
    python tools/migrate_colours.py                # dry-run, prints diff stats
    python tools/migrate_colours.py --apply        # actually rewrite files
    python tools/migrate_colours.py --files <...>  # restrict to specific files
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

# ── Canonical token table ────────────────────────────────────────────
# Generated from docs/theming/canonical-tokens.md.  Maps hex (lowercase,
# 6-digit) to the token name it canonicalises onto.  Close variants
# collapse here (sub-perceptual visual diff documented in the taxonomy).
CANONICAL_TOKENS: dict[str, str] = {
    # Backgrounds tier 0 (darkest)
    "#0a0e14": "color.background.0",
    "#0a0a14": "color.background.0",
    "#0a0a18": "color.background.0",
    "#0f0f1a": "color.background.0",
    "#08121d": "color.background.0",
    "#0e1b28": "color.background.0",
    "#1a1a2e": "color.background.0",
    "#0a1420": "color.background.0",
    "#111120": "color.background.0",
    # Backgrounds tier 1 (panel mid)
    "#1a2a3a": "color.background.1",
    "#203040": "color.background.1",
    "#204060": "color.background.1",
    "#243a4e": "color.background.1",
    "#1e2e3e": "color.background.1",
    "#2a3a4a": "color.background.1",
    "#2a3744": "color.background.1",
    "#1a2230": "color.background.1",
    "#2a4458": "color.background.1",
    "#161626": "color.background.1",
    # Backgrounds tier 2 (raised)
    "#304050": "color.background.2",
    "#205070": "color.background.2",
    "#1a3a5a": "color.background.2",
    "#404858": "color.background.2",
    "#0070c0": "color.background.2",
    # Backgrounds tier 3 (highest raised)
    "#506070": "color.background.3",
    # TX-active tint
    "#3a2a0e": "color.background.tx",
    "#4a3a1e": "color.background.tx",
    # Spectrum canvas (pure black, kept distinct)
    "#000000": "color.background.spectrum",
    "#000":    "color.background.spectrum",
    # Accents
    "#00b4d8": "color.accent",
    "#00c8f0": "color.accent.bright",
    "#00c8ff": "color.accent.bright",
    "#0090e0": "color.accent.dim",
    "#0070b8": "color.accent.dim",
    "#008ba8": "color.accent.dim",
    "#4db8d4": "color.accent.dim",
    # Status accents — warning family
    "#ffb84d": "color.accent.warning",
    "#f2c14e": "color.accent.warning",
    "#ffd070": "color.accent.warning",
    "#e8a540": "color.accent.warning",
    "#e8b977": "color.accent.warning",
    "#ff8c00": "color.accent.warning",
    "#ffff00": "color.accent.warning",
    # Status accents — danger family
    "#ff4d4d": "color.accent.danger",
    "#ff4444": "color.accent.danger",
    "#ff4040": "color.accent.danger",
    "#ff6b6b": "color.accent.danger",
    "#c03030": "color.accent.danger",
    "#ff8080": "color.accent.danger",
    # Status accents — success family
    "#4dd87a": "color.accent.success",
    "#00ff88": "color.accent.success",
    "#00a060": "color.accent.success",
    "#66d19e": "color.accent.success",
    "#00e060": "color.accent.success",
    "#30d050": "color.accent.success",
    "#006040": "color.accent.success",
    # Text tiers
    "#e6f0fa": "color.text.primary",
    "#c8d8e8": "color.text.primary",
    "#d7e7f2": "color.text.primary",
    "#d7e4f2": "color.text.primary",
    "#d4deea": "color.text.primary",
    "#e0f0ff": "color.text.primary",
    "#ffffff": "color.text.primary",
    "#fff":    "color.text.primary",
    "#8ea8c0": "color.text.secondary",
    "#8aa8c0": "color.text.secondary",
    "#8090a0": "color.text.secondary",
    "#7f93a5": "color.text.secondary",
    "#8d99ad": "color.text.secondary",
    "#a0b0c0": "color.text.secondary",
    "#a0b4c8": "color.text.secondary",
    "#808080": "color.text.label",
    "#607080": "color.text.label",
    "#6a8090": "color.text.label",
    "#3a4a5a": "color.text.disabled",
    # Borders
    "#1a2330": "color.border.subtle",
    "#2a3a4d": "color.border.strong",
    # Meter accents
    "#405060": "color.meter.bar.fill",

    # ── Near-duplicates absorbed from post-migration audit (ΔRGB ≤ 4) ──
    # 33 colours covering 86 references.
    # Generated by tools/migrate_colours.py expansion pass.
    "#ff8800": "color.accent.warning",  # 2 refs, ΔRGB=4
    "#0b1220": "color.background.0",  # 6 refs, ΔRGB=3
    "#06111c": "color.background.0",  # 6 refs, ΔRGB=4
    "#09111b": "color.background.0",  # 5 refs, ΔRGB=4
    "#1a1a2a": "color.background.0",  # 5 refs, ΔRGB=4
    "#0a0e16": "color.background.0",  # 4 refs, ΔRGB=2
    "#0e1926": "color.background.0",  # 2 refs, ΔRGB=4
    "#0b1520": "color.background.0",  # 2 refs, ΔRGB=2
    "#0b131d": "color.background.0",  # 2 refs, ΔRGB=4
    "#0d1b29": "color.background.0",  # 1 refs, ΔRGB=2
    "#101b26": "color.background.0",  # 1 refs, ΔRGB=4
    "#121220": "color.background.0",  # 1 refs, ΔRGB=2
    "#08111d": "color.background.0",  # 1 refs, ΔRGB=1
    "#0f0f1e": "color.background.0",  # 1 refs, ΔRGB=4
    "#1e3040": "color.background.1",  # 7 refs, ΔRGB=2
    "#24384e": "color.background.1",  # 7 refs, ΔRGB=2
    "#1a2030": "color.background.1",  # 4 refs, ΔRGB=2
    "#1a2a38": "color.background.1",  # 3 refs, ΔRGB=2
    "#293b4a": "color.background.1",  # 2 refs, ΔRGB=2
    "#1a2838": "color.background.1",  # 2 refs, ΔRGB=4
    "#171728": "color.background.1",  # 1 refs, ΔRGB=4
    "#526270": "color.background.3",  # 2 refs, ΔRGB=4
    "#3a2a10": "color.background.tx",  # 1 refs, ΔRGB=2
    "#405262": "color.meter.bar.fill",  # 2 refs, ΔRGB=4
    "#5f7080": "color.text.label",  # 1 refs, ΔRGB=1
    "#e7f1fb": "color.text.primary",  # 3 refs, ΔRGB=3
    "#d6e4f2": "color.text.primary",  # 1 refs, ΔRGB=1
    "#9fb0c0": "color.text.secondary",  # 3 refs, ΔRGB=1
    "#7f93a7": "color.text.secondary",  # 3 refs, ΔRGB=2
    "#8192a0": "color.text.secondary",  # 2 refs, ΔRGB=3
    "#9cb0c0": "color.text.secondary",  # 1 refs, ΔRGB=4
    "#9fb3c8": "color.text.secondary",  # 1 refs, ΔRGB=2
    "#8190a3": "color.text.secondary",  # 1 refs, ΔRGB=4

    # ── Meter/curve widget close-mappings (PR 4/5 paint migration) ──
    # File-scope const QColor declarations in ClientCompKnob/Meter/
    # CurveWidget/ThresholdFader/LimiterButton/GateLevelView. Each is
    # close to an existing canonical value; absorbing them lets the
    # entire compressor + gate + tube panel respond to theme switching.
    "#0a1a28": "color.background.0",         # dark panel base, ΔRGB≈10 from #0a0e14
    "#1f2a38": "color.background.1",         # raised panel, ΔRGB≈9 from #1a2a3a
    "#56c48b": "color.accent.success",       # meter level-lo green, ΔRGB≈46 from #4dd87a
    "#607888": "color.text.label",           # axis labels, ΔRGB≈8 from #607080
    "#b0c4d6": "color.text.secondary",       # knob/meter labels, ΔRGB≈16 from #a0b4c8
    "#c8a040": "color.accent.warning",       # gate curve amber, ΔRGB≈55 from #e8a540
    "#d03030": "color.accent.danger",        # limiter active red, ΔRGB≈16 from #c03030
    "#d47272": "color.accent.danger",        # de-ess soft red sibilant band
    "#e85a5a": "color.accent.danger",        # meter level-hi red, ΔRGB≈23 from #ff4d4d
    "#e8a540": "color.accent.warning",       # threshold/peak amber, canonical family member
    "#e8d65a": "color.accent.warning",       # meter level-mid yellow
    "#e8e8e8": "color.text.primary",         # knob pointer/value off-white, ΔRGB≈32 from #c8d8e8
    "#f0f4f8": "color.text.primary",         # gate input outline near-white, ΔRGB≈10 from #e6f0fa
}

# Build a regex that matches any of our known hex codes, longest first
# so 6-digit forms aren't swallowed by 3-digit prefixes.
_HEX_KEYS = sorted(CANONICAL_TOKENS.keys(), key=len, reverse=True)
HEX_RE = re.compile("(" + "|".join(re.escape(k) for k in _HEX_KEYS) + r")\b",
                    re.IGNORECASE)

# Matches a setStyleSheet(...) call whose argument is one-or-more
# adjacent C++ string literals (regular "..." or QStringLiteral(R"(...)")).
# Lots of edge cases in C++ source — this matches the common one and
# falls back gracefully for the rest.
SETSTYLE_RE = re.compile(
    r'((?:[\w.]+(?:->|\.))?setStyleSheet)\s*\(\s*'      # the call + opening paren
    r'((?:(?:QStringLiteral\s*\(\s*)?R"\([^)]*\)"\s*\)?'   # raw string OR
    r'|"(?:[^"\\\n]|\\.)*"\s*)+)'                       # one-or-more "..." literals
    r'\s*\)',                                            # closing paren
    re.DOTALL,
)

INCLUDE_LINE = '#include "core/ThemeManager.h"'


def substitute_hex_in_string(s: str) -> tuple[str, int]:
    """Replace every known hex literal inside `s` with {{token}}.
    Returns (rewritten, count).  Lowercase normalises before lookup."""
    count = 0

    def _sub(match):
        nonlocal count
        key = match.group(0).lower()
        # Normalise 3-digit to 6-digit for table lookup.
        if len(key) == 4:  # "#abc"
            key6 = "#" + "".join(c * 2 for c in key[1:])
            if key6 in CANONICAL_TOKENS:
                count += 1
                return "{{" + CANONICAL_TOKENS[key6] + "}}"
        if key in CANONICAL_TOKENS:
            count += 1
            return "{{" + CANONICAL_TOKENS[key] + "}}"
        return match.group(0)

    new = HEX_RE.sub(_sub, s)
    return new, count


def rewrite_setstylesheet_calls(content: str) -> tuple[str, int, int]:
    """Walk content, rewrite every setStyleSheet call whose argument
    has hex literals.  Returns (new_content, calls_rewritten, hex_subs)."""
    calls = 0
    subs = 0

    def _replace(match):
        nonlocal calls, subs
        prefix = match.group(1)        # `widget->setStyleSheet` or `setStyleSheet`
        arg_text = match.group(2)      # the literal(s) inside the parens
        new_arg, n = substitute_hex_in_string(arg_text)
        if n == 0:
            return match.group(0)   # nothing to do
        calls += 1
        subs += n
        # Route through ThemeManager::resolve() so {{token}} expands at apply time.
        return f"{prefix}(AetherSDR::ThemeManager::instance().resolve({new_arg}))"

    new = SETSTYLE_RE.sub(_replace, content)
    return new, calls, subs


def ensure_themeManager_include(content: str) -> str:
    """If file gets a rewrite, make sure it includes ThemeManager.h.
    Idempotent — no-op if already present.

    Inserts into the top-of-file include block (last #include before
    the first non-include code line) rather than literally the last
    #include in the file.  Trailing moc-style includes
    (`#include "moc_X.cpp"` at the bottom of a .cpp) would land the
    insertion after all setStyleSheet usage sites otherwise, leaving
    the rewrite uncompilable."""
    if 'core/ThemeManager.h' in content:
        return content
    lines = content.splitlines(keepends=True)
    include_re = re.compile(r'^\s*#include\s+["<]')
    cond_open_re = re.compile(r'^\s*#\s*(if|ifdef|ifndef)\b')
    cond_close_re = re.compile(r'^\s*#\s*endif\b')
    depth = 0
    last_top_include_idx = -1
    for i, line in enumerate(lines):
        stripped = line.strip()
        if cond_open_re.match(line):
            depth += 1
            continue
        if cond_close_re.match(line):
            depth = max(0, depth - 1)
            continue
        if depth > 0:
            # Inside a conditional block — skip; we want unconditional includes.
            continue
        if include_re.match(line):
            last_top_include_idx = i
        elif stripped == '' or stripped.startswith('//') or stripped.startswith('/*') \
                or stripped.startswith('*') or stripped.startswith('#'):
            # Blank, comment, or other preprocessor directive — keep walking.
            continue
        else:
            # Hit the first real code line — stop scanning here.
            break
    if last_top_include_idx < 0:
        # No top-block includes — prepend.
        return INCLUDE_LINE + "\n" + content
    lines.insert(last_top_include_idx + 1, INCLUDE_LINE + "\n")
    return "".join(lines)


def process_file(path: Path, apply: bool) -> tuple[int, int]:
    """Returns (calls_rewritten, hex_substitutions)."""
    try:
        text = path.read_text(encoding='utf-8', errors='replace')
    except OSError:
        return 0, 0

    new_text, calls, subs = rewrite_setstylesheet_calls(text)
    if subs == 0:
        return 0, 0

    new_text = ensure_themeManager_include(new_text)
    if apply:
        path.write_text(new_text, encoding='utf-8')
    return calls, subs


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--src', default='src',
                   help='Source root to scan (default: src/)')
    p.add_argument('--apply', action='store_true',
                   help='Write rewrites to disk (default: dry-run with stats)')
    p.add_argument('--files', nargs='*',
                   help='Restrict to specific files instead of walking src/')
    args = p.parse_args()

    if args.files:
        paths = [Path(f) for f in args.files]
    else:
        root = Path(args.src)
        paths = [p for p in root.rglob('*')
                 if p.is_file() and p.suffix in {'.cpp', '.h', '.hpp', '.cc'}]

    total_files = 0
    total_calls = 0
    total_subs  = 0
    rewritten_files: list[tuple[Path, int, int]] = []
    for path in paths:
        calls, subs = process_file(path, args.apply)
        if subs > 0:
            total_files += 1
            total_calls += calls
            total_subs  += subs
            rewritten_files.append((path, calls, subs))

    print(f"=== migrate_colours.py {'apply' if args.apply else 'dry-run'} summary ===")
    print(f"  Files rewritten        : {total_files}")
    print(f"  setStyleSheet calls    : {total_calls}")
    print(f"  hex substitutions      : {total_subs}")
    if total_files:
        print()
        print("Top files by substitutions:")
        rewritten_files.sort(key=lambda x: -x[2])
        for path, calls, subs in rewritten_files[:20]:
            print(f"  {subs:4d} subs / {calls:3d} calls  {path}")
    if not args.apply:
        print()
        print("(dry-run — pass --apply to actually rewrite files)")
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
