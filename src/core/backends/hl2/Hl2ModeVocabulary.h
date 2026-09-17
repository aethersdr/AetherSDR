#pragma once

// THE TWO MODE LISTS THIS BACKEND KEEPS, AND WHY THERE ARE TWO.
//
// An earlier draft of #5755 published one list on the reasoning that "does this
// backend really do this mode" is a single question. It is two, and
// aethersdr-agent was right to say so:
//
//   * ACCEPT asks "may a stored document say this". It must take every spelling
//     modeFromString() maps, aliases included, or a restore silently loses the
//     operator's mode instead of protecting them from it. This is the guard
//     from PR #4619's review (Ozy311): a mode string that would NOT map is
//     dropped rather than reaching Receiver::mode, the UI, and — via capture —
//     re-persisting itself.
//
//   * OFFER asks "should an operator be able to pick this". It must list each
//     mode ONCE, and only where picking it does something.
//
// The offered list is a SUBSET of the accepted one, which is the invariant that
// matters: a menu can never offer a mode the restore boundary would reject, and
// the boundary stays free to accept spellings the menu has no business showing.
// hl2_mode_vocabulary_test asserts the containment rather than trusting it.
//
// THREE GROUPS ARE ACCEPTED AND NOT OFFERED.
//
//   ALIASES. CWU is CW under a second name (VoiceModeGate.h says so outright),
//   WFM is WBFM's, NFM is FM's — modeFromString() maps each pair onto one
//   WdspChannel mode. A menu carrying both members of a pair asks the operator
//   to choose between a mode and itself.
//
//   WBFM/WFM. RxApplet.cpp's mode handler is written on the stated assumption
//   that "WFM" is never an entry in m_modeCombo, because the WFM software-demod
//   overlay is toggled from the VFO flag instead. Publishing it would put two
//   different WFM concepts in one widget, one of them reached through a handler
//   that tears the other one down.
//
//   DRM. There is no DRM decoder on this backend. modeFromString() mapping it
//   onto a WDSP mode is not the radio demodulating it, and offering it is the
//   same class of claim #5580 exists to remove — its own third bullet.
//
// THE DELTA AGAINST THE COMPILED-IN FLEXRADIO FALLBACK this replaces, in full:
// RTTY, DFM and DSTR leave (nothing here demodulates them, and modeFromString()
// turns all three into USB while every readback still says RTTY); NFM leaves
// (it is FM); DSB and CWL arrive, both real, both distinct, both unreachable
// from the combo before.
//
// HEADER, NOT A .cpp-LOCAL LIST, for the reason tests/tests.cmake states beside
// hl2_pan_limits_declaration_test: "a declaration must not be pinned only
// inside something that does not build." The fake-radio fixture that would have
// carried a seam assertion — hl2_backend_test — is retired inside a commented
// block, so an assertion written there would be green forever.

#include <QString>
#include <QStringList>

namespace AetherSDR::hl2 {

// Every spelling modeFromString() genuinely maps. The restore boundary's
// vocabulary; deliberately generous.
inline const QStringList& knownModeStrings() noexcept
{
    static const QStringList kKnown = {
        QStringLiteral("LSB"), QStringLiteral("USB"), QStringLiteral("DSB"),
        QStringLiteral("CWL"), QStringLiteral("CWU"), QStringLiteral("CW"),
        QStringLiteral("FM"),  QStringLiteral("NFM"), QStringLiteral("AM"),
        QStringLiteral("DIGU"), QStringLiteral("DIGL"), QStringLiteral("SAM"),
        QStringLiteral("DRM"), QStringLiteral("WBFM"), QStringLiteral("WFM"),
    };
    return kKnown;
}

inline bool isKnownModeString(const QString& mode) noexcept
{
    return knownModeStrings().contains(mode.toUpper());
}

// The modes the mode menu offers — a subset of the above.
inline const QStringList& publishedModeStrings() noexcept
{
    static const QStringList kPublished = {
        QStringLiteral("LSB"), QStringLiteral("USB"), QStringLiteral("DSB"),
        QStringLiteral("CWL"), QStringLiteral("CW"),  QStringLiteral("FM"),
        QStringLiteral("AM"),  QStringLiteral("SAM"),
        QStringLiteral("DIGU"), QStringLiteral("DIGL"),
    };
    return kPublished;
}

}  // namespace AetherSDR::hl2
