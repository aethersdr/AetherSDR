#!/usr/bin/env python3
"""
AetherSDR seam-probe table generator.

tests/SeamThreadAffinityProbe.h connects to EVERY signal IRadioBackend
declares, so backend_seam_affinity_test can assert the threading contract
across the whole seam. That table used to be hand-maintained, which made it a
second copy of a list that already exists in
src/core/backends/IRadioBackend.h — and two lists kept in agreement by memory
drift.

They drifted. #5825 added `autoRfGainArmSettled` to IRadioBackend.h without the
matching probe line. backend_seam_affinity_test was GREEN in that PR and RED
the instant it merged: "51 probed, 52 declared". #5865 repaired it with one
line, and left the mechanism intact. #5868 (G6PWY-Chris) asks for the static
check this file's --check mode provides; the generation is the extra half.

WHY THE PR COULD NOT SEE IT — the narrow version, because the broad ones are
false. Detection was not absent: full-suite.yml runs unfiltered ctest on every
push to main and files a sticky issue, which is how this surfaced. Nor is the
per-PR lane blind to whole-tree invariants: static-checks.yml runs nine
build-free whole-tree checkers on every PR. The true statement is narrower —
this particular invariant was expressed as a ctest rather than as a static
check, and ctest is what the PR lane does not run over it
(.github/ci-test-gate.txt is frozen, and ci.yml's Linux job runs no ctest at
all). So detection was post-merge, not missing.

--check moves that assertion to where it can block the PR. Generation goes one
step further: a checker still leaves two lists that must agree, kept in sync by
hand and merely audited; a generator leaves one list, and makes the divergence
unrepresentable rather than merely reported.

    tests/SeamSignalProbeTable.inc
        GENERATED. One `AETHER_SEAM_PROBE(<signal>);` line per signal
        IRadioBackend declares, in declaration order. Included by
        tests/SeamThreadAffinityProbe.h inside attachAllSeamSignals().

WHICH DECLARATIONS COUNT — ACCESS LABELS DECIDE, NOT POSITION.
IRadioBackend has ONE `signals:` section today (the scanner handles several,
because several are ordinary C++ and their union is unambiguous), and it ENDS
at the next access label. Below it sit `protected:` and `private:` sections
holding ordinary inline member functions — `publishLegacyAudio`,
`publishLegacySliceAudio`,
`warnAudioDropped`. Those are not signals and must not be probed (probing one
would not even compile: `&IRadioBackend::publishLegacyAudio` is not a signal,
so QObject::connect rejects it). A scanner that finds `signals:` and then takes
every `void f(...);` to the closing brace picks all three up. The same trap
tools/check_capability_records.py navigates for struct scopes, and
tools/test_gen_seam_probe_table.py pins it.

TWO DERIVATIONS, AND HOW THE SECOND ONE IS KEPT HONEST.
backend_seam_affinity_test's own declaredSeamSignals() reads
IRadioBackend::staticMetaObject at RUNTIME — moc's answer, the authoritative
one. That derivation cannot be reused by a static check: it needs moc, a
compiler and a link, which is exactly the cost this gate exists to avoid. So
this file parses the header instead, and the parser is now the thing that can
be wrong.

Over-reading is loud on its own: AETHER_SEAM_PROBE expands to
`&IRadioBackend::<sig>`, and a non-signal is not connectable, so the build
fails.

UNDER-READING IS THE DANGEROUS DIRECTION, AND --check CANNOT SEE IT.
--check compares the committed table against THIS parser's output. A parser
that misses a signal generates a table that is short in exactly the same way,
the two agree, and --check prints "up to date". The static gate would then be
silent about the one failure it exists to catch, and the first thing to speak
would be backend_seam_affinity_test — at runtime, post-merge. That is #5825
arriving by a new route.

residue() is the answer: after parsing, the signal section is re-scanned for
anything that LOOKS like a declared signal and did not become a probe, and the
tool refuses by name. Add a construct this scanner cannot read — a trailing
return type, Q_SIGNAL, a brace-initialised default argument it mis-splits —
and you get a refusal that names it, never a table that is quietly short.
Refusing is the whole design: a scanner that is merely usually right would
reintroduce, one level down, the two-lists-that-must-agree bug this file
exists to remove.

AND THE GUARANTEE IS A FLOOR WITH NAMED GAPS, NOT A PROOF. Saying so is not a
hedge, it is the same argument one level up: a refusal claim whose edges are
unstated is exactly the kind of thing that gets believed past where it holds.
residue() refuses on "looks like a signal I did not emit", which is a floor
over the constructs a future header could plausibly acquire — it is not a
decision procedure for C++. The gaps known today, each found by ATTACKING this
file rather than by reading it:

  - THE SHAPE TO WATCH FOR: anything that truncates class_body() BEFORE
    residue() runs is invisible to residue() by construction, because the
    declarations it should object to were never in the section it scans. An
    encoding-prefixed char literal holding a closing brace (`L'}'`) was one
    such hole, found by G6PWY-Chris reviewing #5897 and closed by
    DIGIT_SEPARATOR_RE. The other member found so far is a raw string
    containing a `"` (`R"(a"b)"`), which fails LOUDLY as an unterminated class
    body — the correct direction, and the reason it is not fixed here. A third
    member of that class, if one exists, would be silent.
  - A declarator list whose FIRST declarator carries a parenthesised default
    argument (`void a(int x = f(1)), b();`) walks past
    MULTI_DECLARATOR_RE's `[^()]*` and still drops `b` in silence. The plain
    forms (`void a(), b();`, `void a(int x), b();`) are refused. Reading a
    declarator list properly means parsing one, and the refusal is the cheaper
    honest answer for a construct moc is unlikely to accept in a `signals:`
    section at all.

Outside the floor entirely: a signal this file's parse never reaches — one
declared in a base class, or in a header this tool does not read. Nothing
static covers that; backend_seam_affinity_test's runtime reading of
staticMetaObject does, post-merge, which is the cost #5868 set out to reduce
rather than eliminate.

Usage:
    python tools/gen_seam_probe_table.py            # regenerate
    python tools/gen_seam_probe_table.py --check    # exit 1 if stale

Exit 0 on success; --check exits 1 if the committed table differs from what
would be generated, naming the missing or extra signals.

stdlib only; no third-party dependencies.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

BACKEND_H = REPO / "src" / "core" / "backends" / "IRadioBackend.h"
PROBE_TABLE = REPO / "tests" / "SeamSignalProbeTable.inc"

CLASS_NAME = "IRadioBackend"

# An access-specifier label at class scope. Qt spells the signal section
# `signals:` or `Q_SIGNALS:`, optionally qualified (`public signals:`); slots
# are ordinary member functions and never belong in the probe table.
ACCESS_LABEL_RE = re.compile(
    r"^\s*(?:(?P<access>public|protected|private)\s*)?"
    r"(?P<kind>signals|Q_SIGNALS|slots|Q_SLOTS)?\s*:(?!:)"
)
# A member function declaration. Signals always return void.
SIGNAL_DECL_RE = re.compile(r"\bvoid\s+(?P<name>[A-Za-z_]\w*)\s*\(")
# Conditional compilation inside the signal section. #5868 asks for this
# explicitly: the text parse is viable BECAUSE the section has no #if today,
# so the day that stops being true this must fail loudly rather than quietly
# emit whichever branch it happened to read. A signal behind an #if is a
# signal moc may or may not declare, and a single flat table cannot say so.
PREPROCESSOR_RE = re.compile(r"^\s*#\s*(?P<directive>[A-Za-z_]\w*)")
# Two shapes this scanner deliberately does NOT read, checked for by residue()
# so that meeting one is a refusal rather than a short table.
TRAILING_RETURN_RE = re.compile(r"->\s*void\b")
QT_SIGNAL_MACRO_RE = re.compile(r"\bQ_SIGNAL\b(?!S)")
# One declaration, TWO DECLARATORS: `void a(), b();`. SIGNAL_DECL_RE.search
# takes the first, and residue()'s finditer never sees a `void ` before `b(`,
# so `b` would drop with no refusal at all. `[^()]*` declines nested
# parentheses on purpose: this refuses the plain form rather than trying to
# parse a declarator list, and the module docstring names what that leaves out.
MULTI_DECLARATOR_RE = re.compile(r"\bvoid\s+[A-Za-z_]\w*\s*\([^()]*\)\s*,")
# Tells a C++14 DIGIT SEPARATOR (48'000, 0x1F'FF) from an ENCODING PREFIX
# (L'}', u'}', U'}', u8'{'). Both put an identifier character immediately
# before a quote, which is all the first version of this guard looked at — so
# the prefixed forms were read as separators, the literal was never blanked,
# and a brace inside it moved class_body()'s depth. A CLOSING brace is the
# dangerous direction: the class ends early, every signal below it vanishes,
# and residue() cannot object because the section it scans never contained
# them. The distinguishing rule is that a separator only ever appears inside a
# NUMERIC literal, so the run of identifier characters ending at the quote
# STARTS WITH A DIGIT; an encoding prefix starts with a letter.
DIGIT_SEPARATOR_RE = re.compile(r"(?<![A-Za-z_])[0-9][0-9A-Fa-fXxbB']*\Z")
# The line the table is made of, for reading a committed table back.
PROBE_LINE_RE = re.compile(r"^\s*AETHER_SEAM_PROBE\((?P<name>[A-Za-z_]\w*)\);\s*$")

HEADER_COMMENT = """\
// GENERATED FILE — regenerate with `python tools/gen_seam_probe_table.py`.
// Derived from the `signals:` section of src/core/backends/IRadioBackend.h,
// in declaration order. Add the signal to the header; never edit this table.
//
// Included by tests/SeamThreadAffinityProbe.h inside attachAllSeamSignals(),
// which defines AETHER_SEAM_PROBE. Not a standalone header: it has no include
// guard on purpose, because it is a list and not a translation unit.
//
// Kept honest by `python tools/gen_seam_probe_table.py --check` in
// .github/workflows/static-checks.yml, on every pull request, without a build.
"""


def strip_comments_and_strings(text: str) -> str:
    """Blank out //, /* */, "..." and '...' while preserving line structure.

    Brace counting and label matching both run over the result, so a `{` in a
    comment (IRadioBackend.h has one: a prose aside spelling a preamp range as
    {"OFF", "P.AMP1", "P.AMP2"}) cannot move the depth, and a `signals:` named
    in prose cannot open a section. Newlines survive so reported positions and
    declaration order stay faithful to the source.
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        two = text[i:i + 2]
        if two == "//":
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif two == "/*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("\n" * text.count("\n", i, j))
            i = j
        elif c == "'" and DIGIT_SEPARATOR_RE.search("".join(out[-24:])):
            # A C++14 DIGIT SEPARATOR, not a quote: 48'000, 0x1F'FF. Treating
            # it as one opens a literal that closes at the NEXT separator and
            # blanks everything between — so `int a = 48'000` and
            # `int b = 96'000` two declarations apart silently swallow the
            # declarations in between.
            #
            # The test is on the RUN, not on the single preceding character.
            # `out[-1].isalnum()` is also true after an encoding prefix, so
            # L'}' and u8'{' were misread as separators and left unstripped —
            # see DIGIT_SEPARATOR_RE for what that costs. \Z rather than $:
            # with $ a run of digits followed by a blanked multi-line comment
            # would still match across the trailing newline, and guessing
            # "separator" is the direction that under-reads.
            out.append(c)
            i += 1
        elif c in "\"'":
            quote, j = c, i + 1
            while j < n and text[j] != quote:
                j += 2 if text[j] == "\\" else 1
            out.append("\n" * text.count("\n", i, min(j + 1, n)))
            i = min(j + 1, n)
        else:
            out.append(c)
            i += 1
    return "".join(out)


def class_body(text: str, name: str) -> str:
    """The body of `class <name>`, braces excluded. Raises if absent."""
    m = re.search(r"\bclass\s+" + re.escape(name) + r"\b[^;{]*\{", text)
    if not m:
        raise SystemExit(
            f"gen_seam_probe_table: class {name} not found in "
            f"{BACKEND_H.relative_to(REPO)}")
    start = m.end()
    depth = 1
    for i in range(start, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start:i]
    raise SystemExit(
        f"gen_seam_probe_table: class {name} body is unterminated in "
        f"{BACKEND_H.relative_to(REPO)}")


def signals_section_text(body: str, class_name: str) -> str:
    """The text of every signal section in `body`, at the class's OWN scope.

    A label is honoured only at depth 0 of the class body, so a nested struct
    or enum carrying its own `public:` cannot end a signal section and a nested
    class's members can never be mistaken for the outer class's signals. A
    label ENDS the previous section whatever it opens — which is the whole
    reason publishLegacyAudio, publishLegacySliceAudio and warnAudioDropped
    stay out: they sit BELOW `signals:` in the file, but under `protected:`
    and `private:`.

    Any preprocessor directive inside a signal section is refused. #5868 asks
    for exactly that: the text parse is viable BECAUSE the section carries no
    #if today, and a checker that silently passes when its parse assumption
    breaks is worse than no checker, because the green tick is now false and
    load-bearing.
    """
    out: list[str] = []
    in_signals = False
    depth = 0

    for line in body.splitlines():
        label = ACCESS_LABEL_RE.match(line) if depth == 0 else None
        if label and (label.group("access") or label.group("kind")):
            in_signals = label.group("kind") in ("signals", "Q_SIGNALS")
            line = line[label.end():]

        if in_signals:
            directive = PREPROCESSOR_RE.match(line)
            if directive:
                raise SystemExit(
                    "gen_seam_probe_table: preprocessor directive "
                    f"(#{directive.group('directive')}) inside {class_name}'s "
                    "signals section. The table is a flat list and cannot "
                    "express a signal that exists in only some builds — teach "
                    "this tool the configuration, or keep the section "
                    "unconditional. Refusing rather than emitting one branch.")
            out.append(line)
        else:
            out.append("")   # keep line numbering honest for anyone debugging

        depth += line.count("{") - line.count("}")
        if depth < 0:                      # defensive: malformed input
            depth = 0
    return "\n".join(out)


def split_declarations(text: str) -> list[str]:
    """Split a signal section into declarations at each top-level `;`.

    Depth matters: a brace-initialised default argument
    (`void f(const QVariantMap& m = {});`) opens a brace, and splitting on a
    `;` inside it would cut a declaration in half — while IGNORING lines at
    depth > 0, as an earlier version did, loses the `;` entirely and makes the
    declaration swallow the NEXT one. Both failures are silent: the table is
    simply short, and `--check` compares it against this same parser, so it
    agrees. residue() is the net under that; this is the fix.
    """
    decls: list[str] = []
    depth = 0
    pending: list[str] = []
    for ch in text:
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth = max(0, depth - 1)
        elif ch == ";" and depth == 0:
            decls.append("".join(pending))
            pending = []
            continue
        pending.append(ch)
    if "".join(pending).strip():
        decls.append("".join(pending))
    return decls


def residue(section: str, body: str, names: list[str]) -> list[str]:
    """Things in the signal section this scanner did NOT turn into a probe.

    THE POINT OF THIS FUNCTION. `--check` compares the committed table against
    THIS parser's output, so a parser that misses a signal produces a table
    that is short in exactly the same way and reports "up to date". The static
    gate would then be silent about the one failure it exists to catch, and
    the first thing to speak would be backend_seam_affinity_test — at runtime,
    post-merge. That is #5825 arriving by a new route.

    So under-reading must be LOUD. Anything in the section that looks like a
    declared signal and did not become a probe is refused by name, rather than
    quietly dropped. The constructs below are the ones a future header could
    plausibly acquire; the list is a floor, not a proof of completeness, which
    is why it refuses on "looks like a signal I did not emit" rather than on
    an enumeration of known-bad syntax.
    """
    found = set(names)
    problems: list[str] = []
    for m in SIGNAL_DECL_RE.finditer(section):
        if m.group("name") not in found:
            problems.append(
                f"`void {m.group('name')}(` is declared in the signals "
                "section and did not become a probe")
    if MULTI_DECLARATOR_RE.search(section):
        problems.append(
            "a declarator list (`void a(), b();`) — this scanner reads ONE "
            "declarator per declaration, so every name after the first comma "
            "would be dropped without a word")
    if TRAILING_RETURN_RE.search(section):
        problems.append(
            "a trailing return type (`auto f() -> void`) — this scanner "
            "anchors on a leading `void`")
    if QT_SIGNAL_MACRO_RE.search(body):
        # Scanned over the WHOLE class body, not just the section: Q_SIGNAL's
        # entire purpose is to declare a signal WITHOUT a signals: label, so
        # looking for it only inside one is looking where it cannot be.
        problems.append(
            "Q_SIGNAL, Qt's single-declaration signal macro — it declares a "
            "signal with no signals: label, and this scanner is section-based")
    return problems


def seam_signals(header_text: str, class_name: str = CLASS_NAME) -> list[str]:
    """Every signal `class_name` declares, in declaration order.

    Refuses rather than guesses. Two lists that must agree is the bug being
    designed out here, so a scanner that is merely usually right would
    reintroduce it one level down.
    """
    body = class_body(strip_comments_and_strings(header_text), class_name)
    section = signals_section_text(body, class_name)

    names: list[str] = []
    unreadable: list[str] = []
    for decl in split_declarations(section):
        m = SIGNAL_DECL_RE.search(decl)
        if not m:
            # Not a blank run between declarations (comments strip to
            # whitespace) but real text this scanner could not read — a
            # macro-expanded declaration, most likely, which no regex can
            # resolve without a preprocessor. Collect it for residue() rather
            # than skip it: skipping is how a table goes quietly short.
            if decl.strip():
                unreadable.append(" ".join(decl.split())[:80])
            continue
        name = m.group("name")
        if name in names:
            # A DEFAULT ARGUMENT declares one C++ function that moc reports as
            # two meta-methods — that is why declaredSeamSignals() calls
            # removeDuplicates(). A genuine OVERLOAD is two declarations, and
            # AETHER_SEAM_PROBE(name) would then expand to an ambiguous
            # `&IRadioBackend::name` that does not compile. A flat table
            # cannot express it, so refuse, exactly as for a #if.
            raise SystemExit(
                f"gen_seam_probe_table: `{name}` is declared twice in "
                f"{class_name}'s signals section. AETHER_SEAM_PROBE takes a "
                "NAME, and `&" + class_name + f"::{name}` is ambiguous across "
                "overloads, so no single probe line can cover both. Give the "
                "overloads distinct names, or teach the probe to disambiguate.")
        names.append(name)

    problems = residue(section, body, names)
    problems += [f"`{u}` is a declaration this scanner could not read"
                 for u in unreadable]
    if problems:
        raise SystemExit(
            f"gen_seam_probe_table: {class_name}'s signals section contains "
            "declarations this scanner cannot turn into probe lines, so the "
            "table would be silently SHORT — the exact failure this tool "
            "exists to prevent:\n  - " + "\n  - ".join(problems)
            + "\nTeach tools/gen_seam_probe_table.py the construct (and pin "
              "it in tools/test_gen_seam_probe_table.py). Refusing rather "
              "than emitting an incomplete table.")
    return names


def render(names: list[str]) -> str:
    lines = [HEADER_COMMENT]
    lines += [f"AETHER_SEAM_PROBE({n});" for n in names]
    return "\n".join(lines) + "\n"


def committed_names(text: str) -> list[str]:
    return [m.group("name")
            for m in (PROBE_LINE_RE.match(ln) for ln in text.splitlines())
            if m]


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Generate the IRadioBackend seam-probe table")
    ap.add_argument("--check", action="store_true",
                    help="exit 1 if the committed probe table is stale")
    args = ap.parse_args()

    try:
        names = seam_signals(BACKEND_H.read_text(errors="replace"))
    except SystemExit as refusal:
        # A refusal is the most deliberately designed failure this tool has —
        # it must not be the one GitHub declines to annotate. SystemExit writes
        # to stderr with no ::error:: prefix, so re-say it on stdout in the
        # shape static-checks.yml's other steps use.
        for line in str(refusal).splitlines():
            print(f"::error::{line}" if line.strip() else line)
        return 1
    if not names:
        print(f"::error::no signals found in {BACKEND_H.relative_to(REPO)} —"
              " the scanner is broken, not the header")
        return 1
    out = render(names)

    if args.check:
        current = PROBE_TABLE.read_text() if PROBE_TABLE.is_file() else ""
        if current == out:
            print(f"seam probe table up to date ({len(names)} signals)")
            return 0

        have = committed_names(current)
        missing = [n for n in names if n not in set(have)]
        extra = [n for n in have if n not in set(names)]
        rel = PROBE_TABLE.relative_to(REPO)
        if missing:
            print(f"::error::{rel} is missing a probe for: "
                  + ", ".join(missing)
                  + " — declared by IRadioBackend and never covered by"
                    " backend_seam_affinity_test")
        if extra:
            print(f"::error::{rel} probes a signal IRadioBackend no longer"
                  " declares: " + ", ".join(extra))
        if not missing and not extra:
            print(f"::error::{rel} differs from the generated table"
                  " (ordering or comment drift)")
        print(f"::error::run `python tools/gen_seam_probe_table.py` and commit"
              f" {rel}")
        return 1

    PROBE_TABLE.parent.mkdir(parents=True, exist_ok=True)
    PROBE_TABLE.write_text(out)
    print(f"wrote {PROBE_TABLE.relative_to(REPO)}: {len(names)} seam signals")
    return 0


if __name__ == "__main__":
    sys.exit(main())
