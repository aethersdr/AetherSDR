#pragma once

#include "PersistentDialog.h"

#include <array>

class QPushButton;

namespace AetherSDR {

class RadioModel;
class SliceModel;

// Small always-on-top-capable panel listing all four possible slice slots
// (A-D) with a single click to mute/unmute each slice's audio, so the user
// doesn't have to open every RxApplet to pick which slice to listen to.
class SliceAudioSwitcherDialog : public PersistentDialog {
    Q_OBJECT

public:
    explicit SliceAudioSwitcherDialog(RadioModel* model, QWidget* parent = nullptr);

private:
    struct Slot {
        QPushButton* button{nullptr};
        QMetaObject::Connection muteConn;
    };

    void rebuildSlot(int sliceId);
    void updateButton(QPushButton* button, SliceModel* slice) const;

    RadioModel* m_model;
    std::array<Slot, 4> m_slots;
};

} // namespace AetherSDR
