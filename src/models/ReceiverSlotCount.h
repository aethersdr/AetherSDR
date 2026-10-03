#pragma once

#include <QList>
#include <QObject>

namespace AetherSDR {

class RadioModel;
class SliceModel;

// How many receiver letters (A, B, C, ...) the UI offers: the RX applet's slice
// tabs and the CAT applet's VFO targets, one number so the two cannot disagree
// (#5775, #5776). A backend that declares its own capacity (the HL2) may only
// know it after connect and may revise it mid-session, so this follows every
// edge that can move it and announces changes. Family-agnostic.
class ReceiverSlotCount : public QObject {
    Q_OBJECT
public:
    explicit ReceiverSlotCount(RadioModel* radio, QObject* parent = nullptr);

    // The declared ceiling, floored by the slots running receivers occupy (by
    // global slice id, so a falling ceiling cannot strand a live receiver), and
    // capped at the letters A-H: a slice id is wire data.
    static int forCeiling(int declaredCeiling, const QList<SliceModel*>& slices);

    // CAT VFO letters: every letter with no radio connected, else the radio's
    // own count, even when that is one.
    static int catLetters(const RadioModel* radio);

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
