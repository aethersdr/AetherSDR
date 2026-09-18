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
// AN ACCEPTED ALIAS MUST BE RECONCILED, NOT MERELY ACCEPTED (#5755 review,
// jensenpat). Splitting the lists is only half a change: NFM was an ordinary
// entry in the FlexRadio fallback combo this replaces, it is still accepted on
// restore, and it is no longer offered -- so a session saved in NFM came back
// with the slice holding "NFM" and the menu unable to show it. Both consumers
// rebuild with clear()/addItems()/findText(currentText) and only move the
// selection when findText() succeeds (RxApplet::connectSlice and
// VfoWidget::setSlice, both on SliceModel::modeListChanged), so the combo fell
// to index 0 -- "LSB" -- with signals blocked, while the receiver really was in
// FM. Nothing downstream heals it: RxApplet's SliceModel::modeChanged handler
// is written the same findText()-must-hit way, and VfoWidget's does not touch
// the combo at all -- it only relabels the mode TAB, so that widget ends up
// showing "NFM" on the tab and "LSB" in the combo at the same time. An operator
// reading LSB while hearing FM is the exact fault #5580 exists to remove, so
// introducing a fresh one here would be the change arguing against itself.
//
// canonicalOfferedMode() closes it at the restore boundary: the alias collapses
// onto the spelling the menu carries BEFORE the mode reaches Receiver::mode, so
// the menu and the model cannot disagree about a restored document.
//
// IT CANNOT WEAKEN A TX REFUSAL, and that is checked rather than asserted.
// RadioCapabilities::modeIsReceiveOnly() is a case-insensitive MEMBERSHIP test,
// not alias normalisation, and RadioModel::refuseKeyInReceiveOnlyMode() runs it
// on what the slice holds. Every pair below is therefore listed BOTH WAYS in
// Hl2Backend::capabilities()'s receiveOnlyModes (FM and NFM, WBFM and WFM) or
// on NEITHER (CW and CWU both transmit correctly through the gateware keyer).
// Collapsing one spelling onto the other moves nothing across that boundary --
// hl2_mode_vocabulary_test pins the equivalence for every accepted spelling.
// The duplicate entries stay: CAT, TCI and Hl2Backend::setSliceMode still put
// either spelling on the slice at run time, and those entries are what refuse
// the key when they do.
//
// modeFromString() maps both members of each pair onto ONE WdspChannel mode, so
// the reconciliation is a no-op on the DSP: it renames what the operator is
// shown, never what they hear. defaultPassbandForMode() and cwBfoOffsetHz()
// carry both spellings in one branch for the same reason.
//
// TWO ACCEPTED MODES REMAIN UNDISPLAYABLE -- WBFM and DRM, which have no
// offered twin to collapse onto. Neither is a regression of this PR: neither
// was in the FlexRadio fallback either, so a slice holding one already showed
// index 0 before this change. Reaching them at all needs CAT, TCI or a
// hand-edited document. Said here rather than left for the next reader to
// rediscover; see the residual set in hl2_mode_vocabulary_test.
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
