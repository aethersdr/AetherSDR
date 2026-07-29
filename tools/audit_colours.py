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

# Windows consoles commonly default to a legacy code page (cp1252), which cannot
# encode the non-ASCII characters in this script's summary output — printing one
# raised UnicodeEncodeError and killed the run *after* the CSV was written but
# *before* the migration-target summary appeared.  Re-open stdout/stderr as UTF-8
# and replace anything the terminal genuinely cannot render, so the tool prints a
# useful (if occasionally substituted) summary everywhere instead of aborting.
for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, 'reconfigure'):
        _stream.reconfigure(encoding='utf-8', errors='replace')

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

# `#1839` in "see issue #1839" matches the pattern above but is an ISSUE
# REFERENCE, not a colour — and this repo's house style is to cite the issue
# number in the log line that fixes it, so they are everywhere. On main today
# that is 29 of the 650 "unique colours" (45 references).
#
# Cosmetic under a count-based ratchet; fatal under a set-based one, where a PR
# whose only change is `qCWarning(...) << "... (#1234)"` would mint a
# never-before-seen colour and fail the gate. A tool that cries wolf on the
# first unrelated logging change will not survive contact with contributors.
#
# Rule: a 4- or 5-digit run of DECIMAL digits is an issue number. Real colours
# are 3, 6 or 8 hex digits — 4 and 5 are not valid CSS/Qt lengths at all — so
# nothing legitimate is excluded. A 4-digit value containing a-f is still
# rejected on length, exactly as before.
ISSUE_REFERENCE = re.compile(r'^#[0-9]{4,5}$')


def is_issue_reference(token: str) -> bool:
    """True for '#1839'-style issue/PR citations that are not colours."""
    return bool(ISSUE_REFERENCE.match(token))
STRING_LITERAL    = re.compile(r'"((?:[^"\\]|\\.)*)"')

# Setstylesheet call sites — count without parsing the argument; the
# string-literal scanner above catches the colours inside.
SET_STYLESHEET    = re.compile(r'\bsetStyleSheet\s*\(')

# ── Ratchet: files whose hardcoded colours are NOT taxonomy violations ──────
#
# Excluded BY PATH, not by suggested token. suggest_token() is called once per
# COLOUR, not per reference, so a colour shared between a palette and a button
# gets one bucket for both — the classifier cannot carry an exclusion rule even
# now that its context is fixed.
#
# ThemeManager.cpp is the important entry and the one that makes the ratchet
# usable at all: its ~97 references ARE the canonical default token values. They
# define the taxonomy rather than violating it, and without this every
# legitimate token addition would read as a regression.
COLOUR_ALLOWLIST = (
    'src/core/ThemeManager.cpp',        # the default token values themselves
    'src/core/ThemeSeedGenerated.cpp',  # generated from the bundled theme (#3184)
)

# Recorded ceiling. These may only ever SHRINK: --strict fails when the tree
# exceeds them, --update-baseline rewrites them after a genuine reduction.
#
# Deliberately COUNTS, not a set of colours, for now. A set is the right
# endpoint — it is what stops a lateral swap (delete #1a2a3a, add #1b2b3b:
# count flat, taxonomy worse), exactly as check_engine_boundary.py's EB3 argues
# for vendor includes. But EB3 earned that strictness because its inputs are
# unambiguous `#include` stems, whereas colour extraction is a text heuristic.
# Flip to a per-file set once this scanner has run clean for a while; the
# phantom-issue-number class it just fixed is the reason not to do both at once.
COLOUR_BASELINE = {
    'unique_colours':   607,
    'total_references': 2710,
    'setstylesheet':    1111,
}


def is_allowlisted(path: Path) -> bool:
    posix = path.as_posix()
    return any(posix.endswith(entry) for entry in COLOUR_ALLOWLIST)


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
    if len(s) == 8:
        # Qt's #AARRGGBB carries alpha in the LEADING pair, so the RGB triple is
        # the LAST six characters. Truncating to the first six turned
        # MainWindow.cpp's #FFFFC857 into #ffffc8 instead of #ffc857 — one site
        # today, but a baseline would enshrine the wrong value.
        return '#' + s[2:]
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
                if is_issue_reference(hex_m.group(0)):
                    continue   # "…restarting RX (#1361)" is not a colour
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
    p.add_argument('--strict', action='store_true',
                   help='Exit 1 if any tracked count exceeds its recorded baseline')
    p.add_argument('--update-baseline', action='store_true',
                   help='Rewrite the baseline from the current tree (only ever shrinks)')
    args = p.parse_args()

    src_root = Path(args.src)
    if not src_root.is_dir():
        print(f'audit_colours: not a directory: {src_root}', file=sys.stderr)
        return 1

    paths = [p for p in src_root.rglob('*')
             if p.is_file() and p.suffix in {'.cpp', '.h', '.hpp', '.cc'}]
    allowlisted = [p for p in paths if is_allowlisted(p)]
    paths = [p for p in paths if not is_allowlisted(p)]

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
        # Context = file stems AND the surrounding source line.
        #
        # This used to be stems only, which quietly disabled several
        # classifier branches: the waterfall/colormap test could fire only if a
        # FILE was named for it, but the palettes live in SpectrumWidget.cpp, so
        # every palette colour fell through to the later 'spectrum' branch and
        # `color.waterfall.*` resolved 0 references. `border` and `slice` were
        # dead for the same reason. The docstring already promised "nearby
        # identifiers"; scan_file was already recording `snippet`. It simply was
        # never passed in.
        ctx = ' '.join(Path(r['file']).stem for r in refs[:5])
        ctx += ' ' + ' '.join(r.get('snippet', '') for r in refs[:5])
        suggestions[colour] = suggest_token(colour, ctx)

    if not args.summary_only:
        out_path = Path(args.out)
        # Explicit UTF-8: the default encoding is locale-dependent, so a source
        # path containing a non-ASCII character would fail the write on a
        # legacy-code-page Windows console.
        with out_path.open('w', newline='', encoding='utf-8') as fh:
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

    # ── Ratchet ────────────────────────────────────────────────────────────
    current = {
        'unique_colours':   len(by_colour),
        'total_references': len(all_records),
        'setstylesheet':    setStyleSheet_count,
    }

    if args.update_baseline:
        print()
        print('--- baseline update ---')
        for key, value in current.items():
            recorded = COLOUR_BASELINE[key]
            if value > recorded:
                print(f'  REFUSED {key}: {value} > recorded {recorded} — the '
                      f'baseline only shrinks. Reduce first, then update.')
                return 1
            print(f'  {key}: {recorded} -> {value}')
        print('\nEdit COLOUR_BASELINE in this file to the values above.')
        return 0

    if allowlisted:
        print()
        print(f'Allow-listed (excluded by path, {len(allowlisted)} file(s)):')
        for path in allowlisted:
            print(f'  {path.as_posix()}')

    print()
    print('=== ratchet ===')
    regressions = []
    for key, value in current.items():
        recorded = COLOUR_BASELINE[key]
        marker = 'OK ' if value <= recorded else 'OVER'
        print(f'  {marker} {key:18} {value:5d}  (baseline {recorded})')
        if value > recorded:
            regressions.append((key, value, recorded))

    if regressions:
        print()
        print('FAIL: the hardcoded-colour count rose above its recorded baseline.')
        for key, value, recorded in regressions:
            print(f'  {key}: {value} > {recorded}  (+{value - recorded})')
        print('\nEither migrate the new colour to a token in docs/theming/'
              'canonical-tokens.md, or — if it genuinely belongs hardcoded — add '
              'its file to COLOUR_ALLOWLIST with a reason.')
        if args.strict:
            return 1

    return 0


if __name__ == '__main__':
    raise SystemExit(main())
