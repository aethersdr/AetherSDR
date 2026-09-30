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
// THE SHAPE: one control, because there is one span. When the span is radio-
// wide, exactly one pane keeps the -/+ pair and the others lose it; when it is
// not, every pane keeps its own, exactly as before. The pane that keeps it is
// the one holding the TRANSMIT slice -- the pane the operator is working, and
// one the UI already distinguishes -- and, when no slice transmits or the TX
// slice's pane is not in the stack, the FIRST pane in the stack's own order.
// The fallback is deliberately dull: a fixed pane rather than "the focused
// one", which would move with every click.
//
// THE INPUT IS THE DECLARATION, NOT A FAMILY. `radioWide` is read off
// RadioModel::backendCapabilities().panSpanModel, the record Hl2Backend and
// AnanBackend already write. It is NOT inferred from
// `receivePanBandwidthControl == nullopt`: that absent optional means four
// different things across the backends, and a Flex -- which genuinely has
// independent per-pan spans -- declares it nullopt too (#5750 triage). An
// ABSENT panSpanModel means nobody has read the question, and it keeps
// today's per-pane behaviour: absence must never hide a control.
//
// Pure and Qt-Core-only so the decision is testable without a widget; the
// call site is MainWindow::syncPanSpanControlPlacement().

#include <QString>
#include <QStringList>

namespace AetherSDR {

// The pane that carries the span control when the span is radio-wide, or an
// empty string meaning "every pane carries its own" (per-pan span, an absent
// declaration, or no panes at all).
inline QString radioWideSpanControlPan(bool radioWide,
                                       const QStringList& panIdsInStackOrder,
                                       const QString& txSlicePanId)
{
    if (!radioWide || panIdsInStackOrder.isEmpty()) {
        return {};
    }
    if (!txSlicePanId.isEmpty() && panIdsInStackOrder.contains(txSlicePanId)) {
        return txSlicePanId;
    }
    return panIdsInStackOrder.first();
}

// Whether `panId`'s pane shows the -/+ span pair.
inline bool spanControlShownOnPan(bool radioWide,
                                  const QStringList& panIdsInStackOrder,
                                  const QString& txSlicePanId,
                                  const QString& panId)
{
    const QString owner =
        radioWideSpanControlPan(radioWide, panIdsInStackOrder, txSlicePanId);
    return owner.isEmpty() || owner == panId;
}

}  // namespace AetherSDR
