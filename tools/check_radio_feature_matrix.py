#!/usr/bin/env python3
"""Radio feature register — the derivable cells are GENERATED and DIFFED.

WHY THIS EXISTS. docs/architecture/radio-feature-matrix.md answers the one
question a support matrix normally gets wrong: not "did the backend declare
it" and not "is there a control on screen", but "where does the click END".
A control that does nothing looks exactly like a control that works, so the
document is only worth having if it cannot quietly stop being true.

WHY NOT A TOUCH-COUNTER, WHICH IS THE OBVIOUS DESIGN. The tempting check is
"if you edit a capability field you must also edit the matrix". That enforces
A change, not THE RIGHT change. This project already owns one gate with that
shape — tools/audit_colours.py counts call sites — and the observed behaviour
is that contributors (human and agent) reason about satisfying the counter
rather than about the intent. A counter can be paid off with a comment.

So this checker GENERATES the derivable cells from the source and DIFFS them
against the committed record. A token edit cannot satisfy a diff: to change a
cell you have to change what the code does, or admit the cell was wrong.

WHAT IS DERIVABLE, AND IT IS EXACTLY FOUR QUESTIONS.

  1. Is a capability declared, and to what?   — backend capabilities() bodies
     against the defaults in RadioCapabilities.h. Three answers matter, not
     two: explicitly false, explicitly true, and NEVER ASSIGNED — the third is
     the permissive-default trap that produces a phantom control.
  2. Is the seam virtual overridden?          — the backend's class body.
  3. Is the base body a Q_UNUSED no-op?       — IRadioBackend.h.
  4. Is the control gated above the seam?     — a capability read in src/gui or
     src/models (the GUI gate), or in RadioModel (the model gate, which drops
     the intent one layer up where no backend audit can see it).

Those four decide W, D, P and H/M. Everything else this checker declines to
decide and emits U, which is a legitimate committed value.

THE STATES, which are ON8ST's vocabulary and not this tool's:

  W  works / reachable   — the control reaches the backend and the backend acts
  D  dead                — the control exists but the path terminates
  P  phantom             — reports success without the radio moving; reads back
                           cached state. TWO GROUNDS, and they are different
                           repairs: an inherited permissive default the backend
                           never asked for (one assignment), or a control the
                           backend DID ask for whose stored value nothing ever
                           reads (real work). A backend that asked and simply
                           did not implement is D, not P — see gate_state()
  R  refuses visibly     — the user is told no
  H  hidden, applet-wide — the whole applet is hidden, which the #5262 M3a
                           ruling permits (theme-style-guide.md §4a)
  M  gated per control, pending M3b — the GUI gates ONE control away on a
                           capability (hides it, disables it without an
                           announced reason, or relabels it), where §4a says
                           it must stay visible and dim with a reason
  U  unverified          — the generator could not decide
  V  VERIFIED ON REAL HARDWARE — hand-added, never generated, must cite a run

V IS NEVER GENERATED, AND THAT IS THE POINT. No static read can prove a radio
obeyed; reachability is necessary and not sufficient. So the checker asserts
only two things about a V cell: that it carries a run citation, and that the
generator independently derives W for it. The second is the sharper half — a V
sitting on a cell the source says is dead is either an over-claimed V or a
broken generator, and both need a human.

U IS AN ANSWER, NOT A FAILURE — but it is not an escape hatch either. U is
accepted only where the GENERATOR emitted U. Committing U over a cell the
generator decided is a diff and fails, exactly like any other disagreement.

WHAT THIS CHECKER CANNOT SEE, stated plainly because a green run must not be
read as more than it is:

  * WHETHER THE RADIO OBEYED. W means the call reaches an implementation. The
    bench answers the rest, and only a V cell claims it.
  * A CONTROL THAT NEVER REACHES ITS SIGNAL. The widget-to-model wiring is Qt
    lambdas in constructor bodies and does not yield to a parser. The `control`
    field of each record names the widget; keeping it true is a human job.
  * WIRE TEXT THAT BYPASSES THE SEAM. RadioModel::sendCommand carries SmartSDR
    text that only a Flex ever receives, from 100+ call sites. A record may
    declare an `alt_path` for a backend; the checker verifies the named file
    contains both the named symbol and the literal wire token, which is weaker
    evidence than an override and is marked as such in the document.
    A broken alt_path is TWO different facts and the checker splits them —
    ROTTED fails, RETIRED is reported as progress. See alt_path_state(), which
    carries the whole argument; without the split, every successful seam
    migration raises an error and the error stops being read.
  * WHETHER A MODEL GATE REFUSES VISIBLY OR DROPS SILENTLY. The `visible` flag
    on a model_gate is AUTHORED. The checker verifies the gate exists; it
    cannot read a tr() string and know the operator saw it.
  * WHICH OF H AND M A HIDDEN CELL IS. The generator derives "gated away";
    whether the gate is an applet hidden wholesale (H, compliant with M3a) or
    a single control (M, pending M3b) is a fact about widgets, and widgets do
    not yield to a parser. The record's `hide` block is AUTHORED: it names the
    granularity, the observed form and the sites. The checker holds it to
    three things — every cell that derives hidden has one, the cell code
    agrees with its granularity, and every site still names a symbol that is
    in its file — so the classification cannot outlive the code it describes.
  * AN OVERRIDE THAT REACHES THE WIRE BUT WRITES THE WRONG REGISTER. The base
    IRadioBackend::setXitOffset is an alias into setRitOffset; a backend that
    overrides the target and not the alias is reachable and wrong. The checker
    emits U for an alias base rather than pretending either way.

ANTI-VACUITY. Every parser here fails toward "found nothing", and "found
nothing" would make every cell derive the same way and the diff pass on a
matrix that had stopped meaning anything. The floors below are not tuning
knobs: they separate "the tree moved" from "the parser fell over", and they
return 1 regardless of --strict because a silent disarm is the failure this
whole file exists to prevent.

Usage:
    python tools/check_radio_feature_matrix.py            # report
    python tools/check_radio_feature_matrix.py --strict   # exit 1 on a diff
    python tools/check_radio_feature_matrix.py --print    # derived cells only

stdlib only; no third-party dependencies.
"""

import argparse
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
# Anchored on the script, not the cwd — every sibling checker in tools/ does
# this, so "run it from anywhere" is true of all of them.
SEAM_HEADER = REPO / "src" / "core" / "backends" / "IRadioBackend.h"
CAPS_HEADER = REPO / "src" / "core" / "backends" / "RadioCapabilities.h"
RADIO_MODEL = REPO / "src" / "models" / "RadioModel.cpp"
MATRIX_JSON = REPO / "docs" / "architecture" / "radio-feature-matrix.json"
MATRIX_DOC = REPO / "docs" / "architecture" / "radio-feature-matrix.md"
GATE_DIRS = (REPO / "src" / "gui", REPO / "src" / "models")

# family key -> (class name, header, translation unit, display name)
BACKENDS = {
    "flex": ("FlexBackend", "flex/FlexBackend.h", "flex/FlexBackend.cpp", "Flex"),
    "icom": ("IcomCivBackend", "icom/IcomCivBackend.h", "icom/IcomCivBackend.cpp", "Icom"),
    "hl2": ("Hl2Backend", "hl2/Hl2Backend.h", "hl2/Hl2Backend.cpp", "HL2"),
    "anan": ("AnanBackend", "anan/AnanBackend.h", "anan/AnanBackend.cpp", "ANAN"),
    "rtl": ("RtlSdrBackend", "rtl/RtlSdrBackend.h", "rtl/RtlSdrBackend.cpp", "RTL"),
    "sim": ("SimBackend", "sim/SimBackend.h", "sim/SimBackend.cpp", "Demo"),
}
BACKEND_ORDER = list(BACKENDS)

STATES = set("WDPRHMUV")

# The authored half of a hidden cell. See the docstring: the generator decides
# THAT a control is gated away, the record says HOW, and these are the only
# answers the record may give.
HIDE_GRANULARITY = {"applet": "H", "control": "M"}
HIDE_FORMS = ("hidden", "disabled", "relabelled", "not-closed")

# ---- anti-vacuity floors -----------------------------------------------------
# Each one is "the parser found so little that it cannot have worked". Raise
# one only with the evidence that the tree really shrank that far.
MIN_SEAM_VIRTUALS = 60      # IRadioBackend.h carries 79 today
MIN_CAPS_FIELDS = 90        # RadioCapabilities.h carries 116 members today
MIN_OVERRIDES_PER_BACKEND = 8   # the sparsest backend (ANAN) has 15
MIN_CAPS_ASSIGNMENTS = 20   # the sparsest capabilities() body assigns 35
MIN_CELLS = 200             # 61 rows x 6 backends = 366 today. A VACUITY floor,
                            # not a roster check: deleting a row keeps the run green.
                            # See the document's "roster is not guarded" section.


# ---- source parsing ----------------------------------------------------------

def strip_comments(text: str) -> str:
    """Comment-free text. Braces and identifiers in comments are not code."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def _balanced(text: str, open_index: int) -> tuple[str, int]:
    """Body between the brace at open_index and its match, plus the end index."""
    depth = 0
    i = open_index
    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[open_index + 1:i], i
        i += 1
    raise SystemExit("check_radio_feature_matrix: unbalanced braces while scanning")


def _skip_parens(text: str, open_index: int) -> int:
    """Index just past the ')' matching the '(' at open_index.

    A parser that scanned for a bare ')' broke on every seam verb carrying a
    default argument — `const Completion& completion = {}` — and silently lost
    setKeying, setTune and setAtu from the Flex and Icom override sets. Every
    one of those backends then looked like it implemented nothing.
    """
    depth = 0
    i = open_index
    while i < len(text):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    raise SystemExit("check_radio_feature_matrix: unbalanced parens while scanning")


def class_body(text: str, cls: str) -> str:
    """The body of `class <cls> ... { ... }`, comments already stripped."""
    m = re.search(r"\bclass\s+%s\b[^;{]*\{" % re.escape(cls), text)
    if not m:
        raise SystemExit(f"check_radio_feature_matrix: class {cls} not found")
    return _balanced(text, m.end() - 1)[0]


def seam_verbs(text: str) -> dict[str, tuple[str, str]]:
    """Every `virtual` on IRadioBackend -> (classification, body text).

    PURE    `= 0`; the compiler forces every backend to implement it.
    NOOP    the body is nothing but Q_UNUSED(...) — the intent is discarded.
    REFUSAL the body returns a failure the caller can see (false, a message).
    ALIAS   the body calls another seam verb.
    VALUE   the body returns a datum (nullptr, {}, 0) rather than refusing.
    """
    out: dict[str, tuple[str, str]] = {}
    for m in re.finditer(r"\bvirtual\b", text):
        j = m.end()
        # the declared name is the identifier immediately before the first '('
        paren = text.find("(", j)
        if paren < 0:
            continue
        semi = text.find(";", j)
        brace = text.find("{", j)
        if (semi >= 0 and semi < paren) or (brace >= 0 and brace < paren):
            continue
        k = paren - 1
        while k > j and text[k].isspace():
            k -= 1
        end = k + 1
        while k > j and (text[k].isalnum() or text[k] == "_"):
            k -= 1
        name = text[k + 1:end]
        if not name:
            continue
        rest = text[_skip_parens(text, paren):]
        if re.match(r"\s*(?:const\s*)?=\s*0\s*;", rest):
            out[name] = ("PURE", "")
            continue
        mm = re.match(r"\s*(?:const\s*)?\{", rest)
        if not mm:
            continue
        body = " ".join(_balanced(rest, mm.end() - 1)[0].split())
        out[name] = (classify_body(body, name), body)
    return out


def classify_body(body: str, name: str) -> str:
    residue = re.sub(r"Q_UNUSED\s*\([^)]*\)\s*;", "", body).strip()
    if not residue:
        return "NOOP"
    if re.fullmatch(r"return\s+(false|0|\{\s*\}|nullptr)\s*;", residue):
        # false/0 from a create/remove verb is a refusal the caller sees;
        # {} / nullptr from a query is a datum, not a refusal.
        return "REFUSAL" if re.match(r"(create|remove|apply|finish)", name) else "VALUE"
    if re.search(r"return\s+QStringLiteral|return\s+tr\(", residue):
        return "REFUSAL"
    if re.search(r"\bset[A-Z]\w*\s*\(", residue):
        return "ALIAS"
    return "OTHER"


def overrides_of(header_text: str, cls: str) -> set[str]:
    """Names declared `override` in this backend's own class body."""
    body = class_body(header_text, cls)
    found: set[str] = set()
    for m in re.finditer(r"\b([a-zA-Z_]\w*)\s*\(", body):
        name = m.group(1)
        if name in ("if", "for", "while", "switch", "return", "sizeof",
                    "Q_UNUSED", "emit", "connect", cls) or name.startswith("~"):
            continue
        tail = body[_skip_parens(body, m.end() - 1):][:80]
        if re.match(r"\s*(?:const\s+)?(?:noexcept\s+)?override\b", tail):
            found.add(name)
    return found


def capability_defaults(text: str) -> dict[str, str]:
    """Member -> its in-class initialiser, for direct members only.

    Deliberately NOT a bool count. tools/check_capability_records.py owns the
    frozen-71 ratchet and this file must never become a second, disagreeing
    authority on what a capability field is — it only needs each field's
    DEFAULT, because a field a backend never assigns is the phantom case.
    """
    m = re.search(r"\bstruct\s+RadioCapabilities\b[^;{]*\{", text)
    if not m:
        raise SystemExit("check_radio_feature_matrix: struct RadioCapabilities not found")
    body = _balanced(text, m.end() - 1)[0]
    statements: list[str] = []
    depth = 0
    buf = ""
    for ch in body:
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
        if ch == ";" and depth == 0:
            statements.append(buf)
            buf = ""
        else:
            buf += ch
    fields: dict[str, str] = {}
    for raw in statements:
        s = " ".join(raw.split())
        if "(" in s:
            continue
        s = re.sub(r"^(?:\[\[[^\]]*\]\]\s*|mutable\s+|static\s+|inline\s+)+", "", s)
        # `bool x = false` and `QString x{"…"}` are both in-class initialisers.
        mm = re.match(r"^[A-Za-z_][\w:<>,\s*&]*?\s+([A-Za-z_]\w*)\s*=\s*(.+)$", s)
        if mm:
            fields[mm.group(1)] = mm.group(2).strip()
            continue
        mm = re.match(r"^[A-Za-z_][\w:<>,\s*&]*?\s+([A-Za-z_]\w*)\s*\{(.*)\}$", s)
        if mm:
            fields[mm.group(1)] = mm.group(2).strip()
            continue
        mm = re.match(r"^[A-Za-z_][\w:<>,\s*&]*?\s+([A-Za-z_]\w*)$", s)
        if mm:
            fields[mm.group(1)] = ""
    return fields


def capability_assignments(text: str, cls: str) -> dict[str, list[str]]:
    """Field -> every right-hand side assigned in this backend's capabilities().

    More than one distinct value means the body decides at runtime. Icom builds
    almost its whole record from a per-model profile, so this is the normal
    case there rather than an anomaly.
    """
    m = re.search(r"RadioCapabilities\s+%s::capabilities\s*\(\s*\)\s*const\s*\{"
                  % re.escape(cls), text)
    if not m:
        raise SystemExit(f"check_radio_feature_matrix: {cls}::capabilities() not found")
    body = _balanced(text, m.end() - 1)[0]
    out: dict[str, list[str]] = {}
    for mm in re.finditer(r"\b(?:c|caps)\.([A-Za-z_]\w*)\s*=\s*([^;]+);", body):
        out.setdefault(mm.group(1), []).append(" ".join(mm.group(2).split()))
    return out


# TRUE / FALSE / DYNAMIC / DEFAULT_TRUE / DEFAULT_FALSE / UNKNOWN
def effective(field: str, assigns: dict[str, list[str]], defaults: dict[str, str]) -> str:
    values = assigns.get(field)
    if values is None:
        if field not in defaults:
            return "UNKNOWN"
        return "DEFAULT_TRUE" if truthy(defaults[field]) else "DEFAULT_FALSE"
    distinct = set(values)
    if len(distinct) == 1:
        only = distinct.pop()
        if only == "true":
            return "TRUE"
        if only == "false":
            return "FALSE"
        if re.fullmatch(r"-?\d+", only):
            return "TRUE" if int(only) > 0 else "FALSE"
        # A qualified enumerator is a static claim, not a runtime expression.
        if re.fullmatch(r"\w+(?:::\w+)+", only):
            return "TRUE" if truthy(only) else "FALSE"
        return "DYNAMIC"
    return "DYNAMIC"


NEGATIVE_ENUMERATORS = ("::Hidden", "::None", "::Unknown", "::Off")


def truthy(literal: str) -> bool:
    if literal == "true":
        return True
    if re.fullmatch(r"-?\d+", literal):
        return int(literal) > 0
    # An enumerator naming absence is a negative claim even though it is a
    # non-empty token. FmTonePresentation::Hidden is the live case: the tone
    # controls are gated on this enum, not on hasFmRepeaterOffset, and reading
    # it as "set, therefore true" would score four FM rows wrong on two radios.
    if any(literal.endswith(tail) for tail in NEGATIVE_ENUMERATORS):
        return False
    # A non-empty QString/name default (cwTextKeyerName) is a positive claim.
    return bool(literal) and literal != "false"


def gate_census(fields: set[str]) -> dict[str, int]:
    """How many files above the seam read each capability field.

    Zero means the record claims a gate the GUI does not apply — which is the
    #5859 shape, a capability nothing consults, and the row is then describing
    a gate that is not there.
    """
    counts = dict.fromkeys(fields, 0)
    for root in GATE_DIRS:
        for path in sorted(root.rglob("*")):
            if path.suffix not in (".cpp", ".h"):
                continue
            text = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
            for field in fields:
                if re.search(r"\b%s\b" % re.escape(field), text):
                    counts[field] += 1
    return counts


# ---- derivation --------------------------------------------------------------

class Source:
    def __init__(self) -> None:
        self._site_text: dict[str, str] = {}
        for path in (SEAM_HEADER, CAPS_HEADER, RADIO_MODEL):
            if not path.exists():
                raise SystemExit(f"check_radio_feature_matrix: {path} not found")
        self.seam = seam_verbs(strip_comments(SEAM_HEADER.read_text(encoding="utf-8")))
        self.defaults = capability_defaults(strip_comments(CAPS_HEADER.read_text(encoding="utf-8")))
        self.radio_model = strip_comments(RADIO_MODEL.read_text(encoding="utf-8"))
        base = REPO / "src" / "core" / "backends"
        self.overrides: dict[str, set[str]] = {}
        self.assigns: dict[str, dict[str, list[str]]] = {}
        self.bodies: dict[str, str] = {}
        for fam, (cls, header, unit, _display) in BACKENDS.items():
            htext = strip_comments((base / header).read_text(encoding="utf-8"))
            ctext = strip_comments((base / unit).read_text(encoding="utf-8"))
            self.overrides[fam] = overrides_of(htext, cls)
            self.assigns[fam] = capability_assignments(ctext, cls)
            self.bodies[fam] = htext + "\n" + ctext

    def vacuity(self) -> list[str]:
        problems = []
        if len(self.seam) < MIN_SEAM_VIRTUALS:
            problems.append(f"only {len(self.seam)} virtual(s) found on IRadioBackend "
                            f"(floor {MIN_SEAM_VIRTUALS})")
        if len(self.defaults) < MIN_CAPS_FIELDS:
            problems.append(f"only {len(self.defaults)} RadioCapabilities member(s) parsed "
                            f"(floor {MIN_CAPS_FIELDS})")
        for fam in BACKEND_ORDER:
            if len(self.overrides[fam]) < MIN_OVERRIDES_PER_BACKEND:
                problems.append(f"{fam}: only {len(self.overrides[fam])} override(s) found "
                                f"(floor {MIN_OVERRIDES_PER_BACKEND})")
            if len(self.assigns[fam]) < MIN_CAPS_ASSIGNMENTS:
                problems.append(f"{fam}: capabilities() assigns only "
                                f"{len(self.assigns[fam])} field(s) "
                                f"(floor {MIN_CAPS_ASSIGNMENTS})")
        return problems

    def gate_state(self, fields: list[str], fam: str) -> str:
        """HIDDEN / PERMISSIVE / OPEN for one row's gate on one backend.

        OR semantics, because that is what the GUI does: VfoWidget's noise
        blanker gate is `m_hasRadioSideDsp || m_hasHostNoiseBlanker`, so a
        radio that declines the first and declares the second keeps the button.

        THE THREE STATES ANSWER TWO DIFFERENT QUESTIONS, and conflating them is
        the mistake this comment exists to prevent.

          * VALUE decides whether the control is on screen at all. HIDDEN means
            every gating field is effectively false — and it does not matter
            whether the backend wrote that false or inherited it, because the
            operator sees the same nothing either way. That is `H`.
          * PROVENANCE decides P against D, and ONLY provenance. PERMISSIVE is
            `DEFAULT_TRUE`, which by construction means THE BACKEND NEVER
            ASSIGNED THE FIELD: `effective()` returns DEFAULT_* only when the
            capabilities() body carries no assignment at all. An explicit
            assignment — to any value, permissive or not — yields TRUE or
            DYNAMIC, never DEFAULT_TRUE, so it can never produce a P.

        Stated as the question a contributor should ask: DID THIS BACKEND ASK
        FOR THIS CONTROL? If it asked and did not implement it, that is a bug
        in that backend and the cell is D. If it never mentioned the field and
        a permissive default offered the control on its behalf, that is a gap
        in the default and the cell is P — repairable with one assignment.

        The live pair makes the distinction concrete and they are one row apart.
        FmTonePresentation defaults to `Hidden` and `Legacy` is what turns the
        tone controls ON; HL2 assigns `Legacy` EXPLICITLY, so it asked, and its
        unimplemented tone verbs are D. hasFmRepeaterOffset defaults to `true`
        and HL2 never mentions it, so it never asked, and the repeater rows are
        P. Reading the ENUM VALUE as the discriminator would get both right here
        by luck and be wrong on the next backend that writes a permissive value
        down on purpose.
        """
        if not fields:
            return "OPEN"
        states = [effective(f, self.assigns[fam], self.defaults) for f in fields]
        if all(s in ("FALSE", "DEFAULT_FALSE") for s in states):
            return "HIDDEN"
        if any(s == "DEFAULT_TRUE" for s in states):
            return "PERMISSIVE"
        return "OPEN"

    def override_is_empty(self, fam: str, verb: str) -> bool | None:
        """True when the override's body is nothing but Q_UNUSED, None if unseen."""
        text = self.bodies[fam]
        cls = BACKENDS[fam][0]
        for pattern in (r"\b%s::%s\s*\(" % (re.escape(cls), re.escape(verb)),
                        r"\b%s\s*\(" % re.escape(verb)):
            for m in re.finditer(pattern, text):
                after = _skip_parens(text, m.end() - 1)
                mm = re.match(r"\s*(?:const\s+)?(?:noexcept\s+)?(?:override\s*)?\{",
                              text[after:])
                if not mm:
                    continue
                body = " ".join(_balanced(text[after:], mm.end() - 1)[0].split())
                return classify_body(body, verb) == "NOOP"
        return None

    def emits_wire(self, fam: str, wire: str) -> bool:
        """Does this backend's OWN source carry the literal wire text?

        Comment-free — self.bodies is already stripped — so a wire token
        mentioned in a FlexAPI comment above a function does not count as the
        backend emitting it.
        """
        return bool(wire) and wire in self.bodies[fam]

    def names_symbol(self, rel: str, symbol: str) -> bool:
        """Does repo file `rel` still contain `symbol` outside a comment?

        The anti-rot half of an authored `hide` block, the same shape as the
        alt_path check: a classification that names a site the code has left
        is a claim about a control nobody can find. Comment-free, so a symbol
        surviving only in a comment above a migrated gate does not count.
        """
        if not rel or not symbol:
            return False
        path = (REPO / rel).resolve()
        if REPO.resolve() not in path.parents or not path.is_file():
            return False
        if rel not in self._site_text:
            self._site_text[rel] = strip_comments(path.read_text(encoding="utf-8"))
        return symbol in self._site_text[rel]

    def publishes(self, fam: str, token: str) -> bool:
        """Does this backend ever mention a runtime-published control descriptor?

        Some controls carry no capability field and are gated on a LIST the
        backend publishes: an empty preamp label list means the radio has no
        preamp and the control does not appear. IRadioBackend.h says so in as
        many words — "An EMPTY list means the radio has no such stage and the
        control does not appear; that is the default for every backend". A
        backend whose sources never name the signal cannot fill the list, so
        the control is hidden, and that is derivable without knowing anything
        about the widget.
        """
        return re.search(r"\b%s\b" % re.escape(token), self.bodies[fam]) is not None

    def unread_member(self, spec: dict) -> bool | None:
        """True when a member is written and never read anywhere in its scope.

        THE PHANTOM DERIVATION, and the one question beyond the four that this
        file answers. An override that stores the operator's value and emits a
        readback looks exactly like one that drives the hardware: the widget
        moves, the model commits, a readback confirms, and no sample changes.
        RtlSdrDdc's filter edges are the live instance — two atomics, two
        .store() calls, and no .load() in the tree.

        The check is symmetric on purpose. A record may only declare a phantom
        where the member is genuinely unread; the day someone wires it up, the
        read count rises, this returns False, the cell derives W instead of P,
        and the committed P fails the diff. The claim cannot rot in either
        direction.
        """
        scope = REPO / spec.get("scope", "")
        member = spec.get("member", "")
        if not member or not scope.exists():
            return None
        reads = 0
        seen = False
        paths = sorted(scope.rglob("*")) if scope.is_dir() else [scope]
        for path in paths:
            if path.suffix not in (".cpp", ".h"):
                continue
            for line in strip_comments(
                    path.read_text(encoding="utf-8", errors="replace")).splitlines():
                if not re.search(r"\b%s\b" % re.escape(member), line):
                    continue
                seen = True
                # A declaration or a store is a write; anything else reads it.
                if re.search(r"\b%s\s*(?:\.store\s*\(|=[^=])" % re.escape(member), line):
                    continue
                if re.search(r"[\w>:]\s+%s\s*[{;=]" % re.escape(member), line):
                    continue
                reads += 1
        if not seen:
            return None
        return reads == 0

    # ---- the alternate path, and why a broken one is TWO different facts -----
    #
    # RETIREMENT IS NOT ROT, AND CONFLATING THEM DISARMS THE CHECK. Every
    # alt_path in this register documents the same shape: a control that
    # reaches a Flex by SmartSDR wire text built ABOVE the seam. The M4
    # receive-control migration exists to delete exactly those bypasses — so
    # under a boolean "does the file still contain both", every SUCCESSFUL
    # migration raises feature-matrix-stale-alt-path. A reviewer who sees that
    # error on three green migrations in a row learns to wave it through, and
    # it is then worthless on the day it finds real drift.
    #
    # The distinguishing question is NOT "did the triple break" — it breaks
    # identically either way. It is: DID THE EVIDENCE GET WEAKER, OR DID A
    # WEAKER PIECE OF EVIDENCE STOP BEING NEEDED?
    #
    #   ROTTED   the record's claim is false and nothing stronger replaced it.
    #            Fails, and says which of the four conditions below refused.
    #   RETIRED  the bypass was deliberately deleted because the control moved
    #            BEHIND the seam. The cell's primary evidence is intact and is
    #            now the STRONGER kind — a compiler-checked override instead of
    #            a text match. Reported as progress; never fails.
    #
    # Four conditions, each ruling out one way the cheerful reading could be
    # wrong. All four must hold, or it is rot:
    #
    #   (a) THE TOKEN IS GONE FROM THE DECLARED FILE. A file that still emits
    #       the wire token still has the bypass; the record merely names the
    #       wrong symbol for it. That is a rename nobody recorded — rot.
    #   (b) THE BACKEND OVERRIDES THE ROW'S SEAM VERB, with a body that is not
    #       a Q_UNUSED no-op. This is "the primary evidence is intact", and it
    #       is a compiler-checked fact rather than another text match. It also
    #       means derive() never consulted this record — it returned at the
    #       override rung — which is what makes the retirement cell-neutral.
    #   (c) THE CELL STILL DERIVES W. The alt path existed to argue
    #       reachability. If the control stopped being reachable, nothing was
    #       retired; something was lost.
    #   (d) THE WIRE TOKEN IS STILL EMITTED FROM THE BACKEND'S OWN SOURCE.
    #       The positive half, and the one that catches a "migration" that
    #       moved the call and forgot to send anything: the bypass has to have
    #       been ABSORBED below the seam, not merely deleted.
    #
    # NO RECURSION, though (c) calls derive(): derive() consults an alt_path
    # only at the rung it reaches when the verb is NOT overridden, and (b)
    # has already returned ROTTED in that case.
    #
    # ONE CASE THIS CANNOT SEPARATE, said out loud rather than glossed. Where
    # the backend ALREADY overrode the verb and ALREADY carried the same wire
    # token below the seam — three records today: rx/filter, rx/agc-threshold
    # and rx/pan-center on Flex — mangling the above-seam literal yields a tree
    # indistinguishable from the migration, because it IS the same tree in
    # every respect this register scores: the cell is still true and the bypass
    # no longer carries that text. A perturbation aimed at this check therefore
    # has to be aimed at a LOAD-BEARING record — 37 of the 40 — where breaking
    # it really does change where the click ends.

    ALT_LIVE = "LIVE"
    ALT_RETIRED = "RETIRED"
    ALT_ROTTED = "ROTTED"

    def alt_path_state(self, rid: str, row: dict, fam: str,
                       alt: dict) -> tuple[str, str]:
        """LIVE / RETIRED / ROTTED for one declared alternate path."""
        path = REPO / alt.get("file", "")
        symbol = alt.get("symbol", "")
        wire = alt.get("wire", "")
        cls = BACKENDS[fam][0]
        if not path.is_file():
            return self.ALT_ROTTED, f"{alt.get('file')} is not a file in this tree"
        # Comment-free, so a wire token that survives only in a comment does
        # not keep the claim alive. All forty records pass either reading
        # today; this is the one that stays honest when one stops.
        text = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
        has_symbol = symbol in text
        has_wire = wire in text
        if has_symbol and has_wire:
            return self.ALT_LIVE, ""

        # (a)
        if has_wire:
            return self.ALT_ROTTED, (
                f"{path.name} still emits {wire!r}, so the bypass is still there and "
                f"only the symbol {symbol} is wrong — that is a rename nobody "
                f"recorded, not a retirement")
        # (b)
        verb = row.get("seam", "")
        if verb not in self.overrides.get(fam, set()):
            return self.ALT_ROTTED, (
                f"{cls} does not override {verb}, so this record was the only "
                f"evidence the control reaches the radio at all")
        if self.override_is_empty(fam, verb) is not False:
            return self.ALT_ROTTED, (
                f"{cls}::{verb} is not an override with a body this checker can read, "
                f"so nothing stronger replaced the record")
        # (c)
        derived, why = self.derive(rid, row, fam)
        if derived != "W":
            return self.ALT_ROTTED, (
                f"the cell now derives {derived} ({why}), not W — the control stopped "
                f"being reachable, so nothing was retired")
        # (d) — a PLAIN substring, not publishes(). publishes() anchors on \b
        # for an identifier, and a wire literal is not an identifier: "filt "
        # ends in a space and "agc_threshold=" in an equals sign, and the
        # character after each in the source is '%'. \b then never matches and
        # every relocation would be reported as a loss.
        if not self.emits_wire(fam, wire):
            return self.ALT_ROTTED, (
                f"{wire!r} is emitted from neither {alt.get('file')} nor {cls}'s own "
                f"source, so the wire text was not moved below the seam — it was lost")

        return self.ALT_RETIRED, (
            f"the bypass through {symbol} is gone from {alt.get('file')} and "
            f"{cls}::{verb} overrides the seam verb carrying {wire!r}")

    def derive(self, rid: str, row: dict, fam: str) -> tuple[str, str]:
        """(state, reason) for one cell. Never returns V."""
        verb = row["seam"]
        if verb not in self.seam:
            return "U", f"seam verb {verb} is not declared on IRadioBackend"
        base, _body = self.seam[verb]

        gate = self.gate_state(row.get("capability") or [], fam)

        # (1) the GUI gate hides the control entirely — the honest cell, and
        # it comes first because it is what the operator actually experiences.
        # A control that is not on screen is not a dead control.
        if gate == "HIDDEN":
            return "H", "every gating capability is false for this backend"

        # (1b) the same thing, gated on a list the backend publishes at runtime
        # rather than on a capability field. An empty list hides the control,
        # and a backend that never names the signal cannot fill the list.
        rt = row.get("runtime_gate") or {}
        if rt and not self.publishes(fam, rt["token"]):
            return "H", (f"backend never emits {rt['token']}, so the published list stays "
                         f"empty and the control does not appear")

        # (2) the model gate: the control IS on screen and the intent dies one
        # layer above the backend, so the backend's override table is
        # irrelevant — and invisible to any audit that starts at the seam.
        mg = row.get("model_gate") or {}
        if mg:
            # A list, with OR semantics, because RadioModel really does install
            # two independent forwards for one control: CW pitch reaches the
            # backend either on hasRadioSideCwKeyer (a radio-side keyer) or on
            # hostModulates (a host-demodulating radio whose BFO is engine
            # side). Testing only the first calls CW pitch dead on the HL2 and
            # on the ANAN, where it demonstrably works.
            states = [effective(f, self.assigns[fam], self.defaults)
                      for f in mg["capability"]]
            if all(s in ("FALSE", "DEFAULT_FALSE") for s in states):
                return ("R" if mg.get("visible") else "D",
                        f"every RadioModel gate ({', '.join(mg['capability'])}) "
                        f"is false for {fam}")

        # (3) an override that actually has a body — and, where the record says
        # so and the source agrees, an override whose value nothing consumes.
        if verb in self.overrides[fam]:
            empty = self.override_is_empty(fam, verb)
            if empty is True:
                return "D", "override body is a Q_UNUSED no-op"
            phantom = (row.get("phantom") or {}).get(fam)
            if phantom:
                unread = self.unread_member(phantom)
                if unread is None:
                    return "U", (f"the record claims {phantom.get('member')} is written and "
                                 f"never read, and the checker cannot find it at all")
                if unread:
                    return "P", (f"P/cached-readback: the backend ASKED for this control and "
                                 f"the override stores {phantom['member']}, which nothing in "
                                 f"{phantom['scope']} reads back. Real work repairs it")
                return "W", (f"{phantom['member']} is now read somewhere in "
                             f"{phantom['scope']}; the phantom claim no longer holds")
            return "W", "backend overrides the seam verb"

        # (4) a path that reaches the radio WITHOUT the seam, before the base
        # class gets a say. It has to come first: on a Flex most of these
        # controls never reach IRadioBackend at all — RadioModel builds its
        # SliceModel at two sites and the one that wires the seam intents is
        # guarded !m_flexBackend — so what the base body would have done is
        # not what happens.
        #
        # This is the WEAKEST evidence in the document and is marked `*` in the
        # matrix. An override is a compiler-checked fact; this is a file, a
        # symbol and a literal wire token that all have to still be there.
        alt = (row.get("alt_path") or {}).get(fam)
        if alt and self.alt_path_state(rid, row, fam, alt)[0] == self.ALT_LIVE:
            return "W", f"reached outside the seam via {alt['symbol']} ({alt['wire']!r})"

        # (5) what the base class does with the intent instead.
        if base == "PURE":
            return "U", ("base is pure virtual but no override was found — "
                         "this cannot compile, so the parser is wrong")
        if base == "REFUSAL":
            return "R", "base returns a refusal the caller can see"
        if base == "ALIAS":
            return "U", "base forwards to another seam verb; reachable but not this control"
        if base == "NOOP":
            if gate == "PERMISSIVE":
                return "P", ("P/inherited-default: the backend never mentions the gating "
                             "capability, so a permissive default offered the control on its "
                             "behalf — it never asked. One assignment repairs it")
            return "D", "no override; base body discards the intent"
        return "U", f"base body classified {base}"

    # ---- the headless surface, derived by a DIFFERENT method ------------------
    #
    # WHY IT CANNOT BE THE SAME METHOD, and this is the substantive point of
    # having two tables. The test that decides which derivation is honest is:
    # DOES ANYTHING REFUSE WHEN THE DECLARATION IS ABSENT?
    #
    #   In the GUI, no. The defaults are permissive (hasFmRepeaterOffset = true),
    #   the gates are hand-written and optional, and a control whose capability
    #   nobody declared is simply live. So a GUI cell must be derived from
    #   REACHABILITY — the override table — and a declaration is only a
    #   cross-check.
    #
    #   In aetherd, yes, and it is written into the target. ModelReceiveControlTarget
    #   opens every admission with `caps.<record> && known(authority)` and returns
    #   the typed error `capability.unavailable`; ModelSliceFrequencyTarget's
    #   validCoverage() does the same with a range. There the DECLARATION IS THE
    #   CONTRACT, and reading it is not a shortcut past the truth.
    #
    # The trap this avoids is a PHANTOM W: deriving the headless cell from the
    # backend override would report "works" for every verb the daemon never
    # exposes, because the override exists and nothing headless calls it. So a
    # feature with no aetherd method gets NO headless cell at all — it is listed
    # under "not on this surface" instead of being scored — and a feature whose
    # record is absent gets R, because the client is told no in a typed error.
    #
    # And the ranges are checked, not just the authority. Flex declares
    # sliceFrequencyControl = {Authority::Radio, 0, 0}: the authority is known
    # and validCoverage() still rejects it on minimumHz <= 0. A derivation that
    # read only the authority would print "works" for headless tuning on the one
    # family the daemon refuses it for.
    HEADLESS_RULES = {
        "sliceFrequencyControl": "coverage",
        "receiveModeControl": "optional",
        "receiveFilterControl": "optional",
        "receiveAudioControl": "optional",
        "receivePanCenterControl": "range",
        "receivePanBandwidthControl": "range",
        "canTransmit": "launch",
    }
    FREQUENCY_CEILING = 9_007_199_254 * 1_000_000  # SliceModel::kMaximumReportedFrequencyHz

    def aetherd_families(self) -> set[str]:
        """The families ModelRadioConnectionTarget::supports() will connect.

        Derived, never listed here: a family added to or dropped from that
        function must move this table, and an Icom is absent from it today —
        which is the single largest divergence between the two surfaces.
        """
        text = strip_comments(
            (REPO / "src" / "core" / "backends" / "ModelRadioConnectionTarget.cpp")
            .read_text(encoding="utf-8"))
        m = re.search(r"bool\s+supports\s*\([^)]*\)\s*const\s*override\s*\{", text)
        if not m:
            raise SystemExit("check_radio_feature_matrix: supports() not found")
        body = _balanced(text, m.end() - 1)[0]
        found = set()
        for fam, (cls, _h, _u, _d) in BACKENDS.items():
            if re.search(r'QStringLiteral\(\s*"%s"\s*\)' % re.escape(fam), body) \
                    or f"{cls}::familyName()" in body:
                found.add(fam)
        return found

    def record_state(self, field: str, fam: str) -> str:
        """ENGAGED / ABSENT / UNKNOWN for one aetherd control record."""
        values = self.assigns[fam].get(field)
        if values is None:
            # Never assigned: an optional stays disengaged and a plain record's
            # authority stays Unknown. Both are refusals at the target.
            return "ABSENT"
        text = " ".join(values)
        if "std::nullopt" in text or "Authority::Unknown" in text:
            return "ABSENT"
        if field == "canTransmit":
            return "ABSENT" if text.strip() == "false" else "UNKNOWN"
        if "Authority::Radio" not in text and "Authority::Engine" not in text:
            return "UNKNOWN"
        kind = self.HEADLESS_RULES[field]
        if kind == "optional":
            return "ENGAGED"
        bounds = [int(n.replace("'", "")) for n in re.findall(r"\b\d[\d']*\b", text)]
        if len(bounds) < 2:
            return "UNKNOWN"
        low, high = bounds[0], bounds[1]
        if low <= 0 or high < low:
            return "ABSENT"
        if kind == "coverage" and high > self.FREQUENCY_CEILING:
            return "ABSENT"
        return "ENGAGED"

    def derive_headless(self, rid: str, row: dict, fam: str) -> tuple[str, str]:
        head = row.get("headless") or {}
        if not head:
            return "", "aetherd exposes no method for this feature"
        if fam not in self.aetherd_families():
            return "R", ("ModelRadioConnectionTarget::supports() will not connect this "
                         "family; ControlService answers capability.unavailable")
        field = head.get("record", "")
        if field not in self.HEADLESS_RULES:
            return "U", f"no derivation rule for control record {field!r}"
        if field not in self.defaults:
            return "U", f"RadioCapabilities has no member {field}"
        state = self.record_state(field, fam)
        if field == "canTransmit":
            # The daemon builds its transmit service only under --allow-local-tx,
            # so a TX-capable radio's cell depends on how the process was
            # launched and not on the radio. ABSENT (canTransmit == false) is
            # still a confident refusal: the engine preflight declines whatever
            # the flag says.
            if state == "ABSENT":
                return "R", "canTransmit is false; the engine transmit preflight refuses"
            return "U", ("the daemon binds a transmit target only under --allow-local-tx, "
                         "so this cell depends on the launch and not on the radio")
        if state == "ENGAGED":
            return "W", f"{field} is declared with a known authority and a valid range"
        if state == "ABSENT":
            return "R", f"{field} is absent or out of range; the target returns capability.unavailable"
        return "U", f"{field} is assigned an expression this checker cannot evaluate"


# ---- the committed record ----------------------------------------------------

RECORD_KEYS = ("control", "seam", "capability", "model_gate", "absent_probe",
               "alt_path", "cells", "headless", "verified", "hide", "note")


def load_matrix() -> dict:
    if not MATRIX_JSON.exists():
        raise SystemExit(f"check_radio_feature_matrix: {MATRIX_JSON} not found")
    data = json.loads(MATRIX_JSON.read_text(encoding="utf-8"))
    if not isinstance(data, dict) or not data:
        raise SystemExit("check_radio_feature_matrix: matrix JSON is not a non-empty object")
    return data


def doc_cells() -> tuple[dict[str, dict[str, str]], dict[str, dict[str, str]]]:
    """(gui, headless) cells read back out of the markdown tables.

    The document is the half people read, so it is the half that can go quietly
    wrong. A pipe table is parsed when its first header cell is `feature` (the
    GUI matrix) or `headless` (the daemon matrix); the rest of the document is
    prose and is not the checker's business.
    """
    if not MATRIX_DOC.exists():
        raise SystemExit(f"check_radio_feature_matrix: {MATRIX_DOC} not found")
    gui: dict[str, dict[str, str]] = {}
    headless: dict[str, dict[str, str]] = {}
    columns: list[str] | None = None
    target: dict[str, dict[str, str]] | None = None
    display = {d: f for f, (_c, _h, _u, d) in BACKENDS.items()}
    for line in MATRIX_DOC.read_text(encoding="utf-8").splitlines():
        if not line.startswith("|"):
            columns = None
            target = None
            continue
        parts = [p.strip() for p in line.strip().strip("|").split("|")]
        if not parts:
            continue
        head = parts[0].lower()
        if head in ("feature", "headless"):
            columns = [display.get(p) for p in parts[1:]]
            target = gui if head == "feature" else headless
            continue
        if target is None or columns is None or set(parts[0]) <= set("-: "):
            continue
        # The feature cell may carry the row-level ∅ mark beside the id, so
        # take the backtick-quoted token rather than the whole cell.
        name = re.match(r"\s*`([^`]+)`", parts[0])
        rid = name.group(1) if name else parts[0].strip("`")
        for col, raw in zip(columns, parts[1:]):
            if col is None:
                continue
            code = re.sub(r"[^WDPRHMUV∅]", "", raw)
            target.setdefault(rid, {})[col] = code[:1] if code else raw
    return gui, headless


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--strict", action="store_true",
                    help="exit 1 when a committed cell disagrees with the source")
    ap.add_argument("--print", dest="dump", action="store_true",
                    help="print the derived matrix and exit, deriving nothing else")
    args = ap.parse_args()

    src = Source()
    problems = src.vacuity()
    if problems:
        for p in problems:
            print(f"::error file=tools/check_radio_feature_matrix.py,"
                  f"title=feature-matrix-vacuity::{p}. That is too little to be a real "
                  f"tree and is almost certainly a PARSE FAILURE. DO NOT lower the floor "
                  f"to match: a generator that finds nothing derives every cell the same "
                  f"way and the diff then passes on a matrix that has stopped meaning "
                  f"anything.")
        print(f"radio-feature-matrix: {len(problems)} vacuity failure(s) — parser broken")
        return 1

    matrix = load_matrix()

    if args.dump:
        print(f"# aetherd connects: {' '.join(sorted(src.aetherd_families()))}")
        for rid, row in matrix.items():
            gui = " ".join(f"{f}={src.derive(rid, row, f)[0]}" for f in BACKEND_ORDER)
            head = " ".join(f"{f}={src.derive_headless(rid, row, f)[0] or '-'}"
                            for f in BACKEND_ORDER)
            print(f"{rid:26s} GUI  {gui}")
            if row.get("headless"):
                print(f"{'':26s} HEAD {head}")
        return 0

    errors: list[str] = []
    notices: list[str] = []
    retired: list[str] = []
    tally: dict[str, int] = {}
    reasons: dict[str, list[str]] = {}
    grounds: dict[str, list[str]] = {}
    cell_count = 0
    hidden_rows: set[str] = set()
    hide_forms: dict[str, list[str]] = {}

    named_caps: set[str] = set()
    for row in matrix.values():
        named_caps.update(row.get("capability") or [])
        mg = row.get("model_gate") or {}
        if mg:
            named_caps.update(mg["capability"])
    census = gate_census(named_caps) if named_caps else {}

    for rid, row in matrix.items():
        where = f"docs/architecture/radio-feature-matrix.json"

        missing = [k for k in RECORD_KEYS if k not in row]
        if missing:
            errors.append(f"::error file={where},title=feature-matrix-record::"
                          f"{rid} is missing record key(s) {', '.join(missing)}. Every "
                          f"record carries the same key set so the shape is one question, "
                          f"not sixty.")
            continue

        if row["seam"] not in src.seam:
            errors.append(f"::error file={where},title=feature-matrix-stale-seam::"
                          f"{rid} names seam verb {row['seam']}, which no longer exists on "
                          f"IRadioBackend. The verb was renamed or removed; re-derive the "
                          f"row rather than deleting it, because the control probably "
                          f"still exists.")

        # A named capability must exist, and something above the seam must read
        # it. A gate nobody applies is the #5859 shape and the row is fiction.
        for field in (row.get("capability") or []):
            if field not in src.defaults:
                errors.append(f"::error file={where},title=feature-matrix-stale-capability::"
                              f"{rid} gates on RadioCapabilities::{field}, which is not a "
                              f"member of that struct.")
            elif census.get(field, 0) == 0:
                errors.append(f"::error file={where},title=feature-matrix-ungated::"
                              f"{rid} claims the GUI gates on {field}, but no file under "
                              f"src/gui or src/models reads it. Either the gate was "
                              f"removed — in which case the cells are now wrong — or the "
                              f"row never had one.")

        mg = row.get("model_gate") or {}
        if mg:
            for field in mg["capability"]:
                if f"backendCapabilities().{field}" not in src.radio_model:
                    errors.append(f"::error file={where},title=feature-matrix-stale-model-gate::"
                                  f"{rid} claims RadioModel tests "
                                  f"backendCapabilities().{field} before forwarding, and it "
                                  f"no longer does. The intent now reaches the backend (or "
                                  f"dies somewhere else); re-derive the row.")
            if mg.get("symbol") and mg["symbol"] not in src.radio_model:
                errors.append(f"::error file={where},title=feature-matrix-stale-model-gate::"
                              f"{rid} names RadioModel symbol {mg['symbol']} as the gated "
                              f"forward, and it is gone from RadioModel.cpp.")

        # The ∅ claim — "no capability field exists for this at all" — is a
        # claim about ABSENCE, which is exactly the claim that rots silently
        # when someone adds the field. Probing named candidates makes it fail
        # loudly instead.
        for probe in (row.get("absent_probe") or []):
            if probe in src.defaults:
                errors.append(f"::error file={where},title=feature-matrix-absent-probe::"
                              f"{rid} is marked ∅ (no capability field exists) but "
                              f"RadioCapabilities::{probe} now does. Re-derive the row and "
                              f"drop the ∅ — a control that gained a gate is no longer in "
                              f"the class this mark describes.")

        for fam, alt in (row.get("alt_path") or {}).items():
            alt_state, alt_why = src.alt_path_state(rid, row, fam, alt)
            if alt_state == Source.ALT_ROTTED:
                errors.append(f"::error file={where},title=feature-matrix-stale-alt-path::"
                              f"{rid}/{fam} declares an alternate path through "
                              f"{alt.get('file')}::{alt.get('symbol')} carrying "
                              f"{alt.get('wire')!r}, and that file no longer contains both. "
                              f"This is ROT, not a retirement: {alt_why}. An alternate "
                              f"path is the weakest evidence in this document; it must "
                              f"not be allowed to rot into a claim.")
            elif alt_state == Source.ALT_RETIRED:
                retired.append(f"{rid}/{fam}")
                notices.append(
                    f"::notice file={where},title=feature-matrix-alt-path-retired::"
                    f"{rid}/{fam} RETIRED its alternate path — {alt_why}. That is the "
                    f"seam migration landing, not drift: the cell is unchanged and now "
                    f"rests on a compiler-checked override instead of a wire literal. "
                    f"Drop the alt_path entry for {fam} from the sidecar and the `*` "
                    f"from that cell in radio-feature-matrix.md; this line repeats "
                    f"until someone does.")

        hide = row.get("hide") or {}
        hide_code = None
        if hide:
            hide_code = HIDE_GRANULARITY.get(hide.get("granularity"))
            if hide_code is None:
                errors.append(f"::error file={where},title=feature-matrix-hide-record::"
                              f"{rid} has hide.granularity {hide.get('granularity')!r}; it "
                              f"must be one of {', '.join(HIDE_GRANULARITY)}. The two are "
                              f"different verdicts under #5262 M3a, so there is no third.")
            if hide.get("form") not in HIDE_FORMS:
                errors.append(f"::error file={where},title=feature-matrix-hide-record::"
                              f"{rid} has hide.form {hide.get('form')!r}; it must be one of "
                              f"{', '.join(HIDE_FORMS)}.")
            if not hide.get("why"):
                errors.append(f"::error file={where},title=feature-matrix-hide-record::"
                              f"{rid} classifies its hidden cells without saying why.")
            if not hide.get("sites"):
                errors.append(f"::error file={where},title=feature-matrix-hide-record::"
                              f"{rid} classifies its hidden cells with no site. A "
                              f"classification nobody can locate cannot be checked.")
            for site in hide.get("sites") or []:
                if not src.names_symbol(site.get("file", ""), site.get("symbol", "")):
                    errors.append(f"::error file={where},title=feature-matrix-stale-hide-site::"
                                  f"{rid} says its control is gated at "
                                  f"{site.get('file')}::{site.get('symbol')}, and that file "
                                  f"no longer contains it outside a comment. The gate moved "
                                  f"or was migrated; re-read it and reclassify the row.")

        cells = row["cells"]
        if set(cells) != set(BACKEND_ORDER):
            errors.append(f"::error file={where},title=feature-matrix-columns::"
                          f"{rid} has cells for {sorted(cells)} — every row carries every "
                          f"backend, so a family that was added or retired is one edit "
                          f"everywhere rather than a hole here.")
            continue

        for fam in BACKEND_ORDER:
            cell_count += 1
            committed = cells[fam]
            derived, reason = src.derive(rid, row, fam)
            tally[committed] = tally.get(committed, 0) + 1
            if derived == "U":
                reasons.setdefault(reason, []).append(f"{rid}/{fam}")
            if derived == "P":
                grounds.setdefault(reason.split(":", 1)[0], []).append(f"{rid}/{fam}")

            if committed not in STATES:
                errors.append(f"::error file={where},title=feature-matrix-vocabulary::"
                              f"{rid}/{fam} is {committed!r}, which is not one of "
                              f"W D P R H M U V. The vocabulary is fixed; a cell that needs "
                              f"a new state needs the document's legend changed first.")
                continue

            # The generator says "hidden"; the authored record says at which
            # granularity, and so which of the two codes the cell must carry.
            if derived == "H":
                hidden_rows.add(rid)
                if hide_code is None:
                    if not hide:
                        errors.append(
                            f"::error file={where},title=feature-matrix-hide-unclassified::"
                            f"{rid}/{fam} derives hidden ({reason}) and the record has no "
                            f"`hide` block. Under #5262 M3a an applet hidden wholesale (H) "
                            f"and a single control hidden or dimmed without a reason (M, "
                            f"pending M3b) are different verdicts, so the record has to "
                            f"say which one this is, and where.")
                    continue
                derived = hide_code
                reason = f"{reason}; the hide record classifies it as {hide['granularity']}-level"
                if hide_code == "M":
                    hide_forms.setdefault(hide.get("form"), []).append(f"{rid}/{fam}")

            if committed == "V":
                # V is the one mark a static read cannot produce. Both halves
                # of the assertion matter: a citation, and agreement with the
                # source about reachability.
                cite = (row.get("verified") or {}).get(fam) or {}
                if not cite.get("run") or not cite.get("shows"):
                    errors.append(f"::error file={where},title=feature-matrix-uncited-v::"
                                  f"{rid}/{fam} is V (verified on real hardware) with no "
                                  f"run citation. V is never generated and never assumed: "
                                  f"it carries the run identifier it came from and one "
                                  f"line saying what that run showed, or it is not V.")
                elif derived != "W":
                    errors.append(f"::error file={where},title=feature-matrix-impossible-v::"
                                  f"{rid}/{fam} is V citing {cite['run']}, but the source "
                                  f"derives {derived} ({reason}). A cell cannot be verified "
                                  f"on hardware and unreachable in the same tree — either "
                                  f"the V is over-claimed or the code changed under it.")
                continue

            if committed != derived:
                errors.append(f"::error file={where},title=feature-matrix-drift::"
                              f"{rid}/{fam} is committed as {committed} and the source "
                              f"derives {derived} ({reason}). The matrix is generated and "
                              f"diffed, not hand-maintained: change the code, or correct "
                              f"the cell — a comment will not settle this.")

    for rid, row in matrix.items():
        if row.get("hide") and rid not in hidden_rows:
            errors.append(f"::error file=docs/architecture/radio-feature-matrix.json,"
                          f"title=feature-matrix-stale-hide-record::{rid} carries a `hide` "
                          f"classification and no cell of it derives hidden any more. The "
                          f"gate it describes is gone; delete the block rather than leave "
                          f"a verdict about a control that is now on screen.")

    # The markdown is the half people read.
    doc, doc_head = doc_cells()
    # doc_cells() keys its columns by FAMILY, having already resolved the
    # display name in the header; this map is only for the error text.
    display = {f: d for f, (_c, _h, _u, d) in BACKENDS.items()}
    for rid, row in matrix.items():
        if rid not in doc:
            errors.append(f"::error file=docs/architecture/radio-feature-matrix.md,"
                          f"title=feature-matrix-doc-missing::{rid} is in the JSON sidecar "
                          f"and in no table of the document. The sidecar may be richer than "
                          f"the doc, but it may not carry a row the reader never sees.")
            continue
        for fam in BACKEND_ORDER:
            want = row["cells"][fam]
            got = doc[rid].get(fam)
            if got != want:
                errors.append(f"::error file=docs/architecture/radio-feature-matrix.md,"
                              f"title=feature-matrix-doc-drift::{rid}/{display[fam]} reads "
                              f"{got!r} in the document and {want!r} in the sidecar. The "
                              f"document is what an operator is handed; it does not get to "
                              f"disagree.")
    for rid in doc:
        if rid not in matrix:
            errors.append(f"::error file=docs/architecture/radio-feature-matrix.md,"
                          f"title=feature-matrix-doc-orphan::{rid} is a table row in the "
                          f"document with no record in the JSON sidecar, so nothing "
                          f"derives it and nothing can catch it going stale.")

    # The headless table is a SUBSET by design — a feature aetherd does not
    # expose is listed in prose, never scored — so the check runs both ways:
    # every scored row must have a record, and no row with a record may be
    # missing from the table.
    for rid, row in matrix.items():
        head = row.get("headless") or {}
        if head and rid not in doc_head:
            errors.append(f"::error file=docs/architecture/radio-feature-matrix.md,"
                          f"title=feature-matrix-headless-missing::{rid} has an aetherd "
                          f"method ({head.get('method')}) in the sidecar and no row in the "
                          f"headless table.")
        if not head and rid in doc_head:
            errors.append(f"::error file=docs/architecture/radio-feature-matrix.md,"
                          f"title=feature-matrix-headless-orphan::{rid} is scored in the "
                          f"headless table and the sidecar gives it no aetherd method. A "
                          f"feature the daemon does not expose is NOT unsupported — it is "
                          f"absent, and scoring it invents a refusal nobody makes.")
        for fam in BACKEND_ORDER:
            if not head:
                continue
            want = (head.get("cells") or {}).get(fam)
            derived, reason = src.derive_headless(rid, row, fam)
            if want != derived:
                errors.append(f"::error file={where},title=feature-matrix-headless-drift::"
                              f"{rid}/{fam} is committed headless as {want} and the control "
                              f"records derive {derived} ({reason}). The headless cells come "
                              f"from the capability RECORD, because aetherd refuses when one "
                              f"is absent — that derivation is reading the contract, not a "
                              f"shortcut past it.")
            got = doc_head.get(rid, {}).get(fam)
            if got != want:
                errors.append(f"::error file=docs/architecture/radio-feature-matrix.md,"
                              f"title=feature-matrix-doc-drift::{rid}/{display[fam]} reads "
                              f"{got!r} in the headless table and {want!r} in the sidecar.")

    if cell_count < MIN_CELLS:
        print(f"::error file=docs/architecture/radio-feature-matrix.json,"
              f"title=feature-matrix-vacuity::only {cell_count} cell(s) were checked "
              f"against a floor of {MIN_CELLS}. A matrix that shrank this far is a "
              f"truncated file or a broken parser, not a simplification. DO NOT lower "
              f"MIN_CELLS to match.")
        print(f"radio-feature-matrix: {cell_count} cell(s) — below the vacuity floor")
        return 1

    for line in errors:
        print(line)
    for line in notices:
        print(line)

    shape = " ".join(f"{k}={tally.get(k, 0)}" for k in "WDPRHMUV")
    if grounds:
        print("radio-feature-matrix: the P cells, by provenance — the split is the "
              "repair, not a nuance:")
        for ground, where in sorted(grounds.items()):
            print(f"  {len(where):3d}  {ground}  ({', '.join(where)})")
    print(f"radio-feature-matrix: the hidden cells, by granularity (#5262 M3a) — "
          f"H applet-level {tally.get('H', 0)}, M per-control pending M3b "
          f"{tally.get('M', 0)}:")
    for form, where in sorted(hide_forms.items()):
        print(f"  {len(where):3d}  M/{form}")
    if retired:
        print(f"radio-feature-matrix: {len(retired)} alternate path(s) RETIRED — the "
              f"bypass is gone and the seam carries the control now. Progress, not "
              f"drift; the record should be deleted:")
        for cell in retired:
            print(f"       {cell}")
    print(f"radio-feature-matrix: {len(matrix)} feature(s) x {len(BACKEND_ORDER)} "
          f"backend(s) = {cell_count} cell(s); {shape}; "
          f"{len(retired)} alt path(s) retired; "
          f"{len(errors)} disagreement(s) with the source"
          + ("" if args.strict else " — would block under --strict"))
    # NOT gated on a clean run. A failing run is exactly when the U cells are
    # worth reading — an unexplained U is often the same parse failure that
    # produced the error above it — and hiding the breakdown behind "no errors"
    # withheld it from every run that needed it.
    if reasons:
        print("radio-feature-matrix: the generator declined to decide —")
        for reason, where in sorted(reasons.items()):
            print(f"  {len(where):3d}  {reason}")
    return 1 if (args.strict and errors) else 0


if __name__ == "__main__":
    sys.exit(main())
