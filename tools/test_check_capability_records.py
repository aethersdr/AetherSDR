#!/usr/bin/env python3
"""Parser regression checks for tools/check_capability_records.py (#5727).

WHY THIS FILE EXISTS. The ratchet's whole authority is
`direct_bool_fields()` being right about what a bool member is. It is a pure
text -> names function, so every interesting shape can be handed to it directly
— and #5727 was invisible for exactly as long as the parser was only ever run
against the one header that happened not to contain the shape. A check that
only sees today's tree cannot tell you it stopped seeing things.

The shape that broke it is the one the ratchet ASKS contributors to write: an
`std::optional<Record>` capability with an accessor beside it. Both directions
are covered below, and the second is the dangerous one:

  * a bool declared AFTER an accessor vanished from the count, so the tool
    reported a drop and advised lowering FROZEN_BOOL_COUNT to match — which
    would have given the field away permanently;
  * a bool ADDED after an accessor was never seen, so the population could grow
    past the freeze with the count flat. MAX_PLAUSIBLE_DROP cannot catch that
    one: nothing dropped.

AND THE SAME THING HAPPENED TO THE FIX (#5860 review, ten9876). The first
revision of the character scan lost `bool a{false}, b{true};` — a shape the
line parser it replaced got right. A parser PR that quietly drops a declarator
form is the defect it exists to fix, wearing the fix's clothes, so the shapes
below are written to cover both what the old parser missed and what the new one
could. The ones that would have caught it, had they existed:
`test_brace_initialised_comma_declarators_survive`,
`test_brace_in_a_string_literal_does_not_collapse_the_scan` and
`test_apostrophes_in_comments_do_not_eat_a_declaration`.

The runner DISCOVERS these rather than listing them, for the same reason: a
hand-maintained list silently skips a check that was never appended to it.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_capability_records  # noqa: E402

direct_bool_fields = check_capability_records.direct_bool_fields
HEADER = check_capability_records.HEADER


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def inject_above_declaration(text, name, *lines):
    """Insert `lines` immediately above the DECLARATION of `bool name`.

    Anchored on a line that BEGINS with the declaration, not on the first
    textual occurrence of `bool <name>` (#5860 review, ten9876). A
    `str.replace(f"bool {name}", …, 1)` is hijacked by any earlier mention of
    the name — a comment above the declaration is the obvious one — and the
    injection then lands inside that comment, where it is blanked, so the test
    passes vacuously instead of failing.
    """
    match = re.search(rf"^([ \t]*)bool\s+{re.escape(name)}\b", text, re.M)
    check(match is not None, f"no line-anchored declaration of `bool {name}`")
    indent = match.group(1)
    block = "".join(f"{indent}{line}\n" for line in lines)
    return text[:match.start()] + block + text[match.start():]


def test_plain_fields():
    names = direct_bool_fields("""
struct RadioCapabilities {
    bool alpha = false;
    int gap = 0;
    bool beta = true;
};
""")
    check(names == ["alpha", "beta"], f"plain fields: {names}")


def test_bool_after_single_line_accessor():
    """The reported case. A one-line accessor never leaves depth 1."""
    names = direct_bool_fields("""
struct RadioCapabilities {
    bool alpha = false;
    [[nodiscard]] bool someAccessor() const { return true; }
    bool beta = false;
};
""")
    check(names == ["alpha", "beta"],
          f"a bool after a one-line accessor must survive: {names}")


def test_issue_snippet():
    """#5727's isolated reproduction verbatim: `beta` used to be gone."""
    names = direct_bool_fields("""
struct RadioCapabilities {
    bool alpha = false;
    std::optional<PanAmplitudeModel> panAmplitude;
    [[nodiscard]] bool dbmAxisIsCalibrated() const
    { return !panAmplitude || panAmplitude->calibratedDbm; }
    bool beta = false;
    double gap = 0.0;
    bool delta = false;
};
""")
    check(names == ["alpha", "beta", "delta"], f"issue snippet: {names}")


def test_bool_after_multi_line_member_function():
    """The other direction: growth must be VISIBLE, not swallowed."""
    names = direct_bool_fields("""
struct RadioCapabilities {
    bool alpha = false;
    [[nodiscard]] double maxWattsAt(double hz) const noexcept
    {
        for (const TxPowerBand& band : txPowerBands) {
            if (hz >= band.lowHz) { return band.maxWatts; }
        }
        return txPowerMaxWatts;
    }

    bool probeAfterFn = false;
};
""")
    check(names == ["alpha", "probeAfterFn"],
          f"a bool added after a member function must be COUNTED: {names}")


def test_defaulted_and_deleted_members():
    names = direct_bool_fields("""
struct RadioCapabilities {
    RadioCapabilities() = default;
    RadioCapabilities(const RadioCapabilities&) = delete;
    bool operator==(const RadioCapabilities&) const = default;
    bool alpha = false;
};
""")
    check(names == ["alpha"],
          f"= default / = delete are not capability bools: {names}")


def test_multi_line_brace_initializer():
    """`agcModes = {…};` cost hasModeIndependentSquelch once (#5619)."""
    names = direct_bool_fields("""
struct RadioCapabilities {
    QStringList agcModes = {
        QStringLiteral("slow"),
        QStringLiteral("fast"),
    };
    bool hasModeIndependentSquelch = false;
};
""")
    check(names == ["hasModeIndependentSquelch"],
          f"the field after a multi-line initializer must survive: {names}")


def test_nested_types_are_excluded():
    """A bool inside a nested struct is not a direct member."""
    names = direct_bool_fields("""
struct RadioCapabilities {
    bool alpha = false;
    struct RxFilterPreset {
        bool notAField = false;
        bool operator==(const RxFilterPreset&) const = default;
    };
    enum class ClientSettingsDomain : quint32 {
        None = 0,
        Audio = 1,
    };
    bool beta = false;
};
""")
    check(names == ["alpha", "beta"], f"nested types must be excluded: {names}")


def test_brace_initialised_comma_declarators_survive():
    """#5860 review, finding 1 — a regression this PR introduced, now fixed.

    The first revision emitted the synthetic `;` on EVERY depth-1 `{`, so
    `bool a{false}, b{true};` scanned as `bool a; , b; ;` and `b` was deleted:
    growth with the count flat, the one direction a frozen number cannot see.
    The pre-#5727 line parser counts all three, so this was a shape `main`
    handled and the fix lost — introduced by nothing more than a reformat to
    brace initialisation. `test_wrapped_and_comma_declarations` below cannot
    see it: its comma case uses `=` initialisers only.
    """
    names = direct_bool_fields(
        "struct RadioCapabilities {\n"
        "    bool a{false}, b{true};\n"
        "    bool c = false;\n"
        "};\n")
    check(names == ["a", "b", "c"], f"brace-init comma declarators: {names}")


def test_brace_initialiser_does_not_need_a_terminator():
    """The other half of finding 1: initialisers must still be safe.

    `agcModes = {…};` gets no synthetic `;` now, and needs none — its interior
    is dropped at depth >= 2 and the real `;` after the closing brace ends the
    statement. Same for a nested type with a trailing declarator, an
    `enum class`, and a lambda with no parameter list.
    """
    names = direct_bool_fields(
        "struct RadioCapabilities {\n"
        "    QStringList agcModes = {\n"
        '        QStringLiteral("slow"),\n'
        "    };\n"
        "    struct Inner { bool notAField = false; } inner;\n"
        "    enum class Domain : quint32 { None = 0 };\n"
        "    auto fn = []{ return true; };\n"
        "    bool survivor = false;\n"
        "};\n")
    check(names == ["survivor"],
          f"initialisers must not need a synthetic terminator: {names}")


def test_wrapped_and_comma_declarations():
    """clang-format wrapping and comma declarators (#5619 re-review)."""
    names = direct_bool_fields("""
struct RadioCapabilities {
    bool
        wrapped = false;
    bool first = false; bool second = false;
    mutable bool qualified = false;
    bool comma_a = false, comma_b = false;
};
""")
    check(names == ["wrapped", "first", "second", "qualified",
                    "comma_a", "comma_b"], f"wrapped/comma forms: {names}")


def test_brace_in_a_string_literal_does_not_collapse_the_scan():
    """#5860 review, finding 2. A `{` in a literal raised the depth forever.

    Every field after it vanished, and the loss stays under MAX_PLAUSIBLE_DROP
    for a literal anywhere after the 56th of today's 71 bools — silent, and in
    the direction that hides growth. Pre-existing: the pre-#5727 parser returns
    [] here too.
    """
    names = direct_bool_fields(
        "struct RadioCapabilities {\n"
        '    QString marker = QStringLiteral("{");\n'
        "    bool alpha = false;\n"
        "    bool beta = false;\n"
        "};\n")
    check(names == ["alpha", "beta"], f"brace in a string literal: {names}")


def test_brace_in_a_character_literal_does_not_collapse_the_scan():
    names = direct_bool_fields(
        "struct RadioCapabilities {\n"
        "    char marker = '{';\n"
        "    bool alpha = false;\n"
        "};\n")
    check(names == ["alpha"], f"brace in a char literal: {names}")


def test_double_slash_inside_a_string_literal_is_not_a_comment():
    """The mirror of finding 2, reached through the comment pass.

    `QStringLiteral("http://x")` made the line-comment pattern blank the rest
    of the line, bool included. This is why literals and comments are resolved
    in ONE left-to-right alternation rather than two ordered passes.
    """
    names = direct_bool_fields(
        "struct RadioCapabilities {\n"
        '    QString url = QStringLiteral("http://x"); bool inline_after = false;\n'
        "    bool beta = false;\n"
        "};\n")
    check(names == ["inline_after", "beta"], f"`//` inside a literal: {names}")


def test_apostrophes_in_comments_do_not_eat_a_declaration():
    """The failure the OTHER pass order would have introduced.

    Blanking literals before comments reads the `'` pair in
    `/* it's */ bool alpha = false; /* that's */` as a character literal
    spanning the declaration, and deletes `alpha` — a NEW loss, on a shape both
    the pre-#5727 parser and the first revision of this fix get right. English
    possessives inside comments are everywhere in this header, so this is the
    common case and not the exotic one.
    """
    names = direct_bool_fields(
        "struct RadioCapabilities {\n"
        "    /* it's */ bool alpha = false; /* that's */\n"
        "    // the radio's own scale's business\n"
        "    bool beta = false;\n"
        "};\n")
    check(names == ["alpha", "beta"], f"apostrophes in comments: {names}")


def test_digit_separator_is_not_a_char_literal():
    """A C++14 digit separator must not open a character literal (#5860 review).

    The single alternation takes whichever construct STARTS first, and until a
    number was one of its alternatives the `'` in `1'000` started a "char
    literal" that ran to the next apostrophe — the possessive in the trailing
    comment. Everything between them was blanked, the `;` included, so the
    declaration merged into the following line and the bool declared there was
    DELETED from the count. The pre-#5727 line parser counts it, so this was a
    regression, in the quiet direction: a bool added there is growth the gate
    never sees.

    Neither half is exotic in this tree. Digit separators are already in it
    (`src/core/QsoWavPlayback.h`, `src/core/AnanBackend.cpp`) and this header
    already carries possessive apostrophes in trailing comments. One rate limit
    written with a separator, with an apostrophe after it, is all it takes.
    """
    names = direct_bool_fields(
        "struct RadioCapabilities {\n"
        "    int maxHz = 54'000'000; // radio's max\n"
        "    int minHz = 1'000; // it's low\n"
        "    bool alpha = false;\n"
        "    bool beta = false;\n"
        "};\n")
    check(names == ["alpha", "beta"], f"digit separator with apostrophes: {names}")

    # One separator, one apostrophe, one field: the minimal shape.
    names = direct_bool_fields(
        "struct RadioCapabilities {\n"
        "    int x = 10'000; // the radio's limit\n"
        "    bool alpha = false;\n"
        "};\n")
    check(names == ["alpha"], f"one separator, one possessive: {names}")

    # Two separators on one line pair with EACH OTHER and eat the field between
    # them, with no comment involved at all.
    names = direct_bool_fields(
        "struct RadioCapabilities {\n"
        "    int x = 1'000; bool alpha = false; int y = 2'0;\n"
        "};\n")
    check(names == ["alpha"], f"two separators on one line: {names}")

    # And the number branch must not swallow a real character literal whose
    # prefix ends in a digit: `u8'{'` still has to be lexed as one, or its
    # brace goes live and collapses the scan.
    names = direct_bool_fields(
        "struct RadioCapabilities {\n"
        "    char c = u8'{';\n"
        "    bool alpha = false;\n"
        "};\n")
    check(names == ["alpha"], f"u8'{{' is still a character literal: {names}")


def test_constructor_brace_init_list_does_not_delete_the_next_field():
    """`: a{false}, b{true} {}` must not eat the field after the constructor.

    The same class this PR closes, and the one shape of it still live on both
    parsers (#5860 review, ten9876). `_opens_a_body()` decides on the text
    since the last `;`, and the synthetic `;` emitted at `a{` erased the `(` of
    the constructor's own signature from that window — so the body brace was
    read as an initialiser, no terminator was emitted, and the `, b ` left over
    was glued to the front of the next field, deleting it.

    `: x(1), a{false} {}` already worked, because the `(` survived in the
    window; two BRACE initialisers were needed to lose it. That is the tell
    that the rule, not the shape, was wrong.
    """
    names = direct_bool_fields(
        "struct RadioCapabilities {\n"
        "    RadioCapabilities() : a{false}, b{true} {}\n"
        "    bool a;\n"
        "    bool b;\n"
        "    bool z = false;\n"
        "};\n")
    check(names == ["a", "b", "z"], f"constructor brace-init list: {names}")

    # A plain member function body followed by a brace-initialised comma
    # declarator must keep working — the regression the last round fixed.
    names = direct_bool_fields(
        "struct RadioCapabilities {\n"
        "    bool f() const { return true; }\n"
        "    bool a{false}, b{true};\n"
        "};\n")
    check(names == ["a", "b"], f"body then comma declarator: {names}")


def test_raw_string_with_an_unbalanced_brace_is_a_stated_limitation():
    """`R"(…)"` is not lexed as a raw string; an odd interior `"` mis-pairs.

    Pinned so a future change to it is deliberate, exactly like
    `bool x(false);`. The ordinary shapes survive because the quotes happen to
    pair up; this one does not, and the scan collapses from there.
    """
    names = direct_bool_fields(
        "struct RadioCapabilities {\n"
        '    QString raw = R"(a " { b)";\n'
        "    bool alpha = false;\n"
        "};\n")
    check(names == [], f"stated raw-string limitation changed: {names}")


def test_directive_carrying_a_multi_line_comment_does_not_eat_the_next_field():
    """Blanking a comment to its own newlines split the directive in two.

    The tail (` 1`) stayed live, glued onto the next fragment, and the field
    after it was rejected for the leading junk. Non-code is blanked to a single
    SPACE now; nothing here reads a line number. Pre-existing on the pre-#5727
    parser too.
    """
    names = direct_bool_fields(
        "struct RadioCapabilities {\n"
        "    bool alpha = false;\n"
        "#define SOMETHING /* a\n"
        " b */ 1\n"
        "    bool beta = false;\n"
        "};\n")
    check(names == ["alpha", "beta"], f"directive with a block comment: {names}")


def test_cv_and_constexpr_qualifiers_are_counted():
    """The qualifier list is an ENUMERATION, so it has to be widened (#5860).

    `const bool` / `volatile bool` / `constexpr static bool` were uncounted —
    under-counting, the direction that hides growth. A MACRO qualifier is still
    missed and stays a stated limitation: it is not knowable without a
    preprocessor.
    """
    names = direct_bool_fields(
        "struct RadioCapabilities {\n"
        "    const bool a = false;\n"
        "    volatile bool b = false;\n"
        "    constexpr static bool c = false;\n"
        "    mutable bool d = false;\n"
        "    bool e = false;\n"
        "};\n")
    check(names == ["a", "b", "c", "d", "e"], f"cv/constexpr qualifiers: {names}")


def test_macro_qualifier_is_a_stated_limitation():
    """`Q_DECL_DEPRECATED bool x` stays uncounted, deliberately and pinned."""
    names = direct_bool_fields(
        "struct RadioCapabilities {\n"
        "    Q_DECL_DEPRECATED bool macroQualified = false;\n"
        "    bool beta = false;\n"
        "};\n")
    check(names == ["beta"], f"stated macro-qualifier limitation changed: {names}")


def test_parenthesised_initialiser_is_a_stated_limitation():
    """`bool x(false);` stays uncounted — the most vexing parse, documented."""
    names = direct_bool_fields("""
struct RadioCapabilities {
    bool alpha(false);
    bool beta = false;
};
""")
    check(names == ["beta"], f"stated `(` limitation changed: {names}")


def test_real_header_accessor_does_not_move_the_count():
    """End to end, against the shipped header rather than a synthetic one.

    Pinned to no absolute number, so a real conversion that lowers
    FROZEN_BOOL_COUNT does not fail this: the assertion is that wrapping an
    existing bool in an accessor moves the count by exactly the bool added.
    """
    text = HEADER.read_text(encoding="utf-8")
    baseline = direct_bool_fields(text)
    check(baseline, "no bools found in the shipped RadioCapabilities.h")

    first = baseline[0]
    accessor = ("[[nodiscard]] bool synthetic5727Accessor() const "
                "{ return true; }")
    probe = "bool synthetic5727Probe = false;"

    # An accessor ABOVE the first bool used to delete that bool from the count.
    with_accessor = inject_above_declaration(text, first, accessor)
    names = direct_bool_fields(with_accessor)
    check(names == baseline,
          "an accessor changed the field list: "
          f"{sorted(set(baseline) ^ set(names))}")

    # A NEW bool below the accessor used to be invisible — the growth the
    # ratchet exists to stop, walked past with the count flat.
    with_growth = inject_above_declaration(text, first, accessor, probe)
    grown = direct_bool_fields(with_growth)
    check("synthetic5727Probe" in grown,
          "a bool added below an accessor is not counted — the ratchet is blind")
    check(len(grown) == len(baseline) + 1,
          f"expected {len(baseline) + 1} names, got {len(grown)}")


def test_access_label_does_not_delete_the_next_field():
    """`public:` ends a statement without a `;` (#5860).

    The split-on-`;` could not see it, so the label rode onto the front of the
    next fragment and the `^\\s*bool\\s+` match rejected it. OLD: the field
    after any access label was silently dropped.
    """
    names = direct_bool_fields("""
struct RadioCapabilities {
    bool alpha = false;
public:
    bool beta = false;
    bool gamma = false;
};
""")
    check(names == ["alpha", "beta", "gamma"], f"access label: {names}")


def test_preprocessor_region_does_not_delete_fields():
    """Every directive line used to cost the declaration after it (#5860).

    OLD returned ['alpha'] here: `#ifdef` ate hl2Only and `#endif` ate beta.
    Directives are blanked before the scan, so BOTH arms of an #if/#else are
    counted — deliberate. Evaluating the condition needs a preprocessor, and
    for a shrink-only ratchet over-counting is the fail-loud direction: it can
    make the gate fire, never let growth through.
    """
    names = direct_bool_fields("""
struct RadioCapabilities {
    bool alpha = false;
#ifdef HAVE_HL2
    bool hl2Only = false;
#endif
    bool beta = false;
};
""")
    check(names == ["alpha", "hl2Only", "beta"], f"preprocessor: {names}")


def test_directive_with_an_unbalanced_brace_does_not_swallow_the_struct():
    """A `{` inside a directive used to raise the depth and never return."""
    names = direct_bool_fields("""
struct RadioCapabilities {
    bool alpha = false;
#define SOMETHING_OPEN {
    bool beta = false;
};
""")
    check(names == ["alpha", "beta"], f"unbalanced directive: {names}")


def test_multi_line_block_comment_does_not_delete_the_next_field():
    """`/\\*.*?\\*/` was applied per PHYSICAL line, so it never matched a
    comment spanning lines; the interior prefixed the next fragment. Costs a
    field even when the comment is perfectly balanced."""
    names = direct_bool_fields("""
struct RadioCapabilities {
    bool alpha = false;
    /* plain
       multi line */
    bool beta = false;
};
""")
    check(names == ["alpha", "beta"], f"multi-line comment: {names}")


def test_real_header_label_growth_is_visible():
    """The dangerous direction, against the SHIPPED header.

    A bool added under an access label left the count EXACTLY at the frozen
    value, so the ratchet printed "ok (shrink only)" while the population had
    grown. MAX_PLAUSIBLE_DROP is blind to it because nothing dropped. This is
    the #5727 failure class reached through a different door, which is why it
    is guarded against the real file and not only a snippet.
    """
    text = HEADER.read_text(encoding="utf-8")
    baseline = direct_bool_fields(text)
    check(baseline, "no bools found in the shipped RadioCapabilities.h")
    grown = inject_above_declaration(
        text, baseline[0], "public:", "bool synthetic5860Probe = false;")
    names = direct_bool_fields(grown)
    check("synthetic5860Probe" in names,
          "a bool added under an access label is not counted — the ratchet is blind")
    check(len(names) == len(baseline) + 1,
          f"expected {len(baseline) + 1} names, got {len(names)}")


def test_real_header_anchor_is_not_hijacked_by_a_comment():
    """The anchoring is load-bearing, so it gets its own check (#5860 review).

    With a `str.replace(f"bool {name}", …, 1)` anchor, a comment mentioning the
    name above the declaration takes the injection, which is then blanked as a
    comment — the growth probe disappears and the assertion that used to catch
    a blind ratchet passes vacuously. Prepending exactly such a comment must
    leave the result unchanged.
    """
    text = HEADER.read_text(encoding="utf-8")
    baseline = direct_bool_fields(text)
    check(baseline, "no bools found in the shipped RadioCapabilities.h")
    first = baseline[0]

    decoyed = inject_above_declaration(
        text, first, f"// prose that names bool {first} before it is declared")
    check(direct_bool_fields(decoyed) == baseline,
          "the decoy comment itself moved the count")

    grown = inject_above_declaration(
        decoyed, first, "bool synthetic5860AnchorProbe = false;")
    names = direct_bool_fields(grown)
    check("synthetic5860AnchorProbe" in names,
          "the injection was hijacked by a comment mentioning the name")
    check(len(names) == len(baseline) + 1,
          f"expected {len(baseline) + 1} names, got {len(names)}")


if __name__ == "__main__":
    # DISCOVERED, NOT LISTED (#5860 review, ten9876). The hand-maintained call
    # list had both failure modes this PR exists to close: it stopped at the
    # first AssertionError, so one broken shape hid the rest — the body's
    # "four of the ten fail against `main`" was never something one run could
    # show — and a `test_` function added without being appended to it never
    # ran at all, silently. A test harness that skips a test is the same defect
    # class as a parser that stops seeing a field.
    checks = [fn for name, fn in sorted(globals().items())
              if name.startswith("test_") and callable(fn)]
    if not checks:
        raise SystemExit("capability-record parser: no checks discovered")

    failures = []
    for fn in checks:
        try:
            fn()
        except (Exception, SystemExit) as exc:  # report it and keep going
            failures.append(f"FAIL {fn.__name__}: {exc}")

    for line in failures:
        print(line)
    if failures:
        raise SystemExit(f"capability-record parser: {len(failures)} of "
                         f"{len(checks)} check(s) failed")
    print(f"capability-record parser checks passed ({len(checks)} checks)")
