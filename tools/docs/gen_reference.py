#!/usr/bin/env python3
"""Generate the "Generated Reference" pages of the user docs from the source.

Usage:
    python3 tools/docs/gen_reference.py            # rewrite the pages
    python3 tools/docs/gen_reference.py --check    # CI: exit 1 and print a
                                                   # diff if a page is stale
    python3 tools/docs/gen_reference.py --sources  # list the C++ files read

Writes Markdown into docs/user/docs/generated/. Each page is a table that the
app already holds in code, so it is parsed from the C++ source text instead of
being copied by hand into a page that drifts:

  default-shortcuts.md       ShortcutManager registrations
                             (src/gui/MainWindow_Shortcuts.cpp)
  midi-actions.md            MIDI parameter registry
                             (MainWindow::registerMidiParams)
  flexcontrol-actions.md     FlexControl action table (FlexControlDialog.cpp)
  ulanzi-dial-actions.md     Ulanzi Dial wheel options and button defaults
  hid-controller-actions.md  Stream Deck+, HID encoder and TMate 2 tables
                             (RadioSetupDialog.cpp)
  log-categories.md          LogManager's curated category list
  tci-commands.md            Commands the TCI server dispatches on
  index.md                   The list of the pages above

No app, no Qt, no build: a static parse, in the manner of
tools/gen_bridge_docs.py. The parser is strict. Every registration site it
finds with a loose probe must also be understood by the strict parser, and a
key, constant or loop it does not recognise is an error, never a silently
missing row. When this script fails after a source change, teach it the new
shape; do not loosen the check.

Not generated, on purpose:
  * The menu bar tree. MainWindow_Menus.cpp builds whole submenus in loops
    over runtime data (themes, profiles, band plans, UI scales), moves actions
    between menus, and varies by platform and build feature, so a text parse
    would be approximate.
  * CAT (SmartCAT / rigctld) commands. The dispatchers answer many recognised
    commands with fixed values, no-op acknowledgements or "not available"
    codes written inline, so a list of names would overstate support.
  * Settings keys. There is no central registry; keys are declared where
    they are used.

Stdlib only; Python 3.9+.
"""

from __future__ import annotations

import argparse
import difflib
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
OUT_DIR = REPO / "docs" / "user" / "docs" / "generated"
GENERATOR = "tools/docs/gen_reference.py"

SHORTCUTS_CPP = "src/gui/MainWindow_Shortcuts.cpp"
SHORTCUT_MANAGER_CPP = "src/core/ShortcutManager.cpp"
HELPERS_H = "src/gui/MainWindowHelpers.h"
DIGITAL_VOICE_H = "src/core/DigitalVoiceFeature.h"
DV_REGISTRY_CPP = "src/core/DigitalVoiceModeRegistry.cpp"
CONTROLLERS_CPP = "src/gui/MainWindow_Controllers.cpp"
MIDI_H = "src/core/MidiControlManager.h"
FLEXCONTROL_CPP = "src/gui/FlexControlDialog.cpp"
ULANZI_DIALOG_CPP = "src/gui/UlanziDialMapperDialog.cpp"
ULANZI_MAPPINGS_CPP = "src/core/UlanziDialMappings.cpp"
RADIO_SETUP_CPP = "src/gui/RadioSetupDialog.cpp"
LOG_MANAGER_CPP = "src/core/LogManager.cpp"
TCI_PROTOCOL_CPP = "src/core/TciProtocol.cpp"
TCI_SERVER_CPP = "src/core/TciServer.cpp"

ALL_SOURCES = [
    SHORTCUTS_CPP, SHORTCUT_MANAGER_CPP, HELPERS_H, DIGITAL_VOICE_H,
    DV_REGISTRY_CPP,
    CONTROLLERS_CPP, MIDI_H, FLEXCONTROL_CPP, ULANZI_DIALOG_CPP,
    ULANZI_MAPPINGS_CPP, RADIO_SETUP_CPP, LOG_MANAGER_CPP, TCI_PROTOCOL_CPP,
    TCI_SERVER_CPP,
]


class ParseError(Exception):
    pass


# ---------------------------------------------------------------------------
# C++ text utilities


class Source:
    """A C++ file with a comment-blanked copy that keeps every offset."""

    def __init__(self, rel: str):
        self.rel = rel
        path = REPO / rel
        if not path.exists():
            raise ParseError(f"{rel}: file not found")
        self.raw = path.read_text(encoding="utf-8")
        self.code = blank_comments(self.raw)
        self._cond = None

    def line_of(self, pos: int) -> int:
        return self.raw.count("\n", 0, pos) + 1

    def where(self, pos: int) -> str:
        return f"{self.rel}:{self.line_of(pos)}"

    def conditions(self, pos: int) -> tuple:
        """Preprocessor conditions in force at ``pos``, outermost first."""
        if self._cond is None:
            self._cond = []
            stack = []
            offset = 0
            for line in self.code.splitlines(keepends=True):
                s = line.strip()
                m = re.match(r"#\s*(ifdef|ifndef|if|elif|else|endif)\b\s*(.*)", s)
                if m:
                    kind, expr = m.group(1), m.group(2).strip()
                    if kind == "ifdef":
                        stack.append(expr)
                    elif kind == "ifndef":
                        stack.append(f"!{expr}")
                    elif kind == "if":
                        stack.append(expr)
                    elif kind in ("elif", "else"):
                        if not stack:
                            raise ParseError(f"{self.rel}: #{kind} without #if")
                        prev = stack.pop()
                        stack.append(expr if kind == "elif" else f"not ({prev})")
                    elif kind == "endif":
                        if not stack:
                            raise ParseError(f"{self.rel}: #endif without #if")
                        stack.pop()
                self._cond.append((offset, tuple(stack)))
                offset += len(line)
        result = ()
        for start, conds in self._cond:
            if start > pos:
                break
            result = conds
        return result


def blank_comments(text: str) -> str:
    """Replace // and /* */ comments with spaces, keeping newlines and
    offsets. String, character and raw string literals are left alone."""
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j == -1 else j
            for k in range(i, j):
                out[k] = " "
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j == -1 else j + 2
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        elif c == "R" and text.startswith('R"', i) and (
            i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_")
        ):
            m = re.match(r'R"([^()\\\s]{0,16})\(', text[i:])
            if not m:
                i += 1
                continue
            end = text.find(")" + m.group(1) + '"', i + m.end())
            if end == -1:
                raise ParseError("unterminated raw string literal")
            i = end + len(m.group(1)) + 2
        elif c in "\"'":
            i = skip_literal(text, i)
        else:
            i += 1
    return "".join(out)


def skip_literal(text: str, i: int) -> int:
    quote = text[i]
    j = i + 1
    n = len(text)
    while j < n:
        if text[j] == "\\":
            j += 2
            continue
        if text[j] == quote:
            return j + 1
        if text[j] == "\n":
            # A ' in a number separator (1'000) or a stray quote: stop here.
            return i + 1
        j += 1
    return n


def match_close(code: str, open_pos: int) -> int:
    """Index of the bracket closing the one at ``open_pos``."""
    pairs = {"(": ")", "{": "}", "[": "]"}
    stack = [pairs[code[open_pos]]]
    i = open_pos + 1
    n = len(code)
    while i < n:
        c = code[i]
        if c in "\"'":
            i = skip_literal(code, i)
            continue
        if c == "R" and code.startswith('R"', i) and not (
            code[i - 1].isalnum() or code[i - 1] == "_"
        ):
            m = re.match(r'R"([^()\\\s]{0,16})\(', code[i:])
            if m:
                end = code.find(")" + m.group(1) + '"', i + m.end())
                i = end + len(m.group(1)) + 2
                continue
        if c in pairs:
            stack.append(pairs[c])
        elif c in ")}]":
            if c != stack[-1]:
                raise ParseError(f"unbalanced {c!r} at offset {i}")
            stack.pop()
            if not stack:
                return i
        i += 1
    raise ParseError(f"no closing bracket for offset {open_pos}")


def split_top(s: str) -> list:
    """Split on commas that are not nested in brackets or literals."""
    parts, depth, start, i = [], 0, 0, 0
    while i < len(s):
        c = s[i]
        if c in "\"'":
            i = skip_literal(s, i)
            continue
        if c in "({[":
            depth += 1
        elif c in ")}]":
            depth -= 1
        elif c == "," and depth == 0:
            parts.append(s[start:i].strip())
            start = i + 1
        i += 1
    tail = s[start:].strip()
    if tail or parts:
        parts.append(tail)
    return parts


def calls(src: Source, name_re: str, start: int = 0, end: int = None):
    """Yield (pos, [args]) for each call matching ``name_re`` + ``(``."""
    end = len(src.code) if end is None else end
    for m in re.finditer(name_re + r"\s*\(", src.code[start:end]):
        open_pos = start + m.end() - 1
        close = match_close(src.code, open_pos)
        yield start + m.start(), split_top(src.code[open_pos + 1:close])


C_ESCAPES = {'"': '"', "'": "'", "\\": "\\", "n": " ", "t": " ", "?": "?"}


def unescape(body: str, where: str) -> str:
    out, i = [], 0
    while i < len(body):
        c = body[i]
        if c == "\\":
            nxt = body[i + 1:i + 2]
            if nxt in C_ESCAPES:
                out.append(C_ESCAPES[nxt])
                i += 2
                continue
            raise ParseError(f"{where}: unsupported escape \\{nxt} in a string")
        out.append(c)
        i += 1
    return "".join(out)


STRING_WRAPPERS = ("QStringLiteral", "QLatin1String", "QString", "tr", "QObject::tr")


def literal(expr: str, where: str):
    """The value of a string literal expression (adjacent literals joined,
    an optional QStringLiteral()/tr()/... wrapper), or None if ``expr`` is not
    one."""
    e = expr.strip()
    m = re.fullmatch(r"(?:%s)\s*\((.*)\)" % "|".join(
        re.escape(w) for w in STRING_WRAPPERS), e, re.S)
    if m:
        e = m.group(1).strip()
    if not e.startswith('"'):
        return None
    pieces = re.findall(r'"((?:[^"\\\n]|\\.)*)"', e)
    rebuilt = re.sub(r'"((?:[^"\\\n]|\\.)*)"', "", e).strip()
    if rebuilt:
        return None
    return unescape("".join(pieces), where)


def need_literal(expr: str, where: str) -> str:
    v = literal(expr, where)
    if v is None:
        raise ParseError(f"{where}: expected a string literal, found {expr!r}")
    return v


def array_initializer(src: Source, name: str, start: int = 0, end: int = None,
                      nearest: bool = False):
    """Elements of ``name[...] = { ... };`` as a nested list of raw strings.

    Exactly one definition must exist in the range searched, unless
    ``nearest`` is set: then the last one before ``end`` is used (for a loop
    that reads an array declared just above it)."""
    end = len(src.code) if end is None else end
    pat = re.compile(r"\b%s\s*(?:\[[^\]]*\]\s*)+=\s*\{" % re.escape(name))
    found = list(pat.finditer(src.code, start, end))
    if nearest and found:
        found = found[-1:]
    if len(found) != 1:
        raise ParseError(
            f"{src.rel}: expected one definition of {name}[], found {len(found)}")
    m = found[0]
    open_pos = m.end() - 1
    close = match_close(src.code, open_pos)

    def parse(s):
        items = []
        for part in split_top(s):
            if not part:
                continue
            if part.startswith("{"):
                inner_close = match_close(part, 0)
                items.append(parse(part[1:inner_close]))
            else:
                items.append(part)
        return items

    return parse(src.code[open_pos + 1:close]), m.start()


def string_rows(src: Source, name: str, start: int = 0, end: int = None,
                nearest: bool = False):
    rows, pos = array_initializer(src, name, start, end, nearest)
    where = src.where(pos)
    out = []
    for row in rows:
        if isinstance(row, list):
            # String fields become their value; numbers and the like stay as
            # source text.
            vals = [literal(x, where) for x in row]
            out.append([v if v is not None else x.strip()
                        for v, x in zip(vals, row)])
        else:
            out.append(need_literal(row, where))
    return out


def function_body(src: Source, signature_re: str):
    """(start, end) offsets of the body of the one function matching."""
    found = list(re.finditer(signature_re + r"\s*\([^;{]*?\)\s*(?:const\s*)?\{",
                             src.code))
    if len(found) != 1:
        raise ParseError(
            f"{src.rel}: expected one definition matching {signature_re!r}, "
            f"found {len(found)}")
    open_pos = found[0].end() - 1
    return open_pos, match_close(src.code, open_pos)


def string_constants(src: Source) -> dict:
    consts = {}
    for m in re.finditer(
        r"constexpr\s+const\s+char\s*\*\s*(k\w+)\s*=\s*(\"(?:[^\"\\]|\\.)*\")\s*;",
        src.code,
    ):
        consts[m.group(1)] = need_literal(m.group(2), src.where(m.start()))
    return consts


# ---------------------------------------------------------------------------
# Markdown helpers


def esc(text: str) -> str:
    """Escape prose for a CommonMark table cell."""
    text = str(text)
    text = text.replace("\\", "\\\\")
    for ch in "*_`[]":
        text = text.replace(ch, "\\" + ch)
    text = text.replace("<", "&lt;").replace(">", "&gt;")
    return text.replace("|", "\\|")


def code(text: str) -> str:
    text = str(text)
    if not text:
        return ""
    fence = "``" if "`" in text else "`"
    pad = " " if fence == "``" else ""
    return f"{fence}{pad}{text}{pad}{fence}".replace("|", "\\|")


def table(headers: list, rows: list) -> list:
    out = ["| " + " | ".join(headers) + " |",
           "|" + "|".join("---" for _ in headers) + "|"]
    for r in rows:
        out.append("| " + " | ".join(c if c else " " for c in r) + " |")
    return out


def yaml_str(value: str) -> str:
    # A JSON string is a valid YAML double-quoted scalar.
    return json.dumps(value, ensure_ascii=False)


def page(title: str, description: str, sources: list, intro: list, body: list,
         position: int) -> str:
    src_list = ", ".join(f"`{s}`" for s in sources)
    fm = [
        "---",
        f"title: {yaml_str(title)}",
        f"description: {yaml_str(description)}",
        f"sidebar_position: {position}",
        "custom_edit_url: null",
        f"generated_by: {yaml_str(GENERATOR)}",
        "generated_from: [" + ", ".join(yaml_str(s) for s in sources) + "]",
        "---",
        "",
        ":::info[Generated page]",
        "",
        f"This page is generated from {src_list} by `{GENERATOR}`. "
        "Do not edit it by hand: change the source, then run "
        f"`python3 {GENERATOR}`.",
        "",
        ":::",
        "",
    ]
    lines = fm + intro + ([""] if intro else []) + body
    while lines and not lines[-1].strip():
        lines.pop()
    return "\n".join(lines) + "\n"


def join_conditions(conds) -> str:
    return " and ".join(f"`{c}`" for c in conds)


# ---------------------------------------------------------------------------
# Keyboard shortcuts

QT_KEYS = {
    "Right": "Right", "Left": "Left", "Up": "Up", "Down": "Down",
    "Space": "Space", "BracketLeft": "[", "BracketRight": "]",
    "Equal": "=", "Minus": "-", "Slash": "/", "Plus": "+", "Comma": ",",
    "Period": ".", "Semicolon": ";", "Apostrophe": "'", "Backslash": "\\",
    "Escape": "Esc", "Tab": "Tab", "Return": "Return", "Enter": "Enter",
    "Backspace": "Backspace", "Delete": "Del", "Insert": "Ins",
    "Home": "Home", "End": "End", "PageUp": "PgUp", "PageDown": "PgDown",
}
QT_MODIFIERS = [("CTRL", "Ctrl"), ("ALT", "Alt"), ("SHIFT", "Shift"),
                ("META", "Meta")]


def qt_key_name(token: str, where: str) -> str:
    m = re.fullmatch(r"Qt::Key_(\w+)", token)
    if not m:
        raise ParseError(f"{where}: unrecognised key token {token!r}")
    k = m.group(1)
    if re.fullmatch(r"[A-Z0-9]", k) or re.fullmatch(r"F\d{1,2}", k):
        return k
    if k in QT_KEYS:
        return QT_KEYS[k]
    raise ParseError(
        f"{where}: key Qt::Key_{k} has no display name; add it to QT_KEYS in "
        f"{GENERATOR}")


def key_sequence(expr: str, where: str) -> str:
    """Display text for a QKeySequence constructor; '' for no key."""
    e = expr.strip()
    if e in ("{}", "QKeySequence()", "QKeySequence{}"):
        return ""
    m = re.fullmatch(r"QKeySequence\s*\((.*)\)", e, re.S)
    if not m:
        raise ParseError(f"{where}: unrecognised key sequence {expr!r}")
    inner = m.group(1).strip()
    if not inner:
        return ""
    s = literal(inner, where)
    if s is not None:
        return s
    tokens = [t.strip() for t in inner.split("|")]
    mods, key = [], None
    for t in tokens:
        mm = re.fullmatch(r"Qt::(CTRL|ALT|SHIFT|META)(?:Modifier)?", t)
        if mm:
            mods.append(mm.group(1))
            continue
        if key is not None:
            raise ParseError(f"{where}: two keys in {expr!r}")
        key = qt_key_name(t, where)
    if key is None:
        raise ParseError(f"{where}: no key in {expr!r}")
    names = [disp for tok, disp in QT_MODIFIERS if tok in mods]
    return "+".join(names + [key])


def platform_keys(src: Source, name: str, where: str) -> dict:
    """{'macOS': ..., 'other': ...} for a QKeySequence variable declared in
    an ``#ifdef Q_OS_MACOS`` / ``#else`` block."""
    found = {}
    for m in re.finditer(
        r"const\s+QKeySequence\s+%s\s*(\((?:[^;]*)\))?\s*;" % re.escape(name),
        src.code,
    ):
        conds = src.conditions(m.start())
        if not conds:
            raise ParseError(f"{where}: {name} is not inside a platform #ifdef")
        last = conds[-1]
        if last == "Q_OS_MACOS":
            plat = "macOS"
        elif last == "not (Q_OS_MACOS)":
            plat = "other"
        else:
            raise ParseError(f"{where}: {name} is under unexpected #if {last!r}")
        arg = m.group(1)
        found[plat] = key_sequence(f"QKeySequence{arg or '()'}", where)
    if set(found) != {"macOS", "other"}:
        raise ParseError(f"{where}: {name} must be declared for macOS and for "
                         "other platforms")
    return found


def digital_voice_gate(dv: Source):
    """(modes, conditions): the radio modes modeActionModes() drops unless the
    local digital-voice helper is built in, and the #if that builds it."""
    m = re.search(r"kLocalDigitalVoiceWaveformAvailable\s*=\s*true", dv.code)
    if not m:
        raise ParseError(f"{dv.rel}: kLocalDigitalVoiceWaveformAvailable = true "
                         "not found")
    conds = dv.conditions(m.start())
    if not conds:
        raise ParseError(f"{dv.rel}: the digital-voice helper is no longer "
                         "behind a build switch")
    reg = Source(DV_REGISTRY_CPP)
    rows, pos = array_initializer_braced(reg, "kSupportedModes")
    modes = []
    for row in rows:
        if not isinstance(row, list) or len(row) < 4:
            raise ParseError(f"{reg.where(pos)}: a kSupportedModes entry changed "
                             "shape (radioMode is the 4th field)")
        modes.append(need_literal(row[3], reg.where(pos)))
    return set(modes), conds


def array_initializer_braced(src: Source, name: str):
    """Elements of ``Type name { ... };`` (brace initialisation)."""
    found = list(re.finditer(r"\b%s\s*\{" % re.escape(name), src.code))
    if len(found) != 1:
        raise ParseError(f"{src.rel}: expected one {name} {{...}}, found "
                         f"{len(found)}")
    open_pos = found[0].end() - 1
    close = match_close(src.code, open_pos)
    rows = []
    for part in split_top(src.code[open_pos + 1:close]):
        if part.startswith("{"):
            rows.append(split_top(part[1:match_close(part, 0)]))
        elif part:
            rows.append(part)
    return rows, found[0].start()


def parse_shortcuts():
    src = Source(SHORTCUTS_CPP)
    helpers = Source(HELPERS_H)
    consts = string_constants(helpers)
    dv = Source(DIGITAL_VOICE_H)
    mgr = Source(SHORTCUT_MANAGER_CPP)

    # Category order, as the keyboard map shows it.
    b0, b1 = function_body(mgr, r"QStringList\s+ShortcutManager::categories")
    m = re.search(r"return\s*\{(.*?)\}\s*;", mgr.code[b0:b1], re.S)
    if not m:
        raise ParseError(f"{mgr.rel}: cannot read ShortcutManager::categories()")
    categories = [need_literal(x, mgr.rel) for x in split_top(m.group(1))]

    # The mode list: modeActionModes() in DigitalVoiceFeature.h.
    d0, d1 = function_body(dv, r"inline\s+QStringList\s+modeActionModes")
    m = re.search(r"\{\s*(\"[^}]*)\}", dv.code[d0 + 1:d1])
    if not m:
        raise ParseError(f"{dv.rel}: cannot read the modeActionModes() list")
    modes = [need_literal(x, dv.rel) for x in split_top(m.group(1))]
    gated, gate_conds = digital_voice_gate(dv)

    bands = string_rows(src, "kShortcutBandNames")

    def resolve(expr, where, what):
        v = literal(expr, where)
        if v is not None:
            return v
        if re.fullmatch(r"k\w+", expr) and expr in consts:
            return consts[expr]
        raise ParseError(f"{where}: cannot resolve the {what} {expr!r}")

    def flag(args, idx, where):
        if len(args) <= idx:
            return False
        v = args[idx].strip()
        if v in ("true", "false"):
            return v == "true"
        raise ParseError(f"{where}: expected true/false, found {v!r}")

    # registerTxShortcut is a local lambda that forwards to registerAction
    # with category "TX" and keysTx=true. Its forwarding call is not an
    # action of its own; check that it still says what this parser assumes.
    helper = list(re.finditer(r"\bregisterTxShortcut\s*=\s*\[[^\]]*\]\s*\(",
                              src.code))
    if len(helper) != 1:
        raise ParseError(f"{src.rel}: expected one registerTxShortcut lambda, "
                         f"found {len(helper)}")
    h_params_close = match_close(src.code, helper[0].end() - 1)
    h_open = src.code.index("{", h_params_close)
    h_span = (h_open, match_close(src.code, h_open))
    forwarded = [(p, a) for p, a in calls(src, r"\.registerAction", *h_span)]
    if len(forwarded) != 1 or need_literal(forwarded[0][1][2], src.rel) != "TX" \
            or [x.strip() for x in forwarded[0][1][5:7]] != ["false", "true"]:
        raise ParseError(f"{src.where(h_open)}: registerTxShortcut no longer "
                         "forwards as category TX with keysTx=true")
    helper_pos = forwarded[0][0]

    entries = []  # (pos, dict)
    for pos, args in calls(src, r"\.registerAction"):
        if pos == helper_pos:
            continue
        where = src.where(pos)
        if len(args) < 5:
            raise ParseError(f"{where}: registerAction with {len(args)} args")
        id_expr, name_expr, cat_expr, key_expr = args[:4]
        category = need_literal(cat_expr, where)
        policy = args[7].strip() if len(args) > 7 else ""
        if policy and not re.fullmatch(
                r"ShortcutManager::ShortcutPolicy::(Operating|WindowManagement)",
                policy):
            raise ParseError(f"{where}: unrecognised policy {policy!r}")
        common = {
            "category": category,
            "repeat": flag(args, 5, where),
            "tx": flag(args, 6, where),
            "window": policy.endswith("WindowManagement"),
            "conds": src.conditions(pos),
        }
        # The band loop: registerAction(id, bandName, "Band", ...).
        if id_expr == "id" and category == "Band":
            if key_expr.strip() not in ("QKeySequence()", "{}"):
                raise ParseError(f"{where}: band shortcuts gained a default key")
            for b in bands:
                entries.append((pos, dict(common, id=f"band_{b}", name=b, key="")))
            continue
        # The mode loop: registerAction(QString("mode_%1").arg(m.toLower()), m, ...)
        mm = re.fullmatch(r'QString\s*\(\s*"mode_%1"\s*\)\s*\.arg\(\s*m\.toLower\(\)\s*\)',
                          id_expr)
        if mm:
            if name_expr.strip() != "m" or key_expr.strip() not in ("QKeySequence()", "{}"):
                raise ParseError(f"{where}: the mode shortcut loop changed shape")
            for mode in modes:
                conds = common["conds"] + (gate_conds if mode in gated else ())
                entries.append((pos, dict(common, id=f"mode_{mode.lower()}",
                                          name=mode, key="", mode=True,
                                          conds=conds)))
            continue
        ident = resolve(id_expr, where, "action id")
        name = resolve(name_expr, where, "display name")
        if re.fullmatch(r"[A-Za-z_]\w*", key_expr.strip()) and not key_expr.strip().startswith("Qt"):
            keys = platform_keys(src, key_expr.strip(), where)
            entries.append((pos, dict(common, id=ident, name=name, key=keys)))
        else:
            entries.append((pos, dict(common, id=ident, name=name,
                                      key=key_sequence(key_expr, where))))

    for pos, args in calls(src, r"(?<![\w.])registerTxShortcut"):
        where = src.where(pos)
        if len(args) != 5:
            raise ParseError(f"{where}: registerTxShortcut with {len(args)} args")
        entries.append((pos, {
            "id": resolve(args[0], where, "action id"),
            "name": resolve(args[1], where, "display name"),
            "category": "TX",
            "key": key_sequence(args[2], where),
            "repeat": False, "tx": True, "window": False,
            "conds": src.conditions(pos),
        }))

    # Fail loudly on a registration the strict parser did not see.
    loose = len(re.findall(r"\bregisterAction\s*\(", src.code)) + \
        len(re.findall(r"(?<![\w.])registerTxShortcut\s*\(", src.code))
    seen = len({pos for pos, _ in entries}) + 1  # + the helper's forwarding call
    if loose != seen:
        raise ParseError(f"{src.rel}: {loose} registration sites, parsed {seen}")

    entries.sort(key=lambda e: e[0])
    actions = [e for _, e in entries]
    ids = [a["id"] for a in actions]
    dups = sorted({i for i in ids if ids.count(i) > 1})
    if dups:
        raise ParseError(f"{src.rel}: duplicate shortcut ids {dups}")
    unknown = sorted({a["category"] for a in actions} - set(categories))
    if unknown:
        raise ParseError(f"{src.rel}: categories {unknown} are not in "
                         "ShortcutManager::categories()")
    return actions, categories, [src.rel, mgr.rel, helpers.rel, dv.rel,
                                 DV_REGISTRY_CPP]


def render_shortcuts(actions, categories, sources) -> str:
    intro = [
        "These are the actions you can bind in **Settings → Configure "
        "Shortcuts...**, with the key each one has before you change it. An "
        "action with no default key does nothing from the keyboard until you "
        "bind one.",
        "",
        "- The **ID** is the stable name used in a shortcuts backup file and "
        "by the automation bridge's `shortcut` verb.",
        "- On macOS, Qt reads **Ctrl** as the Command (⌘) key and **Meta** as "
        "the Control key.",
        "- Menu accelerators and fixed application shortcuts are not in this "
        "list; they are not assignable.",
    ]
    body = []
    for cat in categories:
        rows = []
        for a in actions:
            if a["category"] != cat:
                continue
            k = a["key"]
            if isinstance(k, dict):
                key = "; ".join(
                    f"{plat}: {code(k[plat]) if k[plat] else 'none'}"
                    for plat in ("macOS", "other"))
            else:
                key = code(k)
            notes = []
            if a["tx"]:
                notes.append("Keys the transmitter")
            if a["repeat"]:
                notes.append("Repeats while held")
            if a["window"]:
                notes.append("Window management: works with keyboard "
                             "shortcuts switched off")
            if a["conds"]:
                notes.append("Only in builds with " + join_conditions(a["conds"]))
            # Notes are fixed prose plus code spans; nothing here needs escaping.
            rows.append([esc(a["name"]), code(a["id"]), key, "; ".join(notes)])
        if not rows:
            continue
        body += [f"## {cat}", ""] + table(
            ["Action", "ID", "Default key", "Notes"], rows) + [""]
    return page(
        "Default Keyboard Shortcuts",
        "Every assignable keyboard action in AetherSDR, with its ID and "
        "default key, generated from the source.",
        sources, intro, body, 2,
    )


# ---------------------------------------------------------------------------
# MIDI parameters

MIDI_TYPE_TEXT = {
    "Slider": "Slider",
    "Toggle": "Toggle",
    "Trigger": "Trigger",
    "Gate": "Gate (held)",
}


def parse_number(expr: str, where: str) -> str:
    e = expr.strip()
    if not re.fullmatch(r"-?\d+(?:\.\d+)?f?", e):
        raise ParseError(f"{where}: expected a number, found {expr!r}")
    return e.rstrip("f")


def parse_midi():
    src = Source(CONTROLLERS_CPP)
    helpers = Source(HELPERS_H)
    consts = string_constants(helpers)
    hdr = Source(MIDI_H)

    m = re.search(r"enum\s+class\s+MidiParamType\s*\{(.*?)\}", hdr.code, re.S)
    if not m:
        raise ParseError(f"{hdr.rel}: MidiParamType not found")
    known_types = [t.strip() for t in m.group(1).split(",") if t.strip()]
    if set(known_types) != set(MIDI_TYPE_TEXT):
        raise ParseError(f"{hdr.rel}: MidiParamType is {known_types}; update "
                         f"MIDI_TYPE_TEXT in {GENERATOR}")

    b0, b1 = function_body(src, r"void\s+MainWindow::registerMidiParams")
    body = src.code[b0:b1]
    base_conds = src.conditions(b0)
    MIDI_BASE_CONDS["conds"] = base_conds

    def fmt(var):
        mm = re.search(r"\b%s\s*=\s*QString\s*\(\s*(\"[^\"]*\")\s*\)\s*\.arg\(" % var, body)
        if not mm:
            raise ParseError(f"{src.rel}: cannot find the format of {var}")
        return need_literal(mm.group(1), src.rel)

    params = []
    loop_sites = 0
    for pos, args in calls(src, r"(?<![\w.:>])reg", b0, b1):
        where = src.where(pos)
        if len(args) < 6:
            raise ParseError(f"{where}: reg() with {len(args)} args")
        id_expr, name_expr, cat_expr, type_expr, lo, hi = args[:6]
        tm = re.fullmatch(r"P::(\w+)", type_expr.strip())
        if not tm or tm.group(1) not in MIDI_TYPE_TEXT:
            raise ParseError(f"{where}: unrecognised param type {type_expr!r}")
        conds = tuple(c for c in src.conditions(pos)[len(base_conds):])
        common = {"category": need_literal(cat_expr, where), "type": tm.group(1),
                  "lo": parse_number(lo, where), "hi": parse_number(hi, where),
                  "conds": conds}
        e = id_expr.strip()
        if e == "idMidi.toUtf8().constData()":
            # The mode loop over modeActionModes().
            if name_expr.strip() != "name.toUtf8().constData()":
                raise ParseError(f"{where}: the MIDI mode loop changed shape")
            id_fmt, name_fmt = fmt("idMidi"), fmt("name")
            for mode, mode_conds in MODES_CACHE["modes"]:
                params.append((pos, dict(common, id=id_fmt.replace("%1", mode),
                                         name=name_fmt.replace("%1", mode),
                                         conds=common["conds"] + mode_conds)))
            loop_sites += 1
        elif e == "b.idMidi":
            if name_expr.strip() != "b.label":
                raise ParseError(f"{where}: the MIDI band loop changed shape")
            for row in string_rows(src, "kMidiBands", b0, b1):
                params.append((pos, dict(common, id=row[0], name=row[2])))
            loop_sites += 1
        elif e == "id.toUtf8().constData()":
            # The EQ band loop: id from freqs[], name from names[].
            if name_expr.strip() != "names[i]":
                raise ParseError(f"{where}: the MIDI EQ loop changed shape")
            id_fmt = fmt("id")
            freqs, _ = array_initializer(src, "freqs", b0, pos, nearest=True)
            names = string_rows(src, "names", b0, pos, nearest=True)
            if len(freqs) != len(names):
                raise ParseError(f"{where}: EQ freqs[] and names[] differ in length")
            for f, n in zip(freqs, names):
                params.append((pos, dict(common, id=id_fmt.replace("%1", f.strip()),
                                         name=n)))
            loop_sites += 1
        else:
            ident = literal(e, where)
            if ident is None:
                if e in consts:
                    ident = consts[e]
                else:
                    raise ParseError(f"{where}: cannot resolve MIDI id {e!r}")
            name = literal(name_expr, where)
            if name is None:
                name = consts.get(name_expr.strip())
            if name is None:
                raise ParseError(f"{where}: cannot resolve MIDI name {name_expr!r}")
            params.append((pos, dict(common, id=ident, name=name)))

    loose = len(re.findall(r"(?<![\w.:>])reg\s*\(", body))
    seen = len({p for p, _ in params})
    if loose != seen:
        raise ParseError(f"{src.rel}: {loose} reg() sites, parsed {seen}")
    if loop_sites != 3:
        raise ParseError(f"{src.rel}: expected 3 loop registrations, found {loop_sites}")
    out = [p for _, p in params]
    ids = [p["id"] for p in out]
    dups = sorted({i for i in ids if ids.count(i) > 1})
    if dups:
        raise ParseError(f"{src.rel}: duplicate MIDI ids {dups}")
    return out, [src.rel, helpers.rel, DIGITAL_VOICE_H, DV_REGISTRY_CPP, hdr.rel]


MODES_CACHE = {}
MIDI_BASE_CONDS = {}


def render_midi(params, sources) -> str:
    intro = [
        "These are the controls you can map to a MIDI controller in "
        "**Settings → MIDI Mapping...**. The same list feeds the Ulanzi Dial "
        "button menus.",
        "",
        "- **Slider**: a continuous control (knob, fader, pitch bend), scaled "
        "to the range shown.",
        "- **Toggle**: on/off. A CC above 63 is on; a note toggles.",
        "- **Trigger**: fires once on a note or CC.",
        "- **Gate (held)**: on while the note is held, off when it is "
        "released.",
    ]
    if MIDI_BASE_CONDS["conds"]:
        intro += ["", "MIDI control exists only in builds with "
                  + join_conditions(MIDI_BASE_CONDS["conds"]) + "."]
    cats = []
    for p in params:
        if p["category"] not in cats:
            cats.append(p["category"])
    body = []
    for cat in cats:
        rows = []
        for p in params:
            if p["category"] != cat:
                continue
            rng = f"{p['lo']} to {p['hi']}" if p["type"] == "Slider" else ""
            note = ("Only in builds with " + join_conditions(p["conds"])) if p["conds"] else ""
            rows.append([esc(p["name"]), code(p["id"]), MIDI_TYPE_TEXT[p["type"]],
                         rng, note])
        body += [f"## {esc(cat)}", ""] + table(
            ["Control", "ID", "Type", "Range", "Notes"], rows) + [""]
    return page(
        "MIDI Controller Actions",
        "Every control AetherSDR can map to a MIDI controller, with its ID, "
        "type and range, generated from the source.",
        sources, intro, body, 3,
    )


# ---------------------------------------------------------------------------
# FlexControl


def parse_flexcontrol():
    src = Source(FLEXCONTROL_CPP)
    actions = string_rows(src, "kFlexActions")
    defaults = string_rows(src, "defaults")
    ids = {a[0] for a in actions}
    for row in defaults:
        for d in row:
            if d not in ids:
                raise ParseError(f"{src.rel}: default {d!r} is not in kFlexActions")
    return actions, defaults, [src.rel]


def render_flexcontrol(actions, defaults, sources) -> str:
    labels = {a[0]: a[1] for a in actions}
    intro = [
        "The FlexControl knob's three Aux buttons can each run one action on "
        "a single tap and another on a double tap. Set them in **Settings → "
        "FlexControl Knob & Buttons...**.",
    ]
    rows = []
    for i, row in enumerate(defaults):
        if len(row) != 2:
            raise ParseError(f"{FLEXCONTROL_CPP}: aux default row {i} is not a pair")
        rows.append([f"Aux{i + 1}", esc(labels[row[0]]), esc(labels[row[1]])])
    body = ["## Default button actions", ""] + table(
        ["Button", "Single tap", "Double tap"], rows) + [""]
    body += ["## Actions", ""] + table(
        ["Action", "ID"], [[esc(a[1]), code(a[0])] for a in actions]) + [""]
    return page(
        "FlexControl Actions",
        "The actions a FlexControl knob's buttons can run, and their "
        "defaults, generated from the source.",
        sources, intro, body, 4,
    )


# ---------------------------------------------------------------------------
# Ulanzi Dial


def parse_ulanzi(shortcuts, midi):
    dlg = Source(ULANZI_DIALOG_CPP)
    maps = Source(ULANZI_MAPPINGS_CPP)
    rotary = string_rows(dlg, "kRotaryOptions")
    pills = string_rows(dlg, "kPillSpecs")
    r0, r1 = function_body(maps, r"QString\s+UlanziDialMappings::rotaryAction")
    m = re.findall(r"QStringLiteral\(\s*\"(\w+)\"\s*\)\s*:\s*action", maps.code[r0:r1])
    if len(m) != 1:
        raise ParseError(f"{maps.rel}: cannot read the default rotary action")
    default_rotary = m[0]
    if default_rotary not in {r[1] for r in rotary}:
        raise ParseError(f"{dlg.rel}: default rotary action {default_rotary!r} "
                         "is not in kRotaryOptions")

    short_names = {a["id"]: (a["category"], a["name"]) for a in shortcuts}
    midi_names = {p["id"]: (p["category"], p["name"]) for p in midi}
    resolved = []
    for p in pills:
        pill_id, label, action = p[0], p[1], p[3]
        if action == "None":
            text = ""
        elif action.startswith("shortcut:"):
            ref = action.split(":", 1)[1]
            if ref not in short_names:
                raise ParseError(f"{dlg.rel}: pill {pill_id} default {action!r} "
                                 "is not a registered shortcut")
            text = "[%s] %s" % short_names[ref]
        elif action.startswith("midi:"):
            ref = action.split(":", 1)[1]
            if ref not in midi_names:
                raise ParseError(f"{dlg.rel}: pill {pill_id} default {action!r} "
                                 "is not a registered MIDI control")
            text = "[MIDI %s] %s" % midi_names[ref]
        else:
            raise ParseError(f"{dlg.rel}: pill {pill_id} has an unknown default "
                             f"{action!r}")
        resolved.append((pill_id, label, action, text))
    return rotary, default_rotary, resolved, [dlg.rel, maps.rel]


def render_ulanzi(rotary, default_rotary, pills, sources) -> str:
    intro = [
        "Set the Ulanzi Dial up in **Settings → Ulanzi Dial Mapping...**.",
        "",
        "- The **wheel** runs one of the actions below.",
        "- Each **button** can run any action from "
        "[Default Keyboard Shortcuts](./default-shortcuts.md), or any "
        "Toggle, Trigger or Gate control from "
        "[MIDI Controller Actions](./midi-actions.md).",
    ]
    rows = []
    for label, action_id in rotary:
        mark = "Default" if action_id == default_rotary else ""
        m = re.fullmatch(r"\[([^\]]+)\]\s*(.+)", label)
        if not m:
            raise ParseError(f"{ULANZI_DIALOG_CPP}: wheel option {label!r} is not "
                             "labelled \"[Category] Name\"")
        rows.append([esc(m.group(1)), esc(m.group(2)), code(action_id), mark])
    body = ["## Wheel actions", ""] + table(
        ["Category", "Action", "ID", "Default"], rows) + [""]
    rows = [[esc(label), code(pid), esc(text) if text else "Unassigned",
             code(action) if action != "None" else ""]
            for pid, label, action, text in pills]
    body += ["## Button defaults", ""] + table(
        ["Button", "Button ID", "Default action", "Stored as"], rows) + [""]
    return page(
        "Ulanzi Dial Actions",
        "The Ulanzi Dial's wheel actions and button defaults, generated from "
        "the source.",
        sources, intro, body, 5,
    )


# ---------------------------------------------------------------------------
# HID devices (Stream Deck+, RC-28, PowerMate, ShuttleXpress, TMate 2)


def parse_hid():
    src = Source(RADIO_SETUP_CPP)
    t = {}
    for name in ("kKeyActions", "kEncoderActions", "kPushActions",
                 "kTMate2KeyActions", "kTMate2EncoderActions",
                 "kTMate2PushActions"):
        t[name] = string_rows(src, name)
    for name in ("kEncoderDefaults", "kPushDefaults", "kTMate2KeyDefaults",
                 "kTMate2EncoderDefaults", "kTMate2PushDefaults"):
        t[name] = string_rows(src, name)
    pairs = [("kEncoderDefaults", "kEncoderActions"),
             ("kPushDefaults", "kPushActions"),
             ("kTMate2KeyDefaults", "kTMate2KeyActions"),
             ("kTMate2EncoderDefaults", "kTMate2EncoderActions"),
             ("kTMate2PushDefaults", "kTMate2PushActions")]
    for dname, aname in pairs:
        ids = {a[0] for a in t[aname]}
        for d in t[dname]:
            if d not in ids:
                raise ParseError(f"{src.rel}: {dname} has {d!r}, not in {aname}")
    # The page states one build condition for every table, so they must agree.
    conds = set()
    for name in [n for n in t]:
        _, pos = array_initializer(src, name)
        conds.add(src.conditions(pos))
    if len(conds) != 1:
        raise ParseError(f"{src.rel}: the HID and TMate 2 tables are under "
                         f"different build conditions {sorted(conds)}; give each "
                         "section its own note")
    t["conds"] = conds.pop()
    return t, [src.rel]


def render_hid(t, sources) -> str:
    def label_of(table_rows, ident):
        return {a[0]: a[1] for a in table_rows}[ident]

    def actions_table(rows):
        return table(["Action", "ID"], [[esc(a[1]), code(a[0])] for a in rows])

    def defaults_table(what, defaults, actions, prefix):
        return table([what, "Default"], [
            [f"{prefix} {i + 1}", esc(label_of(actions, d))]
            for i, d in enumerate(defaults)])

    intro = [
        "Set these up in **Settings → Radio Setup... → Serial & Controllers**. "
        "The encoder tables also apply to single-encoder HID devices such as "
        "the RC-28, PowerMate and ShuttleXpress, which use Encoder 1 only.",
    ]
    if t["conds"]:
        intro += ["", "These controls exist only in builds with "
                  + join_conditions(t["conds"]) + "."]
    body = ["## Stream Deck+ and HID encoders", "",
            "### LCD key actions", ""]
    body += actions_table(t["kKeyActions"]) + [""]
    body += ["### Encoder turn actions", ""]
    body += defaults_table("Encoder", t["kEncoderDefaults"], t["kEncoderActions"],
                           "Encoder") + [""]
    body += actions_table(t["kEncoderActions"]) + [""]
    body += ["### Encoder push actions", ""]
    body += defaults_table("Encoder", t["kPushDefaults"], t["kPushActions"],
                           "Encoder") + [""]
    body += actions_table(t["kPushActions"]) + [""]
    body += ["## TMate 2", "", "### Function key actions", ""]
    body += defaults_table("Key", t["kTMate2KeyDefaults"], t["kTMate2KeyActions"],
                           "F") + [""]
    body += actions_table(t["kTMate2KeyActions"]) + [""]
    body += ["### Encoder turn actions", ""]
    body += defaults_table("Encoder", t["kTMate2EncoderDefaults"],
                           t["kTMate2EncoderActions"], "Encoder") + [""]
    body += actions_table(t["kTMate2EncoderActions"]) + [""]
    body += ["### Encoder push actions", ""]
    body += defaults_table("Encoder", t["kTMate2PushDefaults"],
                           t["kTMate2PushActions"], "Encoder") + [""]
    body += actions_table(t["kTMate2PushActions"]) + [""]
    return page(
        "Stream Deck+ and HID Controller Actions",
        "The actions and defaults for the Stream Deck+, HID encoders and the "
        "TMate 2, generated from the source.",
        sources, intro, body, 6,
    )


# ---------------------------------------------------------------------------
# Log categories


def parse_log_categories():
    src = Source(LOG_MANAGER_CPP)
    b0, b1 = function_body(src, r"LogManager::LogManager")
    m = re.search(r"\bm_categories\s*=\s*\{", src.code[b0:b1])
    if not m:
        raise ParseError(f"{src.rel}: m_categories initializer not found")
    open_pos = b0 + m.end() - 1
    close = match_close(src.code, open_pos)
    cats = []
    for part in split_top(src.code[open_pos + 1:close]):
        if not part:
            continue
        where = src.where(open_pos)
        if not part.startswith("{"):
            raise ParseError(f"{where}: unexpected category entry {part!r}")
        fields = split_top(part[1:match_close(part, 0)])
        if len(fields) != 3:
            raise ParseError(f"{where}: category entry with {len(fields)} fields")
        cats.append([need_literal(f, where) for f in fields])
    loose = len(re.findall(r'\{\s*"aether\.', src.code[open_pos:close]))
    if loose != len(cats):
        raise ParseError(f"{src.rel}: {loose} category rows, parsed {len(cats)}")
    ids = [c[0] for c in cats]
    dups = sorted({i for i in ids if ids.count(i) > 1})
    if dups:
        raise ParseError(f"{src.rel}: duplicate category ids {dups}")
    return cats, [src.rel]


def render_log_categories(cats, sources) -> str:
    intro = [
        "These are the logging categories listed in **Help → Support & "
        "Diagnostics...**. Turn one on there to add its detail to the support "
        "log. The **Category** is the name the automation bridge's `log set` "
        "verb takes.",
    ]
    rows = [[esc(label), code(cid), esc(desc)] for cid, label, desc in cats]
    body = table(["Label", "Category", "What it logs"], rows) + [""]
    return page(
        "Log Categories",
        "The logging categories AetherSDR offers for support logs, generated "
        "from the source.",
        sources, intro, body, 7,
    )


# ---------------------------------------------------------------------------
# TCI commands

EXTENSION_MARKER = "AetherSDR extensions"


def parse_tci():
    proto = Source(TCI_PROTOCOL_CPP)
    server = Source(TCI_SERVER_CPP)

    p0, p1 = function_body(proto, r"QString\s+TciProtocol::handleCommand")
    marker = proto.raw.find(EXTENSION_MARKER, p0, p1)
    if marker == -1:
        raise ParseError(f"{proto.rel}: the {EXTENSION_MARKER!r} comment is gone "
                         "from handleCommand(); say which commands are extensions")
    commands = []  # (name, extension, layer)
    for m in re.finditer(r'\bname\s*==\s*("[^"]*")', proto.code[p0:p1]):
        pos = p0 + m.start()
        commands.append((need_literal(m.group(1), proto.where(pos)), pos > marker))
    loose = len(re.findall(r"\bname\s*==", proto.code[p0:p1]))
    if loose != len(commands):
        raise ParseError(f"{proto.rel}: {loose} name comparisons, parsed {len(commands)}")

    s0, s1 = function_body(server, r"void\s+TciServer::onTextMessage")
    stop = server.code.find("client.protocol->handleCommand", s0, s1)
    if stop == -1:
        raise ParseError(f"{server.rel}: onTextMessage no longer hands off to "
                         "TciProtocol::handleCommand")
    session = []
    probe = re.compile(
        r"\btrimmed\s*(?:\.startsWith\s*\(|==)\s*(?:QLatin1String\s*\(\s*)?(\"[^\"]*\")")
    for m in probe.finditer(server.code, s0, stop):
        # "iq_start:" and "spectrum_event:on" both name a command before the colon.
        name = need_literal(m.group(1), server.where(m.start())).split(":", 1)[0]
        if not re.fullmatch(r"[a-z_]+", name):
            raise ParseError(f"{server.where(m.start())}: unexpected TCI name {name!r}")
        if name not in session:
            session.append(name)
    loose = len(re.findall(r"\btrimmed\s*(?:\.startsWith\s*\(|==)", server.code[s0:stop]))
    if loose != len(probe.findall(server.code, s0, stop)):
        raise ParseError(f"{server.rel}: a TCI command test in onTextMessage has an "
                         "unexpected shape")
    names = [c[0] for c in commands]
    dups = sorted({n for n in names if names.count(n) > 1})
    if dups:
        raise ParseError(f"{proto.rel}: duplicate TCI commands {dups}")
    overlap = sorted(set(names) & set(session))
    if overlap:
        raise ParseError(f"TCI commands handled in both places: {overlap}")
    return commands, session, [proto.rel, server.rel]


def render_tci(commands, session, sources) -> str:
    intro = [
        "These are the command names the TCI server recognises from a client. "
        "Names are case-insensitive. A command the server does not recognise "
        "is ignored, as the TCI specification requires. Recognising a command "
        "does not mean every radio supports every value of it; see the TCI "
        "Server page for behaviour.",
    ]
    std = [c for c, ext in commands if not ext]
    ext = [c for c, ext in commands if ext]
    body = ["## Audio, IQ and session commands", "",
            "Handled per client connection: audio and IQ streams, stream "
            "format, and sensor reports.", ""]
    body += table(["Command"], [[code(c)] for c in session]) + [""]
    body += ["## Radio commands", ""]
    body += table(["Command"], [[code(c)] for c in std]) + [""]
    body += ["## AetherSDR extensions", "",
             "Not part of the TCI specification.", ""]
    body += table(["Command"], [[code(c)] for c in ext]) + [""]
    return page(
        "TCI Commands",
        "The TCI command names AetherSDR's TCI server recognises, generated "
        "from the source.",
        sources, intro, body, 8,
    )


# ---------------------------------------------------------------------------
# Index


PAGES = [
    ("default-shortcuts.md", "Default Keyboard Shortcuts",
     "assignable keyboard actions and their default keys"),
    ("midi-actions.md", "MIDI Controller Actions",
     "every control you can map to a MIDI controller"),
    ("flexcontrol-actions.md", "FlexControl Actions",
     "FlexControl button actions and defaults"),
    ("ulanzi-dial-actions.md", "Ulanzi Dial Actions",
     "Ulanzi Dial wheel actions and button defaults"),
    ("hid-controller-actions.md", "Stream Deck+ and HID Controller Actions",
     "Stream Deck+, HID encoder and TMate 2 actions and defaults"),
    ("log-categories.md", "Log Categories",
     "logging categories for support logs"),
    ("tci-commands.md", "TCI Commands",
     "command names the TCI server recognises"),
]


def render_index() -> str:
    fm = [
        "---",
        'title: "Generated Reference"',
        'description: "Reference tables generated from the AetherSDR source, '
        'so they always match the code."',
        "sidebar_position: 1",
        "custom_edit_url: null",
        f"generated_by: {yaml_str(GENERATOR)}",
        "---",
        "",
        "The pages in this section are generated from the AetherSDR source "
        "code, not written by hand, so they always match the version of the "
        "app they were built with.",
        "",
    ]
    for fname, title, what in PAGES:
        fm.append(f"- [{title}](./{fname}): {what}.")
    fm += [
        "",
        "## Refreshing these pages",
        "",
        "Run this from the top of the source tree after changing any of the "
        "source files a page names:",
        "",
        "```bash",
        f"python3 {GENERATOR}",
        "```",
        "",
        f"`python3 {GENERATOR} --check` changes nothing and exits with an "
        "error, printing the difference, when a committed page no longer "
        "matches the source. CI runs it, so a change that alters one of "
        "these tables has to carry the regenerated page with it.",
    ]
    return "\n".join(fm) + "\n"


# ---------------------------------------------------------------------------


def render_all() -> dict:
    shortcuts, categories, s_src = parse_shortcuts()
    MODES_CACHE["modes"] = [(a["name"], a["conds"]) for a in shortcuts
                            if a.get("mode")]
    midi, m_src = parse_midi()
    pages = {
        "index.md": render_index(),
        "default-shortcuts.md": render_shortcuts(shortcuts, categories, s_src),
        "midi-actions.md": render_midi(midi, m_src),
    }
    pages["flexcontrol-actions.md"] = render_flexcontrol(*parse_flexcontrol())
    pages["ulanzi-dial-actions.md"] = render_ulanzi(*parse_ulanzi(shortcuts, midi))
    pages["hid-controller-actions.md"] = render_hid(*parse_hid())
    pages["log-categories.md"] = render_log_categories(*parse_log_categories())
    pages["tci-commands.md"] = render_tci(*parse_tci())
    if set(pages) != {"index.md"} | {p[0] for p in PAGES}:
        raise ParseError("PAGES and render_all() list different pages")
    return pages


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--check", action="store_true",
                    help="exit 1 and print a diff if a committed page is stale")
    ap.add_argument("--sources", action="store_true",
                    help="print the source files the pages are generated from")
    args = ap.parse_args(argv)

    if args.sources:
        for s in ALL_SOURCES:
            print(s)
        return 0

    try:
        pages = render_all()
    except ParseError as exc:
        print(f"error: {exc}", file=sys.stderr)
        print(f"error: {GENERATOR} could not parse the source; teach it the "
              "new shape rather than editing the generated pages",
              file=sys.stderr)
        return 1

    existing = {p.name for p in OUT_DIR.glob("*.md")} if OUT_DIR.is_dir() else set()
    stray = sorted(existing - set(pages))

    if args.check:
        stale = False
        for name, text in pages.items():
            path = OUT_DIR / name
            old = path.read_text(encoding="utf-8") if path.exists() else ""
            if old != text:
                stale = True
                rel = path.relative_to(REPO).as_posix()
                sys.stdout.writelines(difflib.unified_diff(
                    old.splitlines(keepends=True), text.splitlines(keepends=True),
                    fromfile=f"a/{rel}", tofile=f"b/{rel}"))
        for name in stray:
            stale = True
            print(f"stale: {(OUT_DIR / name).relative_to(REPO).as_posix()} is "
                  "no longer generated; delete it")
        if stale:
            print(f"\nerror: generated reference pages are stale; run "
                  f"`python3 {GENERATOR}` and commit the result", file=sys.stderr)
            return 1
        print(f"generated reference pages are up to date ({len(pages)} pages)")
        return 0

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for name in stray:
        (OUT_DIR / name).unlink()
        print(f"removed {name}")
    changed = 0
    for name, text in pages.items():
        path = OUT_DIR / name
        if not path.exists() or path.read_text(encoding="utf-8") != text:
            path.write_text(text, encoding="utf-8")
            changed += 1
    print(f"wrote {len(pages)} pages to "
          f"{OUT_DIR.relative_to(REPO).as_posix()} ({changed} changed)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
