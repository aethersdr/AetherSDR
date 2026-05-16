#pragma once

#include "PersistentDialog.h"
#include "core/tnc/AetherAx25LibmodemShim.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QRadioButton;

namespace AetherSDR {

class AudioEngine;

class Ax25HfPacketDecodeDialog : public PersistentDialog {
    Q_OBJECT

public:
    explicit Ax25HfPacketDecodeDialog(AudioEngine* audio,
                                      int initialSliceId = -1,
                                      QWidget* parent = nullptr);
    ~Ax25HfPacketDecodeDialog() override;

    void setAttachedSlice(int sliceId);

private:
    void setModemProfile(Ax25ModemProfile profile, bool persist);
    void setDecodeEnabled(bool enabled);
    void appendFrame(const Ax25DecodedFrame& frame);
    void refreshStatus();

    AudioEngine* m_audio{nullptr};
    AetherAx25LibmodemShim* m_shim{nullptr};
    QLabel* m_titleLabel{nullptr};
    QRadioButton* m_hf300Profile{nullptr};
    QRadioButton* m_vhf1200Profile{nullptr};
    QCheckBox* m_enableDecode{nullptr};
    QComboBox* m_polarity{nullptr};
    QPlainTextEdit* m_log{nullptr};
    QLabel* m_sliceLabel{nullptr};
    QLabel* m_sampleRateLabel{nullptr};
    QLabel* m_frameCountLabel{nullptr};
    QLabel* m_lastDecodeLabel{nullptr};
    QPushButton* m_clearButton{nullptr};
    int m_attachedSliceId{-1};
    int m_frameCount{0};
    QDateTime m_lastDecodeUtc;
};

} // namespace AetherSDR
