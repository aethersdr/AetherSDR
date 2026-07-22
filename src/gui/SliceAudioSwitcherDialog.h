#pragma once

#include "PersistentDialog.h"

#include <vector>

class QPushButton;
class QHBoxLayout;
class QCheckBox;

namespace AetherSDR {

class RadioModel;
class SliceModel;

// Small always-on-top-capable panel listing every slice slot the connected
// radio can host (sized to RadioModel::maxSlices(), so it grows to 8 on
// dual-SCU radios like the 6700/8600) with a single click to mute/unmute
// each slice's audio, so the user doesn't have to open every RxApplet to
// pick which slice to listen to. An optional "Solo" mode keeps at most one
// slice's audio on at a time — unmuting a slice mutes every other one.
class SliceAudioSwitcherDialog : public PersistentDialog {
    Q_OBJECT

public:
    explicit SliceAudioSwitcherDialog(RadioModel* model, QWidget* parent = nullptr);

private:
    struct Slot {
        QPushButton* button{nullptr};
        SliceModel* slice{nullptr};
        QMetaObject::Connection muteConn;
        QMetaObject::Connection letterConn;
    };

    void setSlotCount(int count);
    void rebuildSlot(int sliceId);
    void updateButton(QPushButton* button, SliceModel* slice) const;

    RadioModel* m_model;
    std::vector<Slot> m_slots;
    QHBoxLayout* m_buttonRow{nullptr};
    QCheckBox* m_soloCheck{nullptr};
};

} // namespace AetherSDR
