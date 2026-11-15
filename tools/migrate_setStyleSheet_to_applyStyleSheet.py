#!/usr/bin/env python3
"""
Phase 5 PR 2 sweep — convert every

    WIDGET->setStyleSheet(THEMEREF.resolve("template"));
    setStyleSheet(THEMEREF.resolve("template"));        // called on `this`
    obj.setStyleSheet(THEMEREF.resolve("template"));    // stack-allocated

into

    THEMEREF.applyStyleSheet(WIDGET, "template");
    THEMEREF.applyStyleSheet(this,   "template");
    THEMEREF.applyStyleSheet(&obj,   "template");

The replacement is bit-identical at runtime (applyStyleSheet's first step is
`widget->setStyleSheet(resolve(template))`) but adds the (widget → tokens)
reverse-map entry that the Phase 5 inspector queries.

The receiver-finding walk handles arbitrary chained-access expressions:
  m_btn->setStyleSheet(...)
  this->m_btn->setStyleSheet(...)
  btns[i]->setStyleSheet(...)
  statusBar()->setStyleSheet(...)
  parent()->widget->setStyleSheet(...)
"""

import argparse
import re
import sys
from pathlib import Path

STYLE_RE = re.compile(r'\bsetStyleSheet\s*\(')


def find_matching_paren(src: str, open_idx: int) -> int:
    """Given index of '(' return index of matching ')'. -1 on imbalance."""
    depth = 0
    i = open_idx
    in_str = False
    str_ch = ''
    while i < len(src):
        c = src[i]
        if in_str:
            if c == '\\':
                i += 2
                continue
            if c == str_ch:
                in_str = False
        else:
            if c in ('"', "'"):
                in_str = True
                str_ch = c
            elif c == '(':
                depth += 1
            elif c == ')':
                depth -= 1
                if depth == 0:
                    return i
        i += 1
    return -1


def find_matching_paren_back(src: str, close_idx: int) -> int:
    """Walk backwards from a ')' or ']' to its matching open. -1 if none."""
    open_ch, close_ch = {'(': '(', ')': '(', '[': '[', ']': '['}.get(src[close_idx]), src[close_idx]
    if src[close_idx] == ')':
        opener = '('
    elif src[close_idx] == ']':
        opener = '['
    else:
        return -1
    depth = 1
    i = close_idx - 1
    while i >= 0:
        c = src[i]
        if c == src[close_idx]:
            depth += 1
        elif c == opener:
            depth -= 1
            if depth == 0:
                return i
        i -= 1
    return -1


# Characters that can be part of a chained receiver expression once we
# strip out balanced (...) / [...] groups.  Anything outside this set
# (e.g. ';', '{', ',', '=', '+', whitespace at the start of a line)
# terminates the receiver.
RECEIVER_TOKEN_CHARS = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.")


def find_receiver_start(src: str, end: int) -> int:
    """Scan backwards from `end` (exclusive) to find the start of the
    receiver expression for setStyleSheet.  Returns the start index, or
    `end` itself if the call has no receiver (implicit `this`).
    """
    i = end - 1
    # Skip optional `->` immediately before the call.  If we see one,
    # there IS a receiver.  If we see whitespace-only back to a statement
    # boundary, there's no receiver.
    while i >= 0 and src[i] in ' \t':
        i -= 1
    if i < 1:
        return end
    # Need either `->` or `.` (or `::` though that's a static-call thing
    # and doesn't apply to setStyleSheet which is non-static).
    if src[i] == '>' and src[i-1] == '-':
        i -= 2  # skip past '->'
    elif src[i] == '.':
        i -= 1  # skip past '.'
    else:
        return end  # no receiver — implicit `this`

    # Walk backwards consuming the receiver expression.  Track paren/
    # bracket depth so we don't stop in the middle of `arr[i]` or
    # `func(args)`.
    start = i + 1
    while i >= 0:
        c = src[i]
        if c == ')':
            jump = find_matching_paren_back(src, i)
            if jump < 0:
                break
            i = jump - 1
            start = i + 1
            continue
        if c == ']':
            jump = find_matching_paren_back(src, i)
            if jump < 0:
                break
            i = jump - 1
            start = i + 1
            continue
        # Member access chain — keep going.
        if c == '>' and i > 0 and src[i-1] == '-':
            i -= 2
            start = i + 1
            continue
        if c == '.':
            i -= 1
            start = i + 1
            continue
        if c == ':' and i > 0 and src[i-1] == ':':
            i -= 2
            start = i + 1
            continue
        if c in RECEIVER_TOKEN_CHARS:
            i -= 1
            start = i + 1
            continue
        break
    return start


def extract_resolve_call(arg_text: str):
    """Return (themeref, args_text) if arg_text is exactly `THEMEREF.resolve(...)`
    at the top level (with no trailing garbage other than whitespace).
    """
    s = arg_text.strip()
    depth = 0
    in_str = False
    str_ch = ''
    candidates = []
    i = 0
    while i < len(s):
        c = s[i]
        if in_str:
            if c == '\\':
                i += 2
                continue
            if c == str_ch:
                in_str = False
        else:
            if c in ('"', "'"):
                in_str = True
                str_ch = c
            elif c == '(':
                depth += 1
            elif c == ')':
                depth -= 1
            elif depth == 0 and c == '.':
                if s[i:i+9] == '.resolve(':
                    candidates.append(i)
        i += 1
    if not candidates:
        return None
    dot = candidates[0]
    themeref = s[:dot].strip()
    open_paren = dot + len('.resolve')
    close_paren = find_matching_paren(s, open_paren)
    if close_paren < 0:
        return None
    inner = s[open_paren + 1:close_paren]
    trailing = s[close_paren + 1:].strip()
    if trailing != '':
        return None
    if not themeref:
        return None
    return themeref, inner


def transform_file(path: Path, dry_run: bool = False, verbose: bool = False):
    text = path.read_text()
    out = []
    cursor = 0
    changes = 0
    skipped = 0

    while True:
        m = STYLE_RE.search(text, cursor)
        if not m:
            out.append(text[cursor:])
            break
        # Walk back to find receiver start.
        rec_start = find_receiver_start(text, m.start())
        out.append(text[cursor:rec_start])

        open_paren = m.end() - 1
        close_paren = find_matching_paren(text, open_paren)
        if close_paren < 0:
            out.append(text[rec_start:])
            break

        inner = text[open_paren + 1:close_paren]
        parsed = extract_resolve_call(inner)
        if not parsed:
            # Not a resolve-wrapping setStyleSheet — preserve verbatim.
            out.append(text[rec_start:close_paren + 1])
            cursor = close_paren + 1
            skipped += 1
            continue

        themeref, template_args = parsed
        receiver_text = text[rec_start:m.start()].rstrip()
        # Receiver_text ends with '->' or '.' (or is empty for `this`).
        if receiver_text.endswith('->'):
            widget_expr = receiver_text[:-2].rstrip()
        elif receiver_text.endswith('.'):
            widget_expr = '&' + receiver_text[:-1].rstrip()
        else:
            widget_expr = 'this'
        if not widget_expr or widget_expr == '&':
            # Bail — receiver looked weird, leave the source alone.
            out.append(text[rec_start:close_paren + 1])
            cursor = close_paren + 1
            skipped += 1
            continue

        replacement = f"{themeref}.applyStyleSheet({widget_expr}, {template_args})"
        out.append(replacement)
        cursor = close_paren + 1
        changes += 1
        if verbose:
            print(f"  {path}:{text.count(chr(10), 0, rec_start)+1}: {widget_expr}")

    new_text = ''.join(out)
    if new_text != text:
        if dry_run:
            print(f"{path}: {changes} migrated, {skipped} skipped (dry run)")
        else:
            path.write_text(new_text)
            print(f"{path}: {changes} migrated, {skipped} skipped")
    return changes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('paths', nargs='+', help='files or directories to scan')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('-v', '--verbose', action='store_true')
    args = ap.parse_args()

    targets = []
    for p in args.paths:
        pp = Path(p)
        if pp.is_dir():
            targets.extend(sorted(pp.rglob('*.cpp')))
            targets.extend(sorted(pp.rglob('*.h')))
        else:
            targets.append(pp)

    total = 0
    for t in targets:
        total += transform_file(t, dry_run=args.dry_run, verbose=args.verbose)
    print(f"Total: {total} sites" + (" (dry run)" if args.dry_run else ""))


if __name__ == '__main__':
    sys.exit(main())
