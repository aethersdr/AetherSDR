#pragma once

#include <QList>
#include <QObject>

namespace AetherSDR {

class RadioModel;
class SliceModel;

// How many receiver letters (A, B, C, ...) the UI offers: the RX applet's slice
// tabs and the CAT applet's VFO targets. One number, one owner, so the two
// surfaces cannot disagree about the same radio (#5775, #5776).
//
// WHY A TRACKER AND NOT A READ AT ONE EDGE. The slice tabs used to be sized from
// RadioModel::infoChanged alone, which fires at the START of a connect. A
// backend that declares its capacity itself (RadioModel::maxSlices() defers to
// IRadioBackend::capabilities().maxSlices for every non-Flex family) may only
// know that capacity once the link is up, and may revise it mid-session — the
// Hermes-Lite 2 reports 1 until connected, then the board's receiver count,
// which falls when the span outgrows the link budget. Both of those arrive on
// RadioModel::capabilitiesChanged, which nothing sized the tabs from. The CAT
// letters, separately, were sized from the Flex model table
// (RadioModel::maxSlicesForModel), which has no row for a radio outside that
// family and answers its 2-slice default.
//
// So this listens to every edge that can move the answer — the connection and
// capability edges, the Flex info/status edge, and slices coming and going — and
// announces the number only when it changes. It asks nothing about the family.
class ReceiverSlotCount : public QObject {
    Q_OBJECT
public:
    explicit ReceiverSlotCount(RadioModel* radio, QObject* parent = nullptr);

    // The letters to offer for a radio declaring `declaredCeiling` receivers
    // while running `slices`: the declared ceiling, but never fewer than the
    // slots those receivers occupy. A ceiling can FALL below what is running
    // (the HL2 at a wider span), and a shrink must not strand a live receiver
    // without a tab or a CAT letter. Slots, not a count: the tabs are indexed by
    // global slice id, so a receiver in slot C needs three letters even alone.
    static int forCeiling(int declaredCeiling, const QList<SliceModel*>& slices);

    // forCeiling(radio->maxSlices(), radio->slices()) while connected; 0 while
    // disconnected, when there is no radio for the number to describe.
    int count() const { return m_count; }

signals:
    void countChanged(int count);

private:
    void refresh();

    RadioModel* m_radio{nullptr};
    int m_count{0};
};

} // namespace AetherSDR
