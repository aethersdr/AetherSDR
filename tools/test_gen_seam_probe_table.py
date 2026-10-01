#!/usr/bin/env python3
"""
Scanner tests for tools/gen_seam_probe_table.py.

The generator replaces a hand-maintained list with a parsed one, so the parser
is now the thing that can be wrong. Two ways it can be wrong quietly:

  * UNDER-reading — a real signal is missed, the probe line is never written,
    and the seam goes uncovered. That is #5825's failure returning by a new
    route. backend_seam_affinity_test still catches it against the
    meta-object, but only after the merge, which is the whole problem.

  * OVER-reading — an ordinary member function below `protected:`/`private:`
    is mistaken for a signal. IRadioBackend really has three of those
    (publishLegacyAudio, publishLegacySliceAudio, warnAudioDropped) sitting
    textually BELOW the signals section, and a scanner that stops at the
    class's closing brace instead of at the next access label takes all three.
    That one is loud — `&IRadioBackend::publishLegacyAudio` is not a signal,
    so the probe fails to compile — but loud at build time is still hours
    later than here.

The last test is the live one: the real header must yield exactly the table
committed in tests/SeamSignalProbeTable.inc.

    python3 tools/test_gen_seam_probe_table.py     # exits non-zero on any failure
"""

from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_seam_probe_table as gen  # noqa: E402


def check(condition, message):
    if not condition:
        raise AssertionError(message)


# The real header's shape in miniature: one signals: section, then protected:
# and private: sections whose members are ordinary inline functions.
SHAPED_LIKE_IRADIOBACKEND = """
#pragma once
class IRadioBackend : public QObject {
    Q_OBJECT
public:
    virtual bool isConnected() const = 0;
    virtual void invokeExtension(const QString& ns, quint64 id,
                                 const QVariant& arg = {}) = 0;

signals:
    void connected();
    void disconnected();
    // A range like {"OFF", "P.AMP1"} in prose, and the word signals: in prose.
    void connectionError(const QString& reason);
    void panCenterBandwidthChanged(const QString& panId,
                                   double centerMhz, double bandwidthMhz);
    void meterDefined(const QString& name,
                      const QString& unitSuffix = QStringLiteral(" dB"));
    void autoRfGainArmSettled(bool armed);

protected:
    void publishLegacyAudio(const QByteArray& pcm)
    {
        if (!m_pcmLive) {
            warnAudioDropped();
            return;
        }
        emit audioFrameReady(pcm);
    }
    bool publishLegacySliceAudio(int sliceId, const QByteArray& pcm)
    {
        void notASignalEither(int x);
        return sliceId >= 0;
    }

private:
    void warnAudioDropped()
    {
        qWarning() << "dropping";
    }
    bool m_pcmLive{false};
};
"""


def test_signals_section_ends_at_the_next_access_label():
    names = gen.seam_signals(SHAPED_LIKE_IRADIOBACKEND)
    check(names == ["connected", "disconnected", "connectionError",
                    "panCenterBandwidthChanged", "meterDefined",
                    "autoRfGainArmSettled"],
          f"unexpected signal list: {names}")
    for excluded in ("publishLegacyAudio", "publishLegacySliceAudio",
                     "warnAudioDropped", "notASignalEither",
                     "invokeExtension"):
        check(excluded not in names,
              f"{excluded} is not a signal and must not be probed")


def test_declaration_order_is_preserved():
    names = gen.seam_signals(SHAPED_LIKE_IRADIOBACKEND)
    check(names[0] == "connected" and names[-1] == "autoRfGainArmSettled",
          f"declaration order not preserved: {names}")


def test_qualified_and_qt_spellings_open_the_section():
    for label in ("signals:", "Q_SIGNALS:", "public signals:", "public Q_SIGNALS:"):
        src = "class B : public QObject { Q_OBJECT\npublic:\n  void notASignal();\n" \
              + label + "\n  void wanted();\n};\n"
        check(gen.seam_signals(src, "B") == ["wanted"],
              f"{label!r} did not open a signal section")


def test_slots_are_not_signals():
    src = """
class B : public QObject { Q_OBJECT
signals:
    void wanted();
public slots:
    void onThing();
private Q_SLOTS:
    void onOther();
};
"""
    check(gen.seam_signals(src, "B") == ["wanted"],
          "slots must not be probed — they are ordinary member functions")


def test_nested_scopes_cannot_close_or_open_the_section():
    # A nested type's own access labels live at depth 1 of the class body and
    # must be invisible: neither ending the outer signals section nor starting
    # one. Anything else and a helper struct silently truncates the table.
    src = """
class B : public QObject { Q_OBJECT
public:
    struct Helper {
    public:
        void notASignal();
    signals:
        void definitelyNotASignal();
    };
signals:
    void first();
    void second();
};
"""
    check(gen.seam_signals(src, "B") == ["first", "second"],
          "a nested type's access labels must not reach the outer class")


def test_comments_and_strings_cannot_forge_structure():
    src = '''
class B : public QObject { Q_OBJECT
signals:
    void first();
    /* private:
       void hidden(); */
    // private:
    void second();
    void third(const QString& s = QStringLiteral("private: { } void nope();"));
};
'''
    check(gen.seam_signals(src, "B") == ["first", "second", "third"],
          "a commented-out or quoted access label must not end the section")


def test_default_argument_pair_is_one_probe():
    # moc reports a default argument as TWO meta-methods off ONE declaration —
    # which is why declaredSeamSignals() calls removeDuplicates(). One C++
    # function, one probe line.
    src = """
class B : public QObject { Q_OBJECT
signals:
    void withDefault(int a, int b = 0);
    void plain();
};
"""
    check(gen.seam_signals(src, "B") == ["withDefault", "plain"],
          "a default argument is one declaration and one probe")


def test_genuine_overload_is_refused():
    # Two DECLARATIONS of one name is a real overload. AETHER_SEAM_PROBE takes
    # a name, and &B::twice is then ambiguous — the emitted line would not
    # compile. A flat table cannot express it, so refuse rather than bake in
    # an output that cannot build.
    src = """
class B : public QObject { Q_OBJECT
signals:
    void twice(int a);
    void twice(int a, int b);
};
"""
    try:
        gen.seam_signals(src, "B")
    except SystemExit as e:
        check("twice" in str(e), f"message should name the signal: {e}")
        check("ambiguous" in str(e), f"message should say why: {e}")
        return
    raise AssertionError("a genuine overload must be refused, not collapsed")


def test_conditional_compilation_in_the_signals_section_is_refused():
    # #5868's own caveat: the text parse is viable BECAUSE the section carries
    # no #if today. Silently reading one branch would produce a table that is
    # right for one build configuration and wrong for another, and --check
    # would then defend the wrong one. Refuse instead.
    src = """
class B : public QObject { Q_OBJECT
signals:
    void always();
#ifdef AETHERSDR_WITH_THING
    void sometimes();
#endif
};
"""
    try:
        gen.seam_signals(src, "B")
    except SystemExit as e:
        check("ifdef" in str(e), f"message should name the directive: {e}")
        check("signals" in str(e), f"message should name the section: {e}")
        return
    raise AssertionError("a #ifdef inside the signals section must be refused")


def test_conditional_compilation_elsewhere_is_fine():
    # Only the signals section is parsed, so guards around the include block or
    # inside an unrelated section must not trip the refusal.
    src = """
#ifndef GUARD
#define GUARD
class B : public QObject { Q_OBJECT
public:
#ifdef AETHERSDR_WITH_THING
    void aMethod();
#endif
signals:
    void wanted();
};
#endif
"""
    check(gen.seam_signals(src, "B") == ["wanted"],
          "a #ifdef outside the signals section must not be refused")


def test_missing_class_is_an_error_not_an_empty_table():
    # Silently returning [] here would render an empty table, and --check would
    # then demand it — turning a rename into "the seam has no signals".
    try:
        gen.seam_signals("class Other {};\n", "IRadioBackend")
    except SystemExit as e:
        check("IRadioBackend" in str(e), f"unhelpful message: {e}")
        return
    raise AssertionError("a missing class must raise, not return an empty list")


def test_live_header_matches_the_committed_table():
    names = gen.seam_signals(gen.BACKEND_H.read_text(errors="replace"))
    check(len(names) > 40,
          f"only {len(names)} signals parsed from the real header — "
          "the scanner is under-reading")
    for expected in ("connected", "disconnected", "audioFrameReady",
                     "autoRfGainArmSettled"):
        check(expected in names, f"{expected} missing from the live scan")
    for excluded in ("publishLegacyAudio", "publishLegacySliceAudio",
                     "warnAudioDropped"):
        check(excluded not in names,
              f"{excluded} is a protected/private member of IRadioBackend, "
              "not a signal")
    check(gen.render(names) == gen.PROBE_TABLE.read_text(),
          "tests/SeamSignalProbeTable.inc is stale — run "
          "`python tools/gen_seam_probe_table.py`")


# ── The under-read class. Each of these used to produce a SHORT table, and a
# short table is invisible to --check: the generator and the committed file
# agree, because both come from this parser. residue() turns the whole class
# into a refusal that names the construct.

def test_brace_initialised_default_argument_does_not_swallow_the_next_signal():
    # The `{` of a brace-init default argument opens a depth the declaration
    # splitter has to survive. Ignoring lines at depth > 0 loses the
    # terminating `;`, and the declaration then swallows the NEXT one — and
    # the regex takes the leftmost match, so it is the LATER signal that
    # vanishes. Silent, and exactly #5825's shape.
    src = """
class B : public QObject { Q_OBJECT
signals:
    void first();
    void withMap(const QVariantMap& opts = {
    });
    void afterIt();
    void lastOne();
};
"""
    check(gen.seam_signals(src, "B") == ["first", "withMap", "afterIt", "lastOne"],
          "a brace-initialised default argument must not swallow what follows")


def test_brace_initialised_default_argument_with_content():
    src = """
class B : public QObject { Q_OBJECT
signals:
    void withMap(const QVariantMap& opts = {
        {"a", 1}
    });
    void afterIt();
};
"""
    check(gen.seam_signals(src, "B") == ["withMap", "afterIt"],
          "braces with content must not swallow what follows")


def test_digit_separators_do_not_blank_the_declarations_between_them():
    # C++14 digit separators are apostrophes. Treating one as a char-literal
    # quote blanks everything up to the NEXT apostrophe — so two separated
    # defaults silently delete every declaration in between.
    src = """
class B : public QObject { Q_OBJECT
signals:
    void first(int rate = 48'000);
    void middleA();
    void middleB();
    void last(int rate = 96'000);
    void afterBoth();
};
"""
    check(gen.seam_signals(src, "B")
          == ["first", "middleA", "middleB", "last", "afterBoth"],
          "a digit separator is not a quote")


def test_char_literal_is_still_stripped():
    # The separator rule must not disarm genuine char literals: a brace inside
    # one would otherwise move the depth.
    src = """
class B : public QObject { Q_OBJECT
signals:
    void first(char c = '}');
    void second();
};
"""
    check(gen.seam_signals(src, "B") == ["first", "second"],
          "a char literal holding a brace must still be stripped")


def test_encoding_prefixed_char_literal_is_stripped_too():
    # THE COMPANION CASE, and the reason the one above was not enough. The
    # separator rule keys on the character before the quote, and an ENCODING
    # PREFIX puts an identifier character there just as a digit does. L'}' was
    # therefore left unstripped, the brace moved class_body()'s depth, the
    # class ended at `withChar`, and `second`/`third` disappeared with no
    # refusal — residue() cannot see them, because the section it scans was
    # truncated before it ran. That is the one under-read this file's whole
    # claim did not cover.
    #
    # A CLOSING brace is asserted because it is the silent direction: L'{'
    # merely leaves the class unterminated, which raises.
    for prefix in ("L", "u", "U", "u8"):
        src = ("class B : public QObject { Q_OBJECT\nsignals:\n"
               "    void first();\n"
               "    void withChar(char c = %s'}');\n"
               "    void second();\n"
               "    void third();\n};\n" % prefix)
        check(gen.seam_signals(src, "B")
              == ["first", "withChar", "second", "third"],
              f"{prefix}'}}' must be stripped like the unprefixed form")

    # And the separator it must not disarm, in all three bases, since the new
    # rule reads a RUN rather than one character.
    for literal in ("48'000", "0x1F'FF", "0b1'0"):
        src = ("class B : public QObject { Q_OBJECT\nsignals:\n"
               "    void first(int rate = %s);\n"
               "    void second();\n};\n" % literal)
        check(gen.seam_signals(src, "B") == ["first", "second"],
              f"{literal} is a digit separator, not a quote")


def test_declarator_list_is_refused():
    # `void a(), b();` is one declaration with two declarators.
    # SIGNAL_DECL_RE.search takes the first and residue()'s finditer never
    # sees a `void ` before `b(`, so `b` used to drop silently. moc almost
    # certainly would not accept this in a signals: section either — this
    # refuses rather than reads it, which is the same call the tool makes for
    # #if and for an overload.
    for decl in ("void a(), b();", "void a(int x), b();"):
        src = ("class B : public QObject { Q_OBJECT\nsignals:\n"
               "    void first();\n    " + decl + "\n    void second();\n};\n")
        try:
            gen.seam_signals(src, "B")
        except SystemExit as e:
            check("declarator list" in str(e), f"message should name it: {e}")
            continue
        raise AssertionError(f"{decl!r} must be refused, not half-read")


def test_trailing_return_type_is_refused():
    src = """
class B : public QObject { Q_OBJECT
signals:
    void first();
    auto second() -> void;
};
"""
    try:
        gen.seam_signals(src, "B")
    except SystemExit as e:
        check("trailing return" in str(e), f"message should name it: {e}")
        return
    raise AssertionError("a trailing return type must be refused, not skipped")


def test_q_signal_macro_is_refused_wherever_it_sits():
    # Q_SIGNAL's whole purpose is to declare a signal with NO signals: label,
    # so looking for it only inside one looks where it cannot be. Both
    # placements must refuse.
    inside = """
class B : public QObject { Q_OBJECT
signals:
    void first();
    Q_SIGNAL void viaMacro();
};
"""
    outside = """
class B : public QObject { Q_OBJECT
signals:
    void first();
public:
    Q_SIGNAL void viaMacro();
};
"""
    for where, src in (("inside the section", inside),
                       ("outside the section", outside)):
        try:
            gen.seam_signals(src, "B")
        except SystemExit as e:
            check("Q_SIGNAL" in str(e), f"{where}: message should name it: {e}")
            continue
        raise AssertionError(f"Q_SIGNAL {where} must be refused")
    return


def test_macro_expanded_declaration_is_refused_by_name():
    # No regex can resolve a macro-expanded declaration without a
    # preprocessor. Skipping it is how a table goes quietly short, so the
    # scanner refuses and quotes the text it could not read.
    src = """
class B : public QObject { Q_OBJECT
signals:
    void first();
    AETHER_SEAM_SIGNAL(viaMacro);
};
"""
    try:
        gen.seam_signals(src, "B")
    except SystemExit as e:
        check("AETHER_SEAM_SIGNAL(viaMacro)" in str(e),
              f"message should quote the unreadable text: {e}")
        return
    raise AssertionError("an unreadable declaration must be refused, not skipped")


def test_a_declaration_the_scanner_cannot_read_is_refused_by_name():
    # The general net: anything shaped like a signal that did not become a
    # probe is named. Here the splitter is fed a declaration with no
    # terminating `;` before the class closes.
    src = """
class B : public QObject { Q_OBJECT
signals:
    void first();
    [[deprecated]] virtual void second() = 0;
};
"""
    # This one the scanner CAN read; assert it does, so the net is not
    # over-eager and does not refuse valid input.
    check(gen.seam_signals(src, "B") == ["first", "second"],
          "attributes and virtual/=0 are ordinary and must parse")


def test_any_preprocessor_directive_in_the_section_is_refused():
    for directive in ("#define X 1", "#undef X", "#include <QString>",
                      "#pragma once", "#if 0", "#endif"):
        src = ("class B : public QObject { Q_OBJECT\nsignals:\n"
               "  void first();\n" + directive + "\n  void second();\n};\n")
        try:
            gen.seam_signals(src, "B")
        except SystemExit:
            continue
        raise AssertionError(f"{directive!r} in the signals section must be refused")



def main() -> int:
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    failures = 0
    for t in tests:
        try:
            t()
            print(f"[PASS] {t.__name__}")
        except AssertionError as e:
            failures += 1
            print(f"[FAIL] {t.__name__}: {e}")
    print(f"{failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
