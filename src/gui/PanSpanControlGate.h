#pragma once

// WHICH PANE CARRIES THE SPAN CONTROL (#5750).
//
// SpectrumWidget builds a -/+ span pair on every pane, and on a radio with
// per-pan span (a Flex) that is the truth: each pane has its own span, and its
// own control for it. On a radio whose span is ONE register for the whole
// board -- RadioCapabilities::panSpanModel->radioWide, declared true by the
// Hermes-Lite 2 -- every pane's pair moves the same register, so pressing "+"
// on one pane re-spans them all. The control was offered per pane and acted
// radio-wide, and nothing on screen said so.
//
// THE SHAPE: one LIVE control, because there is one span. When the span is
// radio-wide, exactly one pane keeps its -/+ pair enabled and every other pane
// shows its pair DIMMED, with the reason in its tooltip and accessible
// description (AGENTS.md: "Dim it, never hide it"); when the span is not
// radio-wide, every pane keeps its own live pair, exactly as before. The pane
// that keeps it live is the one holding the TRANSMIT slice -- the pane the
// operator is working, and one the UI already distinguishes -- and, when no
// slice transmits or the TX slice's pane is not in the stack, the first pane
// in FALLBACK ORDER: docked panes in the order the layout shows them, then any
// pane that is floating or lent to the workspace canvas. So the fallback is a
// pane in the main window whenever one exists, and a fixed one rather than
// "the focused one", which would move with every click.
//
// THE INPUT IS THE DECLARATION, NOT A FAMILY. `radioWide` is read off
// RadioModel::backendCapabilities().panSpanModel, the record Hl2Backend and
// AnanBackend already write. It is NOT inferred from
// `receivePanBandwidthControl == nullopt`: that absent optional means four
// different things across the backends, and a Flex -- which genuinely has
// independent per-pan spans -- declares it nullopt too (#5750 triage). An
// ABSENT panSpanModel means nobody has read the question, and it keeps
// today's per-pane behaviour: absence must never dim a control.
//
// Pure and Qt-Core-only so the decision is testable without a widget; the
// call site is MainWindow::syncPanSpanControlPlacement().

#include <QString>
#include <QStringList>

namespace AetherSDR {

// Fallback order for the live span control: the docked panes in layout order
// first, then every other pane the stack holds (floating, or lent to the
// workspace canvas) in the order given. `dockedInLayoutOrder` comes from
// PanadapterStack::dockedPanIdsInLayoutOrder(); `allPanIds` is every pane the
// stack holds, in any order. An id in the first list that the second does not
// hold is dropped, so the result never names a pane that is gone.
inline QStringList panIdsInSpanFallbackOrder(const QStringList& dockedInLayoutOrder,
                                             const QStringList& allPanIds)
{
    QStringList ordered;
    for (const QString& id : dockedInLayoutOrder) {
        if (allPanIds.contains(id) && !ordered.contains(id)) {
            ordered.append(id);
        }
    }
    for (const QString& id : allPanIds) {
        if (!ordered.contains(id)) {
            ordered.append(id);
        }
    }
    return ordered;
}

// The pane whose span control stays live when the span is radio-wide, or an
// empty string meaning "every pane's control is live" (per-pan span, an absent
// declaration, or no panes at all). `panIdsInFallbackOrder` is every pane the
// stack holds, ordered by panIdsInSpanFallbackOrder().
inline QString radioWideSpanControlPan(bool radioWide,
                                       const QStringList& panIdsInFallbackOrder,
                                       const QString& txSlicePanId)
{
    if (!radioWide || panIdsInFallbackOrder.isEmpty()) {
        return {};
    }
    if (!txSlicePanId.isEmpty() && panIdsInFallbackOrder.contains(txSlicePanId)) {
        return txSlicePanId;
    }
    return panIdsInFallbackOrder.first();
}

// Whether `panId`'s -/+ span pair is live (enabled). False means dimmed with
// the shared-span reason, never hidden.
inline bool spanControlLiveOnPan(bool radioWide,
                                 const QStringList& panIdsInFallbackOrder,
                                 const QString& txSlicePanId,
                                 const QString& panId)
{
    const QString owner =
        radioWideSpanControlPan(radioWide, panIdsInFallbackOrder, txSlicePanId);
    return owner.isEmpty() || owner == panId;
}

}  // namespace AetherSDR
