#pragma once

// Spot ID space and spot-label right-click decisions (#6037). SpotModel IDs:
//   radio spots          radio's non-negative indices
//   TCI-injected         positive, from 10000 up (TciProtocol::cmdSpot, source "TCI")
//   memory markers       -(kMemorySpotIdBase + memoryIndex)
//   passive-local        m_nextPassiveSpotId-- from -kPassiveSpotIdBase (cluster/
//                        RBN/WSJT-X/POTA/manual in Passive mode or client-side-spots
//                        backends like HL2/Icom, plus N1MM and EiBi)
// ID sign doesn't imply ownership: "radio-owned?" reads the source too. Label
// hit is decided by a matched label rect with a valid marker index, not the ID.

#include <QMenu>
#include <QObject>
#include <QPoint>
#include <QString>
#include <QVector>

#include <functional>

namespace AetherSDR {

inline constexpr int kMemorySpotIdBase = 1000000;
inline constexpr int kPassiveSpotIdBase = 2000000;

inline bool isPassiveLocalSpotId(int spotIndex)
{
    return spotIndex <= -kPassiveSpotIdBase;
}

namespace SpotLabelPolicy {

// Index into the widget's marker list of the label under `pos`, or -1 when no
// label is there. The first rect containing `pos` decides, as it always did.
// A rect whose markerIndex is outside [0, markerCount) is an SHistory / QRM
// entry, not a spot label, and reports -1 so the general-area menu still
// opens for it.
template <typename HitRect>
int labelMarkerAt(const QVector<HitRect>& rects, qsizetype markerCount,
                  const QPoint& pos)
{
    for (const auto& hr : rects) {
        if (hr.rect.contains(pos)) {
            return (hr.markerIndex >= 0 && hr.markerIndex < markerCount)
                ? hr.markerIndex : -1;
        }
    }
    return -1;
}

enum class Menu {
    General,      // no spot label under the cursor
    ApplyMemory,  // memory marker: Apply Memory only, never Remove Spot
    Spot,         // any other spot, radio-owned or client-side: Remove Spot
};

inline Menu menuFor(bool labelHit, const QString& source)
{
    if (!labelHit) {
        return Menu::General;
    }
    return source == QLatin1String("Memory") ? Menu::ApplyMemory : Menu::Spot;
}

// Everything the right-click path needs about the label under the cursor.
// `menu` is the only gate: the spot ID is carried, never tested.
struct LabelHit {
    Menu menu{Menu::General};
    int spotId{-1};
    QString callsign;
    double freqMhz{0.0};
    QString source;
};

// Resolve the label under `pos` against the widget's own hit rects and marker
// list. SpectrumWidget::mousePressEvent calls this for the right-click menu,
// so a test that feeds it markers with negative IDs exercises the decision
// that used to be `hitSpotIdx >= 0` inline in the widget.
template <typename HitRect, typename Marker>
LabelHit resolveLabelHit(const QVector<HitRect>& rects,
                         const QVector<Marker>& markers, const QPoint& pos)
{
    LabelHit hit;
    const int i = labelMarkerAt(rects, markers.size(), pos);
    if (i < 0) {
        return hit;
    }
    const Marker& m = markers[i];
    hit.spotId = m.index;
    hit.callsign = m.callsign;
    hit.freqMhz = m.freqMhz;
    hit.source = m.source;
    hit.menu = menuFor(true, m.source);
    return hit;
}

// What each spot-label action does. SpectrumWidget binds these to its signals
// and to the clipboard / browser; a test binds recorders.
struct SpotLabelActions {
    std::function<void(int)> applyMemory;              // spot ID
    std::function<void(double)> tune;                  // MHz
    std::function<void(const QString&)> copyCallsign;
    std::function<void(const QString&)> lookupQrz;
    std::function<void(int)> remove;                   // spot ID
};

// Populate `menu` for a label hit. The production right-click path calls this,
// so the actions a test triggers are the ones the operator gets. Adds nothing
// for Menu::General.
inline void addSpotLabelActions(QMenu& menu, QObject* context, Menu kind,
                                int spotId, const QString& callsign,
                                double freqMhz, const SpotLabelActions& a)
{
    if (kind == Menu::ApplyMemory) {
        const QString title = callsign.isEmpty()
            ? QStringLiteral("Apply Memory")
            : QString("Apply %1").arg(callsign);
        menu.addAction(title, context, [f = a.applyMemory, spotId] {
            if (f) {
                f(spotId);
            }
        });
        return;
    }
    if (kind != Menu::Spot) {
        return;
    }
    menu.addAction(QString("Tune to %1").arg(callsign), context,
        [f = a.tune, freqMhz] { if (f) { f(freqMhz); } });
    menu.addAction("Copy Callsign", context,
        [f = a.copyCallsign, callsign] { if (f) { f(callsign); } });
    menu.addAction("Lookup on QRZ", context,
        [f = a.lookupQrz, callsign] { if (f) { f(callsign); } });
    menu.addSeparator();
    menu.addAction("Remove Spot", context,
        [f = a.remove, spotId] { if (f) { f(spotId); } });
}

enum class RemoveRoute {
    LocalModel,    // drop it from SpotModel here; no wire text
    RadioCommand,  // `spot remove <id>` to the radio that owns it
    Ignore,        // not removable through Remove Spot (memory, unknown)
};

// A spot an external TCI client placed. Matched the way the spotTriggered
// handler matches it, case-insensitively.
inline bool isTciSpotSource(const QString& source)
{
    return source.compare(QLatin1String("TCI"), Qt::CaseInsensitive) == 0;
}

// `source` is the spot's SpotModel source (empty if gone). Only radio-owned
// spots are sent `spot remove`: passive-local IDs have no radio counterpart (and
// HL2 has no command plane), and TCI-injected IDs were never assigned by the
// radio. Both are removed from the model directly, as
// TciProtocol::cmdSpotDelete does. Memory markers are never offered Remove Spot.
inline RemoveRoute removeRoute(int spotIndex, const QString& source)
{
    if (source == QLatin1String("Memory")) {
        return RemoveRoute::Ignore;
    }
    if (isPassiveLocalSpotId(spotIndex) || isTciSpotSource(source)) {
        return RemoveRoute::LocalModel;
    }
    if (spotIndex >= 0) {
        return RemoveRoute::RadioCommand;
    }
    return RemoveRoute::Ignore;
}

} // namespace SpotLabelPolicy
} // namespace AetherSDR
