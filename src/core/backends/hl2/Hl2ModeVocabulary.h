#pragma once

// ACCEPT: every spelling modeFromString() maps (aliases included); anything else
// is dropped at restore. OFFER: what an operator can pick, each mode once.
// OFFER is a subset of ACCEPT (hl2_mode_vocabulary_test). Accepted, not offered:
// aliases CWU/WFM/NFM; WBFM/WFM (RxApplet assumes "WFM" is never in m_modeCombo);
// DRM (no decoder). canonicalOfferedMode() maps an alias to its offered spelling
// at restore and in setSliceMode() so the combos match; display-only, no DSP
// effect. receiveOnlyModes lists each alias pair both ways or neither
// (modeIsReceiveOnly() is a membership test), so canonicalising cannot weaken a
// TX refusal.

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
