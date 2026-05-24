"""
Color and stylesheet inventory for AetherSDR's theming migration (RFC #3076).

Phase 2 of the theming subsystem needs a complete catalog of every hardcoded
colour and stylesheet site in src/ so we can:

  1. Confirm the token taxonomy in default-dark.json covers everything,
  2. See which colours appear how many times (the high-count ones get
     promoted to high-priority migration targets),
  3. Trace each colour back to its source file / line for the actual
     replacement work.

This script does pure static text analysis — no parsing of C++ semantics,
no compile step.  It produces a CSV report plus a brief text summary.

Patterns recognised:
  - QColor("#rrggbb") / QColor("#rgb") — string literal colour
  - QColor(0xrrggbb) — packed-int colour
  - QColor(r, g, b) and QColor(r, g, b, a) — component triple/quad
  - Inline #rrggbb / #rgb inside any setStyleSheet(...) string

Patterns intentionally NOT recognised:
  - Computed colour expressions (e.g. QColor::fromHsl)
  - QPalette modifications (only ~1 use across the whole codebase)
  - Stylesheet colours that come in as variables (rare; we'll catch them
    by visual inspection during migration)

Usage:
    python tools/audit_colours.py [--src src/] [--out /tmp/colour-audit.csv]
"""

from __future__ import annotations

import argparse
import csv
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

# Match QColor("#hex") and QColor(0xhex)
QCOLOR_HEX_STR = re.compile(r'QColor\s*\(\s*"(#[0-9a-fA-F]{3,8})"\s*\)')
QCOLOR_HEX_INT = re.compile(r'QColor\s*\(\s*0x([0-9a-fA-F]{6,8})\b')

# Match QColor(255, 128, 0) / QColor(255, 128, 0, 200).  Stricter integer
# match so we don't pull in QColor(QString("..."), int) or similar.
QCOLOR_RGB = re.compile(
    r'QColor\s*\(\s*'
    r'(\d{1,3})\s*,\s*'
    r'(\d{1,3})\s*,\s*'
    r'(\d{1,3})\s*'
    r'(?:,\s*(\d{1,3})\s*)?\)'
)

# Any #rrggbb / #rgb that appears inside a string literal.  We scan all
# string literals separately so we capture them anywhere (stylesheets,
# QSS resource files inlined as C++ strings, etc.).
HEX_INSIDE_STRING = re.compile(r'#[0-9a-fA-F]{3,8}\b')
STRING_LITERAL    = re.compile(r'"((?:[^"\\]|\\.)*)"')

# Setstylesheet call sites — count without parsing the argument; the
# string-literal scanner above catches the colours inside.
SET_STYLESHEET    = re.compile(r'\bsetStyleSheet\s*\(')

# Naive semantic heuristics for suggested token names.  Two passes:
#   1. By colour family (luma/saturation buckets)
#   2. By call-site context (function name, file name, nearby identifiers)
# A human still has to review the spreadsheet; the suggestions just save
# initial triage time.
def suggest_token(colour: str, ctx: str) -> str:
    c = colour.lower().lstrip('#')
    if len(c) == 3:
        c = ''.join(ch * 2 for ch in c)
    if len(c) < 6:
        return 'color.???'
    r = int(c[0:2], 16)
    g = int(c[2:4], 16)
    b = int(c[4:6], 16)
    # Crude luma; matches Rec.601 enough for bucketing
    luma = (299 * r + 587 * g + 114 * b) / 1000

    ctx_low = ctx.lower()
    if 'meter' in ctx_low or 'crst' in ctx_low or 'thresh' in ctx_low:
        return 'color.meter.???'
    if 'waterfall' in ctx_low or 'colormap' in ctx_low:
        return 'color.waterfall.???'
    if 'slice' in ctx_low and ('label' in ctx_low or 'indicator' in ctx_low):
        return 'color.slice.???'
    if 'spectrum' in ctx_low or 'trace' in ctx_low:
        return 'color.spectrum.???'
    if 'border' in ctx_low or 'outline' in ctx_low:
        return 'color.border.???'

    if luma < 32:
        return 'color.background.0'
    if luma < 64:
        return 'color.background.1'
    if luma < 96:
        return 'color.background.2'
    if luma > 220:
        return 'color.text.primary'
    if 180 < luma <= 220:
        return 'color.text.secondary'
    if 80 < luma <= 130 and abs(r - g) < 32 and abs(g - b) < 32:
        return 'color.text.label'

    # Saturation-leaning colours likely accents
    max_c = max(r, g, b)
    min_c = min(r, g, b)
    if max_c - min_c > 80:
        if r > g and r > b:
            return 'color.accent.danger'
        if g > r and g > b:
            return 'color.accent.success'
        if b > r and b > g and r < 100:
            return 'color.accent'
        if r > 200 and g > 150 and b < 100:
            return 'color.accent.warning'
    return 'color.???'


def strip_comments(src: str) -> str:
    """Strip C++ line and block comments so we don't index colours in
    documentation, license headers, or commented-out code paths."""
    # Block comments
    src = re.sub(r'/\*.*?\*/', ' ', src, flags=re.DOTALL)
    # Line comments
    src = re.sub(r'//[^\n]*', '', src)
    return src


def normalise_colour(raw: str) -> str:
    """Normalise to '#rrggbb' (lowercase, 6-digit) for grouping.  Alpha-
    suffixed and short forms collapse onto the same bucket as the
    underlying RGB."""
    s = raw.lstrip('#').lower()
    if len(s) == 3:
        s = ''.join(ch * 2 for ch in s)
    if len(s) >= 6:
        return '#' + s[:6]
    return '#' + s


def scan_file(path: Path) -> list[dict]:
    """Return one record per colour reference found in `path`."""
    try:
        text = path.read_text(encoding='utf-8', errors='replace')
    except OSError:
        return []
    clean = strip_comments(text)
    lines = clean.split('\n')
    records: list[dict] = []

    for lineno, line in enumerate(lines, start=1):
        # QColor string-literal hex form
        for m in QCOLOR_HEX_STR.finditer(line):
            records.append({
                'colour': normalise_colour(m.group(1)),
                'form':   'QColor("#...")',
                'file':   str(path),
                'line':   lineno,
                'snippet': line.strip()[:160],
            })
        # QColor packed-int form
        for m in QCOLOR_HEX_INT.finditer(line):
            records.append({
                'colour': normalise_colour(m.group(1)),
                'form':   'QColor(0x...)',
                'file':   str(path),
                'line':   lineno,
                'snippet': line.strip()[:160],
            })
        # QColor RGB triple/quad
        for m in QCOLOR_RGB.finditer(line):
            r, g, b = int(m.group(1)), int(m.group(2)), int(m.group(3))
            if r > 255 or g > 255 or b > 255:
                continue
            colour = '#{:02x}{:02x}{:02x}'.format(r, g, b)
            records.append({
                'colour': colour,
                'form':   'QColor(r,g,b)',
                'file':   str(path),
                'line':   lineno,
                'snippet': line.strip()[:160],
            })
        # #xxxxxx inside any string literal on this line
        for str_m in STRING_LITERAL.finditer(line):
            body = str_m.group(1)
            for hex_m in HEX_INSIDE_STRING.finditer(body):
                records.append({
                    'colour': normalise_colour(hex_m.group(0)),
                    'form':   'inline string',
                    'file':   str(path),
                    'line':   lineno,
                    'snippet': line.strip()[:160],
                })

    return records


def main() -> int:
    p = argparse.ArgumentParser(description='Inventory hardcoded colours in src/')
    p.add_argument('--src', default='src', help='Source root to scan (default: src/)')
    p.add_argument('--out', default='/tmp/colour-audit.csv', help='CSV output path')
    p.add_argument('--summary-only', action='store_true',
                   help='Skip CSV; print summary to stdout only')
    args = p.parse_args()

    src_root = Path(args.src)
    if not src_root.is_dir():
        print(f'audit_colours: not a directory: {src_root}', file=sys.stderr)
        return 1

    paths = [p for p in src_root.rglob('*')
             if p.is_file() and p.suffix in {'.cpp', '.h', '.hpp', '.cc'}]

    all_records: list[dict] = []
    setStyleSheet_count = 0
    for path in paths:
        all_records.extend(scan_file(path))
        try:
            setStyleSheet_count += len(SET_STYLESHEET.findall(
                strip_comments(path.read_text(encoding='utf-8', errors='replace'))))
        except OSError:
            pass

    # Group by normalised colour for the spreadsheet.
    by_colour: dict[str, list[dict]] = defaultdict(list)
    for r in all_records:
        by_colour[r['colour']].append(r)

    # Suggested token per colour: use the colour itself + the most
    # common file-name token (best-effort context).
    suggestions = {}
    for colour, refs in by_colour.items():
        ctx = ' '.join(Path(r['file']).stem for r in refs[:5])
        suggestions[colour] = suggest_token(colour, ctx)

    if not args.summary_only:
        out_path = Path(args.out)
        with out_path.open('w', newline='') as fh:
            w = csv.writer(fh)
            w.writerow(['colour', 'count', 'suggested_token',
                        'forms', 'sample_files', 'sample_lines'])
            for colour in sorted(by_colour.keys(), key=lambda c: -len(by_colour[c])):
                refs = by_colour[colour]
                forms = sorted({r['form'] for r in refs})
                sample_files = sorted({Path(r['file']).name for r in refs})[:5]
                sample_lines = [f'{Path(r["file"]).name}:{r["line"]}'
                                for r in refs[:5]]
                w.writerow([colour, len(refs), suggestions[colour],
                            '|'.join(forms), '|'.join(sample_files),
                            '|'.join(sample_lines)])
        print(f'audit_colours: wrote {out_path} ({len(by_colour)} unique colours, '
              f'{len(all_records)} total references)')

    # Summary on stdout — top 20 most-used colours and overall counts.
    top = Counter()
    for colour, refs in by_colour.items():
        top[colour] = len(refs)

    print()
    print('=== AetherSDR colour audit summary ===')
    print(f'  source files scanned        : {len(paths)}')
    print(f'  unique normalised colours   : {len(by_colour)}')
    print(f'  total colour references     : {len(all_records)}')
    print(f'  setStyleSheet() call sites  : {setStyleSheet_count}')
    print()
    print('Top 20 most-used colours (good first migration targets):')
    for colour, count in top.most_common(20):
        print(f'  {colour}  {count:4d}  → {suggestions[colour]}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
