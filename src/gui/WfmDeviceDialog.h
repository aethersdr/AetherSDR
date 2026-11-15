#pragma once
#include <QAudioDevice>
#include <QDialog>

class QCheckBox;
class QDialogButtonBox;
class QListWidget;

namespace AetherSDR {

// Audio output device picker for the WFM demodulator.
// Lists all available output devices via QMediaDevices::audioOutputs() —
// cross-platform: works on Windows, macOS, and Linux.
// The selected device is identified by QAudioDevice::id() (a persistent
// opaque byte string) rather than a human-readable name, so it survives
// device renames and reordering.
class WfmDeviceDialog : public QDialog
{
    Q_OBJECT
public:
    explicit WfmDeviceDialog(QWidget* parent = nullptr);

    // Returns the QAudioDevice::id() of the selected device, or empty if
    // none was selected / the dialog was cancelled.
    QString selectedDeviceId() const;

    // Returns the human-readable description of the selected device.
    QString selectedDeviceName() const;

    bool rememberChoice() const;

private:
    void populate();

    QListWidget*      m_list{nullptr};
    QCheckBox*        m_rememberCheck{nullptr};
    QDialogButtonBox* m_buttons{nullptr};

    QList<QAudioDevice> m_devices;
};

} // namespace AetherSDR
