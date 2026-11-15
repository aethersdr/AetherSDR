#pragma once

#include <QList>
#include <QString>

namespace AetherSDR::PanSliceTitle {

struct SliceOnPan {
    int sliceId;
    QString panId;
};

// The slice a panadapter's title names: the active slice when it is on this
// pan, else the slice the title already names if it is still here, else the
// first slice on the pan. -1 when the pan has no slice.
inline int pick(const QString& panId, int activeSliceId, int shownSliceId,
                const QList<SliceOnPan>& slices)
{
    int first = -1;
    bool shownHere = false;
    for (const SliceOnPan& s : slices) {
        if (s.panId != panId)
            continue;
        if (s.sliceId == activeSliceId)
            return s.sliceId;
        if (s.sliceId == shownSliceId)
            shownHere = true;
        if (first < 0)
            first = s.sliceId;
    }
    return shownHere ? shownSliceId : first;
}

// The id of the panadapter the window shows before any radio connects; the
// first real pan takes it over.
inline const QString kPlaceholderPanId = QStringLiteral("default");

// The plain-text name for a pan: its slice title, or "Pan <id>" when it has
// no slice. The pre-connect placeholder has no name. The docked header and the
// floating window title both use it.
inline QString displayName(const QString& sliceTitle, const QString& panId)
{
    if (!sliceTitle.isEmpty())
        return sliceTitle;
    if (panId.isEmpty() || panId == kPlaceholderPanId)
        return {};
    return QStringLiteral("Pan %1").arg(panId);
}

} // namespace AetherSDR::PanSliceTitle
