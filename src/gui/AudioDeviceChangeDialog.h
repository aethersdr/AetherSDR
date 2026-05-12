#pragma once

#include <QAudioDevice>
#include <QByteArray>
#include <QDialog>
#include <QList>

class QListWidget;
class QShowEvent;

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

protected:
    void showEvent(QShowEvent* event) override;

private:
    QAudioDevice selectedDevice(const QListWidget* list,
                                const QList<QAudioDevice>& devices) const;

    QListWidget* m_inputList{nullptr};
    QListWidget* m_outputList{nullptr};
    QList<QAudioDevice> m_inputDevices;
    QList<QAudioDevice> m_outputDevices;
};

} // namespace AetherSDR
