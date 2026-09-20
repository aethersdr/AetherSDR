#pragma once

#include <QString>
#include <QVector>

namespace AetherSDR::ModeFilters {

// The per-mode filter ladders and the width -> passband-edges rule, lifted out
// of VfoWidget so a second surface can offer the same filter widths without a
// second copy of the arithmetic. Two things ask for them now: the VFO's filter
// grid, and the width row under AetherRX's EQ.
//
// The edge rule is not uniform across modes -- SSB pins its low cut and derives
// the high, CW centres on the carrier, DIGU/DIGL centre on a stored offset,
// RTTY straddles mark and space -- and every one of those clauses was written
// against a specific radio behaviour. They are moved here unchanged, comments
// and issue numbers included; this header adds no rule of its own.

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

// The ladder for a mode, narrow to wide. Empty for modes with no presets (FM);
// unknown modes get the SSB ladder, as they always have.
const QVector<int>& widthsForMode(const QString& mode);

// The passband a labelled width means in this mode.
Edges edgesForWidth(const QString& mode, int widthHz, const SliceContext& ctx);

// A passband's labelled width, for matching a ladder entry against what the
// slice is actually running. The inverse of edgesForWidth for every mode whose
// rule is a straight span, and close enough for the rest that the highlight
// lands on the right button.
int widthForEdges(const QString& mode, int lo, int hi);

} // namespace AetherSDR::ModeFilters
