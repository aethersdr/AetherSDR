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
# nothing legitimate is excluded.
#
# DO NOT widen this to 3 digits. Three hex digits is the valid CSS shorthand
# form, and every all-decimal 3-digit token that actually reaches this scanner
# today is a real colour: 20 references / 13 unique colours, including
# `background: #234` and `border: 1px solid #456` in DemoApplet.cpp,
# `background: #444` / `#666` in SpectrumOverlayMenu.cpp, and `color: #000` in
# SpectrumWidget.cpp. Widening the rule would silently delete all of them.
#
# The repo's 3-digit issue citations — `(#745)`, `(#695)`, `(#277)` — never
# reach here at all, because they live in COMMENTS and strip_comments() has
# already removed them. Only citations inside string literals matter, and those
# are 4-digit in this era.
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

# ── Ratchet: measured as a DELTA against the PR's base, not a frozen constant ──
#
# The original design recorded a ceiling here (607/2710/1111 at the time) that
# --strict compared the tree against. That cannot work: `main` gains colours
# from unrelated UI merges, so the constant goes stale within hours of being
# written, and because it is an ABSOLUTE ceiling rather than a delta, once main
# drifts above it EVERY subsequent PR touching src/ fails — including PRs that
# REMOVE colours. It had already gone stale before review (614/2749/1118 vs
# 607/2710/1111, three separate times in one afternoon as UI PRs landed).
#
# Comparing the PR's tree against its base removes the failure mode entirely:
# there is nothing stored to go stale, no re-seeding treadmill, and the question
# it answers — "did THIS PR add colours?" — is the one a reviewer actually wants.
#
# This also retires the reason the original deferred a set-based gate to stage 2.
# The objection was that freezing a baseline today would bake in the scanner's
# own phantoms. A delta stores nothing: both trees are scanned by the SAME
# scanner in the SAME run, so a phantom appears identically on both sides and
# cancels out. New colours are therefore reported BY NAME below.
#
# Reporting them, not failing on them, is deliberate — gating on the set is a
# contributor-experience policy call that belongs to the original author and the
# maintainers, not to a review fix. Promoting it is a one-line change: fail when
# `new_colours` is non-empty.
TRACKED_METRICS = ('unique_colours', 'total_references', 'setstylesheet')


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
    documentation, license headers, or commented-out code paths.

    Line COUNT is preserved: a multi-line block comment collapses to its own
    newlines rather than to a single space. Replacing it with ' ' shifted every
    subsequent line number in the file (a 4-line licence header moved
    `QColor("#123456")` from line 5 to line 3), which silently corrupted the
    `file:line` column that purpose 3 in this module's docstring — tracing a
    colour back to its source for the actual replacement work — depends on.
    """
    # Block comments: keep one newline per line consumed so line numbers hold.
    src = re.sub(r'/\*.*?\*/',
                 lambda m: '\n' * m.group(0).count('\n'),
                 src, flags=re.DOTALL)
    # Line comments never span a newline, so removing the text is enough.
    src = re.sub(r'//[^\n]*', '', src)
    return src


def normalise_colour(raw: str) -> str:
    """Normalise to '#rrggbb' (lowercase, 6-digit) for grouping, or return None
    if `raw` is not a valid colour length.

    Alpha-suffixed and short forms collapse onto the same bucket as the
    underlying RGB.
    """
    s = raw.lstrip('#').lower()
    if len(s) == 3:
        s = ''.join(ch * 2 for ch in s)
    if len(s) == 8:
        # Qt's #AARRGGBB carries alpha in the LEADING pair, so the RGB triple is
        # the LAST six characters. Truncating to the first six turned
        # MainWindow.cpp's #FFFFC857 into #ffffc8 instead of #ffc857.
        #
        # Note this assumes the Qt spelling. CSS/QSS 8-digit hex is #RRGGBBAA,
        # i.e. the other way round — but the only 8-digit token in src/ today is
        # that one QColor site, so the Qt reading is the correct one to take.
        # Revisit if an 8-digit literal ever appears inside a stylesheet string.
        s = s[2:]
    if len(s) != 6:
        # 4, 5 and 7 hex digits are not valid colour lengths in CSS or Qt.
        # HEX_INSIDE_STRING matches {3,8}, so `#abcd` reaches here and used to
        # be returned verbatim as a 4-character "colour" that then counted as a
        # unique entry — the same phantom class as the issue references above,
        # just spelled with a-f so the decimal filter never saw it. (The old
        # comment on ISSUE_REFERENCE claimed these were "rejected on length";
        # they were not.) Zero occurrences in src/ today, so this is a latent
        # class being closed, not a live count change.
        return None
    return '#' + s


def declaration_at(line: str, offset: int) -> str:
    """Return the ';'-delimited fragment of `line` containing `offset`.

    This is the classifier's context, and it is deliberately NARROWER than the
    whole line. suggest_token() substring-matches its context, and a QSS line
    almost always carries a `border:` or `border-radius:` declaration somewhere,
    so handing it the full line made the border/outline branch pre-empt every
    luma bucket: 199 colours / 1150 of 2710 references classified as
    color.border.???, and unresolved '???' references went from 31% to 69%.

    `background: #1a2a3a; border: 1px solid #205070` is the canonical case —
    117 references that are correctly color.background.1, mis-bucketed as border
    because of the NEIGHBOURING declaration. Splitting on ';' keeps each colour
    with the property that actually sets it.
    """
    start = line.rfind(';', 0, offset) + 1
    end = line.find(';', offset)
    if end == -1:
        end = len(line)
    return line[start:end].strip()[:160]


def scan_file(path: Path) -> list[dict]:
    """Return one record per colour reference found in `path`."""
    try:
        text = path.read_text(encoding='utf-8', errors='replace')
    except OSError:
        return []
    clean = strip_comments(text)
    lines = clean.split('\n')
    records: list[dict] = []

    def add(colour, form, lineno, line, offset):
        # normalise_colour returns None for lengths that are not real colours.
        if colour is None:
            return
        records.append({
            'colour': colour,
            'form':   form,
            'file':   str(path),
            'line':   lineno,
            # Full line: what a human reads in the CSV.
            'snippet': line.strip()[:160],
            # Just this colour's declaration: what the classifier reads.
            'context': declaration_at(line, offset),
        })

    for lineno, line in enumerate(lines, start=1):
        # QColor string-literal hex form
        for m in QCOLOR_HEX_STR.finditer(line):
            add(normalise_colour(m.group(1)), 'QColor("#...")', lineno, line, m.start())
        # QColor packed-int form
        for m in QCOLOR_HEX_INT.finditer(line):
            add(normalise_colour(m.group(1)), 'QColor(0x...)', lineno, line, m.start())
        # QColor RGB triple/quad
        for m in QCOLOR_RGB.finditer(line):
            r, g, b = int(m.group(1)), int(m.group(2)), int(m.group(3))
            if r > 255 or g > 255 or b > 255:
                continue
            add('#{:02x}{:02x}{:02x}'.format(r, g, b),
                'QColor(r,g,b)', lineno, line, m.start())
        # #xxxxxx inside any string literal on this line
        for str_m in STRING_LITERAL.finditer(line):
            body = str_m.group(1)
            for hex_m in HEX_INSIDE_STRING.finditer(body):
                if is_issue_reference(hex_m.group(0)):
                    continue   # "…restarting RX (#1361)" is not a colour
                # Offset within the LINE, not within the string body, so
                # declaration_at() splits on the right ';' boundaries.
                add(normalise_colour(hex_m.group(0)), 'inline string',
                    lineno, line, str_m.start(1) + hex_m.start())

    return records


def scan_tree(src_root: Path) -> tuple[list[dict], int, list[Path], int]:
    """Scan one source root.

    Returns (records, setStyleSheet count, allowlisted paths, files scanned).

    Factored out of main() so the ratchet can scan the PR's tree and its base
    with the SAME scanner in the SAME process — which is what makes comparing
    them safe, and what removes the need for a stored baseline.
    """
    paths = [p for p in src_root.rglob('*')
             if p.is_file() and p.suffix in {'.cpp', '.h', '.hpp', '.cc'}]
    allowlisted = [p for p in paths if is_allowlisted(p)]
    paths = [p for p in paths if not is_allowlisted(p)]

    records: list[dict] = []
    setStyleSheet_count = 0
    for path in paths:
        records.extend(scan_file(path))
        try:
            setStyleSheet_count += len(SET_STYLESHEET.findall(
                strip_comments(path.read_text(encoding='utf-8', errors='replace'))))
        except OSError:
            pass
    return records, setStyleSheet_count, allowlisted, len(paths)


def metrics(records: list[dict], setstylesheet: int) -> dict:
    return {
        'unique_colours':   len({r['colour'] for r in records}),
        'total_references': len(records),
        'setstylesheet':    setstylesheet,
    }


def main() -> int:
    p = argparse.ArgumentParser(description='Inventory hardcoded colours in src/')
    p.add_argument('--src', default='src', help='Source root to scan (default: src/)')
    p.add_argument('--out', default='/tmp/colour-audit.csv', help='CSV output path')
    p.add_argument('--summary-only', action='store_true',
                   help='Skip CSV; print summary to stdout only')
    p.add_argument('--compare-src', metavar='DIR',
                   help='Source root of the PR base to ratchet against; the '
                        'gate is the DELTA between --src and this tree')
    p.add_argument('--strict', action='store_true',
                   help='With --compare-src, exit 1 if any tracked count rose '
                        'above the base tree')
    args = p.parse_args()

    if args.strict and not args.compare_src:
        print('audit_colours: --strict requires --compare-src (the ratchet is a '
              'delta against the PR base, not a stored constant)', file=sys.stderr)
        return 2

    src_root = Path(args.src)
    if not src_root.is_dir():
        print(f'audit_colours: not a directory: {src_root}', file=sys.stderr)
        return 1

    all_records, setStyleSheet_count, allowlisted, files_scanned = scan_tree(src_root)

    # A scanner that stops finding anything must not read as a clean tree: a bad
    # --src, a moved source root, or a regex that stops matching all collapse
    # every metric to zero, and zero is <= any comparison. check_theme_seed.py
    # guards the same failure mode explicitly ("parsed 0 colour seeds — the
    # parser is broken, not the theme"), as does check_engine_boundary.py's
    # FLOOR TRIPPED.
    if not all_records:
        print(f'audit_colours: scanned 0 colour references under {src_root} — '
              'the scanner is broken, not the tree.', file=sys.stderr)
        return 1

    # Group by normalised colour for the spreadsheet.
    by_colour: dict[str, list[dict]] = defaultdict(list)
    for r in all_records:
        by_colour[r['colour']].append(r)

    # Suggested token per colour: use the colour itself + the most
    # common file-name token (best-effort context).
    suggestions = {}
    for colour, refs in by_colour.items():
        # Context = file stems AND each reference's own CSS declaration.
        #
        # Stems alone quietly disabled several classifier branches: the
        # waterfall/colormap test could fire only if a FILE was named for it,
        # and the palettes live in SpectrumWidget.cpp, so `color.waterfall.*`
        # resolved 0 references. The docstring already promised "nearby
        # identifiers"; scan_file was already recording the line. It simply was
        # never passed in.
        #
        # It is the DECLARATION, not the whole line — see declaration_at().
        # Passing the full line traded one dead branch for a dominant one:
        # border/outline went from 0 to 1150 of 2710 references because QSS
        # lines nearly always mention `border:` somewhere.
        ctx = ' '.join(Path(r['file']).stem for r in refs[:5])
        ctx += ' ' + ' '.join(r.get('context', '') for r in refs[:5])
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
    print(f'  source files scanned        : {files_scanned}')
    print(f'  unique normalised colours   : {len(by_colour)}')
    print(f'  total colour references     : {len(all_records)}')
    print(f'  setStyleSheet() call sites  : {setStyleSheet_count}')
    print()
    print('Top 20 most-used colours (good first migration targets):')
    for colour, count in top.most_common(20):
        print(f'  {colour}  {count:4d}  → {suggestions[colour]}')

    if allowlisted:
        print()
        print(f'Allow-listed (excluded by path, {len(allowlisted)} file(s)):')
        for path in allowlisted:
            print(f'  {path.as_posix()}')

    # ── Ratchet ────────────────────────────────────────────────────────────
    current = metrics(all_records, setStyleSheet_count)

    if not args.compare_src:
        return 0

    base_root = Path(args.compare_src)
    if not base_root.is_dir():
        print(f'audit_colours: --compare-src is not a directory: {base_root}',
              file=sys.stderr)
        return 1

    base_records, base_setstylesheet, _, _ = scan_tree(base_root)
    if not base_records:
        print(f'audit_colours: scanned 0 colour references under {base_root} — '
              'the base tree is wrong or the scanner is broken. Refusing to '
              'report every colour in the PR as newly added.', file=sys.stderr)
        return 1
    base = metrics(base_records, base_setstylesheet)

    print()
    print('=== ratchet (vs PR base) ===')
    regressions = []
    for key in TRACKED_METRICS:
        value, was = current[key], base[key]
        delta = value - was
        marker = 'OVER' if delta > 0 else 'OK  '
        print(f'  {marker} {key:18} {value:5d}  (base {was}, {delta:+d})')
        if delta > 0:
            regressions.append((key, value, was))

    # Informational: which colours this PR introduces that the base never had.
    # Safe to compute exactly because both trees went through the same scanner
    # in this process — a phantom shows up on both sides and cancels. This is
    # the stage-2 set signal, reported but not gated; see TRACKED_METRICS.
    new_colours = sorted({r['colour'] for r in all_records} -
                         {r['colour'] for r in base_records})
    if new_colours:
        print()
        print(f'Colours introduced by this PR ({len(new_colours)}):')
        for colour in new_colours[:20]:
            ref = by_colour[colour][0]
            print(f'  {colour}  {suggestions.get(colour, "color.???"):22} '
                  f'{ref["file"]}:{ref["line"]}')
        if len(new_colours) > 20:
            print(f'  … and {len(new_colours) - 20} more')

    if regressions:
        print()
        # WARN, not FAIL, when the run is advisory — a non-strict invocation
        # exits 0, and printing FAIL next to a green build is how a gate teaches
        # people to ignore it.
        label = 'FAIL' if args.strict else 'WARN'
        print(f'{label}: this PR raises the hardcoded-colour count above its base.')
        for key, value, was in regressions:
            print(f'  {key}: {value} > {was}  (+{value - was})')
        print()
        print('Fix by one of:')
        print('  * migrate the new colour to a token in '
              'docs/theming/canonical-tokens.md (the usual answer);')
        print('  * for a rise in setstylesheet, style via an existing themed '
              'widget or an existing setStyleSheet() site rather than a new '
              'call — the count tracks call sites, not colours, so migrating a '
              'colour will not clear it;')
        print('  * if the colours genuinely belong hardcoded (a palette, a '
              'colour picker\'s own swatches), add the FILE to '
              'COLOUR_ALLOWLIST with a reason.')
        if args.strict:
            return 1

    return 0


if __name__ == '__main__':
    raise SystemExit(main())
