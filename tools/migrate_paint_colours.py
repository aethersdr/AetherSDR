"""
Paint-code QColor migration tool (RFC #3076 Phase 3).

Sibling to migrate_colours.py.  Where that tool rewrites hex literals
inside setStyleSheet string arguments, this one rewrites paint-code
QColor literals:

  QColor(0xAA, 0xBB, 0xCC)              → ThemeManager::instance().color("token")
  QColor(0xAA, 0xBB, 0xCC, alpha)       → AetherSDR::theme::withAlpha("token", alpha)
  QColor("#aabbcc")                     → ThemeManager::instance().color("token")
  const QColor kName("#aabbcc")         → inline QColor kName() { return ThemeManager::instance().color("token"); }
  const QColor kName(0xAA,0xBB,0xCC)    → inline QColor kName() { return ThemeManager::instance().color("token"); }

Plus:
  - inserts `#include "core/ThemeManager.h"` into the top-of-file include block
  - updates `kName` → `kName()` call sites to match the new function form

Uses the same CANONICAL_TOKENS table as migrate_colours.py — colours
not in the table are left alone (logged for the cleanup pass).

Limitations (deliberate):
  - 4-arg with non-integer alpha (e.g. computed expressions) is skipped
  - QColor::fromRgb / fromHsv / fromString constructors are skipped
  - Per-call-site context (trace vs axis vs body) isn't considered —
    the hex-to-token table assumes the canonical assignment is right
    everywhere; per-site corrections happen in post-tool review

Usage:
    python tools/migrate_paint_colours.py --files src/gui/SpectrumWidget.cpp
    python tools/migrate_paint_colours.py --files <files...> --apply
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from migrate_colours import CANONICAL_TOKENS, ensure_themeManager_include  # type: ignore


def hex_to_token(hex_str: str) -> str | None:
    """Look up a hex string in the canonical table.  Returns the token
    name or None if not in the table."""
    h = hex_str.lower()
    if h in CANONICAL_TOKENS:
        return CANONICAL_TOKENS[h]
    # Allow #abc → #aabbcc shorthand
    if len(h) == 4 and h.startswith('#'):
        full = '#' + ''.join(c * 2 for c in h[1:])
        if full in CANONICAL_TOKENS:
            return CANONICAL_TOKENS[full]
    return None


def rgb_components_to_hex(components: list[str]) -> str:
    """Convert ['0xAA', '0xBB', '0xCC'] to '#aabbcc' (lowercase)."""
    vals = []
    for c in components:
        c = c.strip()
        if c.startswith('0x') or c.startswith('0X'):
            v = int(c, 16)
        else:
            v = int(c)
        vals.append(v)
    return '#{:02x}{:02x}{:02x}'.format(*vals)


# Matches: QColor(0xAA, 0xBB, 0xCC) or QColor(123, 200, 50) — three args, all numeric
QCOLOR_3ARG_RE = re.compile(
    r'QColor\s*\(\s*'
    r'(0x[0-9a-fA-F]{1,2}|\d{1,3})\s*,\s*'
    r'(0x[0-9a-fA-F]{1,2}|\d{1,3})\s*,\s*'
    r'(0x[0-9a-fA-F]{1,2}|\d{1,3})\s*\)'
)

# Matches: QColor(0xAA, 0xBB, 0xCC, 0xDD) or QColor(R, G, B, A)
QCOLOR_4ARG_RE = re.compile(
    r'QColor\s*\(\s*'
    r'(0x[0-9a-fA-F]{1,2}|\d{1,3})\s*,\s*'
    r'(0x[0-9a-fA-F]{1,2}|\d{1,3})\s*,\s*'
    r'(0x[0-9a-fA-F]{1,2}|\d{1,3})\s*,\s*'
    r'(0x[0-9a-fA-F]{1,3}|\d{1,3})\s*\)'
)

# Matches: QColor("#aabbcc")
QCOLOR_HEX_STR_RE = re.compile(r'QColor\s*\(\s*"(#[0-9a-fA-F]{3,6})"\s*\)')

# Matches:  const QColor kName ("#aabbcc");
#           static const QColor kName("#aabbcc");
# Captures: 1=identifier  2=hex literal
#
# The trailing `[ \t]*` (not `\s*`) keeps the optional inline comment scoped to
# the same line as the semicolon — `\s*` would let the engine swallow the
# newline before a section-header comment on the next line and re-emit it as
# an inline comment on the converted declaration. See #3119.
FILE_SCOPE_HEX_RE = re.compile(
    r'^(?:static\s+)?const\s+QColor\s+([A-Za-z_][A-Za-z0-9_]*)\s*'
    r'\(\s*"(#[0-9a-fA-F]{3,6})"\s*\)\s*;[ \t]*'
    r'(//[^\n]*)?$',
    re.MULTILINE,
)

# Matches:  const QColor kName(0xAA, 0xBB, 0xCC);
#           static const QColor kName(0xAA, 0xBB, 0xCC, 0xDD);
# Captures: 1=ident  2=R  3=G  4=B  5=optional alpha
#
# Same `[ \t]*` rationale as FILE_SCOPE_HEX_RE — keep the inline comment
# capture from swallowing the newline before a section-header comment.
FILE_SCOPE_RGB_RE = re.compile(
    r'^(?:static\s+)?const\s+QColor\s+([A-Za-z_][A-Za-z0-9_]*)\s*'
    r'\(\s*(0x[0-9a-fA-F]{1,2}|\d{1,3})\s*,\s*'
    r'(0x[0-9a-fA-F]{1,2}|\d{1,3})\s*,\s*'
    r'(0x[0-9a-fA-F]{1,2}|\d{1,3})'
    r'(?:\s*,\s*(0x[0-9a-fA-F]{1,3}|\d{1,3}))?\s*\)\s*;[ \t]*'
    r'(//[^\n]*)?$',
    re.MULTILINE,
)


def rewrite_file_scope_consts(content: str) -> tuple[str, int, int]:
    """Convert file-scope `const QColor kName("#hex");` declarations to
    `inline QColor kName() { return ThemeManager::instance().color("token"); }`
    plus rewrite every bare `kName` reference in the file to `kName()`.

    Returns (new_content, replacements, skipped)."""
    rep = 0
    skipped = 0
    renamed_idents: list[str] = []

    def replace_decl_hex(m: re.Match) -> str:
        nonlocal rep, skipped
        ident, hex_str = m.group(1), m.group(2).lower()
        comment = m.group(3) or ''
        tok = hex_to_token(hex_str)
        if tok is None:
            skipped += 1
            return m.group(0)
        rep += 1
        renamed_idents.append(ident)
        tail = f'  {comment}' if comment else ''
        return (f'inline QColor {ident}() {{ return '
                f'AetherSDR::ThemeManager::instance().color("{tok}"); }}{tail}')

    def replace_decl_rgb(m: re.Match) -> str:
        nonlocal rep, skipped
        ident = m.group(1)
        hex_str = rgb_components_to_hex([m.group(2), m.group(3), m.group(4)])
        alpha = m.group(5)
        comment = m.group(6) or ''
        tok = hex_to_token(hex_str)
        if tok is None:
            skipped += 1
            return m.group(0)
        rep += 1
        renamed_idents.append(ident)
        tail = f'  {comment}' if comment else ''
        if alpha is not None:
            body = f'AetherSDR::theme::withAlpha("{tok}", {alpha.strip()})'
        else:
            body = f'AetherSDR::ThemeManager::instance().color("{tok}")'
        return f'inline QColor {ident}() {{ return {body}; }}{tail}'

    content = FILE_SCOPE_HEX_RE.sub(replace_decl_hex, content)
    content = FILE_SCOPE_RGB_RE.sub(replace_decl_rgb, content)

    # Now rewrite call sites: any bare `ident` not followed by `(` becomes
    # `ident()`. The negative lookahead avoids double-rewriting our own
    # injected `ident() { ... }` declaration line.
    for ident in renamed_idents:
        call_site_re = re.compile(rf'\b{re.escape(ident)}\b(?!\s*\()')
        content = call_site_re.sub(f'{ident}()', content)

    return content, rep, skipped


def rewrite_paint_qcolor(content: str) -> tuple[str, int, int]:
    """Returns (new_content, replacements, skipped_unmatched_hex)."""
    rep = 0
    skipped = 0

    def replace_3arg(m: re.Match) -> str:
        nonlocal rep, skipped
        hex_str = rgb_components_to_hex([m.group(1), m.group(2), m.group(3)])
        tok = hex_to_token(hex_str)
        if tok is None:
            skipped += 1
            return m.group(0)
        rep += 1
        return f'AetherSDR::ThemeManager::instance().color("{tok}")'

    def replace_4arg(m: re.Match) -> str:
        nonlocal rep, skipped
        hex_str = rgb_components_to_hex([m.group(1), m.group(2), m.group(3)])
        tok = hex_to_token(hex_str)
        if tok is None:
            skipped += 1
            return m.group(0)
        alpha = m.group(4).strip()
        rep += 1
        return f'AetherSDR::theme::withAlpha("{tok}", {alpha})'

    def replace_hex_str(m: re.Match) -> str:
        nonlocal rep, skipped
        tok = hex_to_token(m.group(1).lower())
        if tok is None:
            skipped += 1
            return m.group(0)
        rep += 1
        return f'AetherSDR::ThemeManager::instance().color("{tok}")'

    content = QCOLOR_4ARG_RE.sub(replace_4arg, content)
    content = QCOLOR_3ARG_RE.sub(replace_3arg, content)
    content = QCOLOR_HEX_STR_RE.sub(replace_hex_str, content)
    return content, rep, skipped


def process_file(path: Path, apply: bool) -> tuple[int, int]:
    try:
        text = path.read_text(encoding='utf-8', errors='replace')
    except OSError:
        return 0, 0
    # File-scope `const QColor kName(...);` first — those declarations also
    # match the QColor(...) regexes below, so we lift them out into inline
    # functions first and the remaining QColor(...) literals inside paint
    # functions get handled by the regular pass.
    text, fs_rep, fs_skipped = rewrite_file_scope_consts(text)
    new_text, rep, skipped = rewrite_paint_qcolor(text)
    total_rep = fs_rep + rep
    total_skipped = fs_skipped + skipped
    if total_rep == 0:
        return 0, total_skipped
    new_text = ensure_themeManager_include(new_text)
    if apply:
        path.write_text(new_text, encoding='utf-8')
    return total_rep, total_skipped


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--files', nargs='+', required=True,
                   help='Specific files to migrate (no auto-discovery — paint code needs targeting)')
    p.add_argument('--apply', action='store_true',
                   help='Write rewrites to disk (default: dry-run)')
    args = p.parse_args()

    total_rep = 0
    total_skipped = 0
    for f in args.files:
        rep, skipped = process_file(Path(f), args.apply)
        print(f'  {f}: {rep} migrated, {skipped} skipped (not in canonical table)')
        total_rep += rep
        total_skipped += skipped
    print()
    print(f'Total: {total_rep} QColor literals migrated, {total_skipped} skipped')
    if not args.apply:
        print('(dry-run — pass --apply to actually rewrite)')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
