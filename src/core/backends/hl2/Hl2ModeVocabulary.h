#pragma once

// THE TWO MODE LISTS THIS BACKEND KEEPS:
//
//   * ACCEPT — "may a stored document say this". Every spelling
//     modeFromString() maps, aliases included; a string that would not map is
//     dropped at restore rather than reaching Receiver::mode and re-persisting.
//   * OFFER — "should an operator be able to pick this". Each mode once, and
//     only where picking it does something.
//
// OFFER is a subset of ACCEPT (hl2_mode_vocabulary_test asserts it), so the menu
// never offers what restore would reject. Accepted but not offered:
//
//   ALIASES   CWU (= CW), WFM (= WBFM), NFM (= FM); one WdspChannel mode each.
//   WBFM/WFM  RxApplet's mode handler assumes "WFM" is never in m_modeCombo
//             (the WFM overlay is toggled from the VFO flag).
//   DRM       no decoder on this backend.
//
// canonicalOfferedMode() collapses an alias onto the offered spelling at the
// restore boundary and in setSliceMode(), before it reaches Receiver::mode.
// Otherwise the combos (which only move on a findText() hit) would show index 0
// ("LSB") while the receiver is in FM. This is a no-op on the DSP: it renames
// what is shown, never what is heard.
//
// It cannot weaken a TX refusal: modeIsReceiveOnly() is a case-insensitive
// membership test on what the slice holds, so every pair is listed both ways in
// receiveOnlyModes (FM/NFM, WBFM/WFM) or on neither (CW/CWU, which transmit via
// the gateware keyer). The duplicates stay; a missing entry costs far more.
//
// WBFM and DRM have no offered twin and remain undisplayable if reached via
// CAT, TCI or a hand-edited document (residual set in hl2_mode_vocabulary_test).
//
// A header, not a .cpp-local list, so it is pinned by a test that builds.

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

// The OFFERED spelling of an accepted one -- uppercased, and with each alias
// pair collapsed onto the member publishedModeStrings() carries.
//
// Total over every input: a spelling with no alias twin (and any string the
// restore guard would have rejected anyway) comes back uppercased and
// otherwise unchanged, so callers need no membership test before calling. The
// three pairs are modeFromString()'s own, read off its NFM/FM, CWU/CW and
// WFM/WBFM branches; adding a pair there means adding it here, which is why
// hl2_mode_vocabulary_test walks knownModeStrings() rather than a retyped copy.
inline QString canonicalOfferedMode(const QString& mode) noexcept
{
    const QString u = mode.toUpper();
    if (u == QLatin1String("NFM")) return QStringLiteral("FM");
    if (u == QLatin1String("CWU")) return QStringLiteral("CW");
    if (u == QLatin1String("WFM")) return QStringLiteral("WBFM");
    return u;
}

}  // namespace AetherSDR::hl2
