#pragma once

#include <QAudioDevice>
#include <QByteArray>
#include <QDialog>
#include <QList>

class QListWidget;
class QCheckBox;
class QVBoxLayout;
class QWidget;

namespace AetherSDR {

class AudioDeviceChangeDialog : public QDialog {
public:
    AudioDeviceChangeDialog(const QList<QAudioDevice>& inputDevices,
                            const QList<QAudioDevice>& outputDevices,
                            const QAudioDevice& currentInputDevice,
                            const QAudioDevice& currentOutputDevice,
                            const QList<QByteArray>& newInputDeviceIds,
                            const QList<QByteArray>& newOutputDeviceIds,
                            QWidget* parent = nullptr);

    QAudioDevice selectedInputDevice() const;
    QAudioDevice selectedOutputDevice() const;
    bool dontAskAgainChecked() const;
    void setFramelessMode(bool on);

private:
    QAudioDevice selectedDevice(const QListWidget* list,
                                const QList<QAudioDevice>& devices) const;

    QWidget* m_titleBar{nullptr};
    QVBoxLayout* m_bodyLayout{nullptr};
    QListWidget* m_inputList{nullptr};
    QListWidget* m_outputList{nullptr};
    QCheckBox* m_dontAskAgainCheck{nullptr};
    QList<QAudioDevice> m_inputDevices;
    QList<QAudioDevice> m_outputDevices;
};

} // namespace AetherSDR
