#pragma once
#include <QtGlobal>
#if defined(Q_OS_WIN) && defined(HAVE_HIDAPI)

#include "core/UlanziChordDecoder.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

class QTimer;

namespace AetherSDR {

// Windows Ulanzi Dial backend (hidapi), same signal contract as Linux evdev:
//   tuneSteps(int)               — rotary delta (+1 CW / -1 CCW)
//   buttonEvent(sig, action)     — see EvdevEncoderManager for signature
//   connectionChanged(bool, name)
// The dial enumerates as several HID interfaces (Keyboard for Ctrl+chords,
// Consumer Control for media keys, maybe Mouse); every device whose
// product_string contains "Ulanzi Dial" is opened and polled, and report diffs
// feed the shared chord decoder. No exclusive grab on Windows, so keystrokes
// still reach the focused window (RawInput design: #3232).
class UlanziDialWindowsManager : public QObject {
    Q_OBJECT
public:
    explicit UlanziDialWindowsManager(QObject* parent = nullptr);
    ~UlanziDialWindowsManager() override;

    void start();
    void stop();
    // Re-announce the current state through stateReported() for the mapper
    // dialog, which opens long after the edges it would otherwise have heard.
    // Deliberately NOT connectionChanged(): that edge also drives the dial's
    // TX disconnect fence, and a re-report must never look like a detach.
    void reportState();

    bool isConnected() const { return !m_devices.isEmpty(); }
    QString deviceName() const { return m_deviceName; }

signals:
    void tuneSteps(int steps);
    void buttonEvent(const QString& signature, int action);
    void connectionChanged(bool connected, const QString& name);
    // Current state, emitted only from reportState(). Not an edge.
    void stateReported(bool connected, const QString& name);

private slots:
    void poll();
    void hotplugCheck();

private:
    struct OpenDevice {
        void* handle{nullptr};        // hid_device*; void* to avoid leaking <hidapi.h> here
        QString path;
        QString productString;
        QVector<unsigned char> lastReport;
    };

    bool rescan();                      // returns true if at least one device is open
    void closeAll();
    void handleReport(OpenDevice& dev, const unsigned char* data, int len);

    // Chord assembly — mirrors EvdevEncoderManager's logic but operates
    // on HID-usage-code-derived keycodes.
    void emitKeyTransition(int linuxKeycode, int value);

    QVector<OpenDevice> m_devices;
    QString m_deviceName;
    QTimer* m_pollTimer{nullptr};
    QTimer* m_hotplugTimer{nullptr};

    // Chord assembly and signature formatting are shared with the Linux and
    // macOS backends (ulanzi_chord_decoder_test covers all three).
    UlanziChordDecoder m_decoder;

    static constexpr int POLL_INTERVAL_MS    = 5;
    static constexpr int HOTPLUG_INTERVAL_MS = 3000;
};

} // namespace AetherSDR

#endif // Q_OS_WIN && HAVE_HIDAPI
