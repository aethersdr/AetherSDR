#pragma once

#include <QWidget>

class QComboBox;

namespace AetherSDR {

class RadioModel;

// PROF — Profile Switcher applet.  A compact three-row selector for the
// radio's Global / TX / Mic profiles.  Each row is a dropdown that enumerates
// the profiles the radio currently knows about; choosing one applies it live.
// The lists and the active selections refresh automatically whenever the radio
// reports a new or changed profile (e.g. after a profile is saved through the
// Profile Manager), so the applet always reflects what the radio actually has.
// See issue #3376.
class ProfileSwitcherApplet : public QWidget {
    Q_OBJECT
public:
    explicit ProfileSwitcherApplet(QWidget* parent = nullptr);

    void setRadioModel(RadioModel* model);

private:
    void refreshGlobal();   // rebuild Global list + current
    void refreshTx();       // rebuild TX list + current
    void refreshMic();      // rebuild Mic list + current
    void syncTxCurrent();   // update only the TX active selection
    void syncMicCurrent();  // update only the Mic active selection

    RadioModel* m_model{nullptr};

    QComboBox* m_globalCombo{nullptr};
    QComboBox* m_txCombo{nullptr};
    QComboBox* m_micCombo{nullptr};
};

} // namespace AetherSDR
