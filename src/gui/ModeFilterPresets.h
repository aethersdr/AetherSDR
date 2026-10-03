#pragma once

#include <QString>
#include <QVector>

namespace AetherSDR { struct ReceiveFilterControl; }

namespace AetherSDR::ModeFilters {

// Per-mode filter ladders and the width → passband-edges rule, shared by the
// VFO filter grid and AetherRX's width row. The rule differs per mode (SSB pins
// the low cut, CW centres on the carrier, DIGU/DIGL on a stored offset, RTTY
// straddles mark and space); each clause matches a specific radio behaviour.

// What the slice contributes to the edge calculation. Everything else the rule
// needs is the mode and the width.
struct SliceContext {
    int diguOffset = 1500;
    int diglOffset = 1500;
    int rttyShift = 170;
};

struct Edges {
    int lo = 0;   // Hz relative to the slice's tuned frequency
    int hi = 0;
};

// The ladder for a mode, narrow to wide. Empty for analog FM without a
// declared adjustable-filter capability;
// unknown modes get the SSB ladder, as they always have.
const QVector<int>& widthsForMode(const QString& mode);

// FM defaults above describe fixed radio filters. A receiver which declares
// adjustable FM edges can use the DFM ladder, or the broadcast-width ladder
// for WFM/WBFM, intersected with its declared bounds.
// The conservative headless record does not gate unrelated desktop modes.
QVector<int> widthsForMode(const QString& mode, const ReceiveFilterControl* control);
// Analog FM spellings. DFM retains its existing separate preset behavior.
bool isFmMode(const QString& mode);

// The passband a labelled width means in this mode.
Edges edgesForWidth(const QString& mode, int widthHz, const SliceContext& ctx);
bool acceptsFmEdges(const QString& mode, const ReceiveFilterControl* control, Edges edges);

// A passband's labelled width, for matching a ladder entry against what the
// slice is actually running. The inverse of edgesForWidth for every mode whose
// rule is a straight span, and close enough for the rest that the highlight
// lands on the right button.
int widthForEdges(const QString& mode, int lo, int hi);

} // namespace AetherSDR::ModeFilters
