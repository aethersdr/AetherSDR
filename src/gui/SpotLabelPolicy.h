#pragma once

// Spot ID space and the spot-label right-click decisions (#6037).
//
// Spot IDs are not all radio-owned. The radio's own spots use its
// non-negative indices; everything this client places in SpotModel itself
// uses negative IDs offset by a base so the two can never collide:
//
//   memory markers       -(kMemorySpotIdBase + memoryIndex)
//   passive-local spots  m_nextPassiveSpotId-- from -kPassiveSpotIdBase down
//                        (DX cluster / RBN / WSJT-X / POTA / manual spots in
//                        Passive mode or on a client-side-spots backend such
//                        as HL2 and Icom, plus N1MM and EiBi)
//
// The right-click menu once decided "was a label hit?" from the SIGN of the
// hit marker's ID (`hitSpotIdx >= 0`), so every client-side label fell
// through to the general-area menu: no Remove Spot for a local spot, and the
// Apply Memory branch was unreachable. Presence is now decided by whether a
// label rect with a valid marker index matched, independent of the ID space.

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
    if (!labelHit)
        return Menu::General;
    return source == QLatin1String("Memory") ? Menu::ApplyMemory : Menu::Spot;
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
            if (f) f(spotId);
        });
        return;
    }
    if (kind != Menu::Spot)
        return;
    menu.addAction(QString("Tune to %1").arg(callsign), context,
        [f = a.tune, freqMhz] { if (f) f(freqMhz); });
    menu.addAction("Copy Callsign", context,
        [f = a.copyCallsign, callsign] { if (f) f(callsign); });
    menu.addAction("Lookup on QRZ", context,
        [f = a.lookupQrz, callsign] { if (f) f(callsign); });
    menu.addSeparator();
    menu.addAction("Remove Spot", context,
        [f = a.remove, spotId] { if (f) f(spotId); });
}

enum class RemoveRoute {
    LocalModel,    // drop it from SpotModel here; no wire text
    RadioCommand,  // `spot remove <id>` to the radio that owns it
    Ignore,        // not removable through Remove Spot (memory, unknown)
};

// Only a radio-owned (non-negative) ID is ever written to the radio. A
// passive-local ID has no radio-side counterpart — and on HL2 there is no
// command plane to receive one — so it is removed from the model directly.
inline RemoveRoute removeRoute(int spotIndex)
{
    if (isPassiveLocalSpotId(spotIndex))
        return RemoveRoute::LocalModel;
    if (spotIndex >= 0)
        return RemoveRoute::RadioCommand;
    return RemoveRoute::Ignore;
}

} // namespace SpotLabelPolicy
} // namespace AetherSDR
