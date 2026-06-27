#pragma once

#include <QString>
#include <QVector>
#include <QSet>

// DisplayInventoryPolicy — classifies the radio-authoritative set of display
// objects (panadapters + waterfalls, as accumulated from "display pan" /
// "display waterfall" status) against what this client actually owns.
//
// This is the Layer-B half of the #3856 leak detector. Unlike the Layer-A
// UDP-orphan detector (which sees streams the radio is still *transmitting*),
// this works purely from status bookkeeping, so it catches the case the
// packet-based detector cannot: a display object the radio keeps *allocated*
// but no longer streams — e.g. a waterfall left behind by a panafall close that
// omitted "display panafall remove" (#3843), on firmware that stops the
// waterfall UDP on pan-removal. The headline signal is a waterfall whose parent
// panadapter no longer exists on the radio.
//
// Pulled into a pure, header-only function (mirroring RadioStatusOwnership and
// SliceRecreatePolicy) so it is unit-testable without a live radio: RadioModel
// feeds it the accumulated radio-side maps + the owned set, and acts on the
// returned Report.

namespace AetherSDR::DisplayInventory {

// One display object exactly as the RADIO reports it, regardless of ownership.
struct RadioPan { QString id; quint32 clientHandle{0}; };
struct RadioWf  { QString id; quint32 clientHandle{0}; QString parentPanId; };

enum class Ownership {
    Ours,     // this client holds a live model for it
    Foreign,  // a different Multi-Flex client owns it (client_handle mismatch)
    Orphan,   // no live owner on this client — leaked / crashed-session / unclaimed
};

struct PanVerdict { QString id; quint32 clientHandle{0}; Ownership ownership{Ownership::Orphan}; };
struct WfVerdict  {
    QString   id;
    quint32   clientHandle{0};
    Ownership ownership{Ownership::Orphan};
    QString   parentPanId;
    bool      parentMissing{false};   // radio has this waterfall but not its parent pan
};

struct Inputs {
    QVector<RadioPan> radioPans;          // every pan the radio currently reports
    QVector<RadioWf>  radioWaterfalls;    // every waterfall the radio currently reports
    QSet<QString>     ownedPanIds;        // panIds this client holds (m_panadapters)
    QSet<QString>     ownedWaterfallIds;  // the waterfallIds those pans reference
    quint32           ourHandle{0};       // this client's handle (0 if unknown)
};

struct Report {
    QVector<PanVerdict> pans;
    QVector<WfVerdict>  waterfalls;
    int oursPanCount{0},   foreignPanCount{0},   orphanPanCount{0};
    int oursWfCount{0},    foreignWfCount{0},     orphanWfCount{0};
    // The headline #3843 fingerprint: waterfalls the radio still holds whose
    // parent panadapter no longer exists — a leaked waterfall stream.
    QVector<QString>    leakedWaterfalls;
};

inline Ownership classifyOwnership(const QString& id, quint32 handle,
                                   const QSet<QString>& owned, quint32 ourHandle)
{
    if (owned.contains(id))
        return Ownership::Ours;
    // A non-zero handle that isn't ours marks another Multi-Flex client. We
    // only assert "foreign" when we actually know our own handle, otherwise a
    // mismatch is unprovable and we fall through to Orphan.
    if (handle != 0 && ourHandle != 0 && handle != ourHandle)
        return Ownership::Foreign;
    return Ownership::Orphan;
}

inline Report classify(const Inputs& in)
{
    Report r;
    QSet<QString> radioPanIds;
    radioPanIds.reserve(in.radioPans.size());
    for (const auto& p : in.radioPans)
        radioPanIds.insert(p.id);

    for (const auto& p : in.radioPans) {
        const Ownership o = classifyOwnership(p.id, p.clientHandle,
                                              in.ownedPanIds, in.ourHandle);
        r.pans.push_back(PanVerdict{p.id, p.clientHandle, o});
        if (o == Ownership::Ours)         ++r.oursPanCount;
        else if (o == Ownership::Foreign) ++r.foreignPanCount;
        else                              ++r.orphanPanCount;
    }

    for (const auto& w : in.radioWaterfalls) {
        const Ownership o = classifyOwnership(w.id, w.clientHandle,
                                              in.ownedWaterfallIds, in.ourHandle);
        const bool parentMissing =
            !w.parentPanId.isEmpty() && !radioPanIds.contains(w.parentPanId);
        r.waterfalls.push_back(WfVerdict{w.id, w.clientHandle, o,
                                         w.parentPanId, parentMissing});
        if (o == Ownership::Ours)         ++r.oursWfCount;
        else if (o == Ownership::Foreign) ++r.foreignWfCount;
        else                              ++r.orphanWfCount;
        // A waterfall whose parent pan is gone is a leak no matter who nominally
        // owned it — the radio is holding a display resource with no panadapter.
        if (parentMissing)
            r.leakedWaterfalls.push_back(w.id);
    }
    return r;
}

}  // namespace AetherSDR::DisplayInventory
