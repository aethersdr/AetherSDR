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
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_capability_records  # noqa: E402

direct_bool_fields = check_capability_records.direct_bool_fields
HEADER = check_capability_records.HEADER


def check(condition, message):
    if not condition:
        raise AssertionError(message)


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
    anchor = f"bool {first}"
    check(text.count(anchor) >= 1, f"declaration of {first} not found")
    accessor = ("[[nodiscard]] bool synthetic5727Accessor() const "
                "{ return true; }\n    ")
    probe = "bool synthetic5727Probe = false;\n    "

    # An accessor ABOVE the first bool used to delete that bool from the count.
    with_accessor = text.replace(anchor, accessor + anchor, 1)
    names = direct_bool_fields(with_accessor)
    check(names == baseline,
          "an accessor changed the field list: "
          f"{sorted(set(baseline) ^ set(names))}")

    # A NEW bool below the accessor used to be invisible — the growth the
    # ratchet exists to stop, walked past with the count flat.
    with_growth = text.replace(anchor, accessor + probe + anchor, 1)
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
    anchor = f"bool {baseline[0]}"
    grown = text.replace(
        anchor, "public:\n    bool synthetic5860Probe = false;\n    " + anchor, 1)
    names = direct_bool_fields(grown)
    check("synthetic5860Probe" in names,
          "a bool added under an access label is not counted — the ratchet is blind")
    check(len(names) == len(baseline) + 1,
          f"expected {len(baseline) + 1} names, got {len(names)}")


if __name__ == "__main__":
    test_plain_fields()
    test_bool_after_single_line_accessor()
    test_issue_snippet()
    test_bool_after_multi_line_member_function()
    test_defaulted_and_deleted_members()
    test_multi_line_brace_initializer()
    test_nested_types_are_excluded()
    test_wrapped_and_comma_declarations()
    test_parenthesised_initialiser_is_a_stated_limitation()
    test_real_header_accessor_does_not_move_the_count()
    test_access_label_does_not_delete_the_next_field()
    test_preprocessor_region_does_not_delete_fields()
    test_directive_with_an_unbalanced_brace_does_not_swallow_the_struct()
    test_multi_line_block_comment_does_not_delete_the_next_field()
    test_real_header_label_growth_is_visible()
    print("capability-record parser checks passed")
