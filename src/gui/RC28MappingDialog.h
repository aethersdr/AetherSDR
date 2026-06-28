#pragma once
#ifdef HAVE_HIDAPI

#include "PersistentDialog.h"

#include <QPointer>

class QCheckBox;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QRadioButton;
class QSlider;

namespace AetherSDR {

class HidEncoderManager;

// RC-28 Button Mapping dialog — configure TX bar PTT mode, F1/F2 press and
// hold actions, and view live device info + button activity.
// Opened from Settings → Icom RC-28 Remote Encoder...   (#3323)
class RC28MappingDialog : public PersistentDialog {
    Q_OBJECT

public:
    explicit RC28MappingDialog(HidEncoderManager* encoder,
                               QWidget* parent = nullptr);

    // Called from MainWindow's HID dispatch path (dispatchHidAction and PTT
    // branch) to populate the activity log while the dialog is open.
    void appendButtonEvent(const QString& slotLabel,
                           const QString& actionName);

signals:
    void mappingFieldChanged(const QString& field, const QString& value);

private slots:
    void onConnectionChanged(bool connected, const QString& deviceName);
    void onMultipleDevicesDetected(const QString& deviceName);

private:
    void buildDeviceSection();
    void buildTxBarSection();
    void buildAssignSection();
    void buildEncoderSection();
    void buildLogSection();

    void refreshDeviceInfo();
    void populateActionCombo(QComboBox* combo, const QString& field,
                             const QString& defaultId, bool holdOnly = false);

    QPointer<HidEncoderManager> m_encoder;
    QString m_knownDeviceName;

    // Section A — device info
    QLabel* m_statusDot{nullptr};
    QLabel* m_statusLabel{nullptr};
    QLabel* m_vidPidLabel{nullptr};
    QLabel* m_pathLabel{nullptr};
    QLabel* m_serialLabel{nullptr};
    QLabel* m_firmwareLabel{nullptr};

    // Section B — TX bar PTT mode
    QRadioButton* m_momentaryBtn{nullptr};
    QRadioButton* m_latchedBtn{nullptr};

    // Section C — F1/F2 action combos
    QComboBox* m_f1PressCombo{nullptr};
    QComboBox* m_f1HoldCombo{nullptr};
    QComboBox* m_f2PressCombo{nullptr};
    QComboBox* m_f2HoldCombo{nullptr};

    // Section D — encoder tuning aids
    QSlider*   m_sensitivitySlider{nullptr};
    QLabel*    m_sensitivityValueLabel{nullptr};
    QCheckBox* m_autoSnapCheck{nullptr};

    // Section E — debug log
    QPlainTextEdit* m_log{nullptr};
};

} // namespace AetherSDR
#endif // HAVE_HIDAPI
