#pragma once
#include <QtGlobal>
#if defined(Q_OS_WIN) && defined(HAVE_HIDAPI)

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

class QTimer;

namespace AetherSDR {

// Windows backend for the Ulanzi Dial using hidapi.  Mirrors the Linux
// evdev backend's Qt signal contract:
//   tuneSteps(int)               — rotary delta (+1 CW / -1 CCW)
//   buttonEvent(sig, action)     — see EvdevEncoderManager for signature
//   connectionChanged(bool, name)
//
// Implementation notes:
// - The dial enumerates as multiple HID interfaces on Windows: a Keyboard
//   page interface (Ctrl + chord keys), a Consumer Control page interface
//   (PLAYPAUSE / MUTE / NEXTSONG / PREVIOUSSONG), and possibly Mouse.
//   We open every matching device whose product_string contains
//   "Ulanzi Dial" and read reports from each via a poll timer.
// - Each report is diff-compared against the previous to detect press /
//   release transitions, then fed into the same chord-assembly state
//   machine the Linux backend uses.
// - No EVIOCGRAB-equivalent: keystrokes still go to the focused window
//   on Windows.  A follow-up could wire RawInput + RIDEV_NOLEGACY to
//   intercept globally; see #3232 for the design notes.
class UlanziDialWindowsManager : public QObject {
    Q_OBJECT
public:
    explicit UlanziDialWindowsManager(QObject* parent = nullptr);
    ~UlanziDialWindowsManager() override;

    void start();
    void stop();

    bool isConnected() const { return !m_devices.isEmpty(); }
    QString deviceName() const { return m_deviceName; }

signals:
    void tuneSteps(int steps);
    void buttonEvent(const QString& signature, int action);
    void connectionChanged(bool connected, const QString& name);

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

    bool m_ctrlDown{false};
    int  m_lastNonModKey{-1};
    bool m_prevsongAlongsideCtrl{false};

    static constexpr int POLL_INTERVAL_MS    = 5;
    static constexpr int HOTPLUG_INTERVAL_MS = 3000;
};

} // namespace AetherSDR

#endif // Q_OS_WIN && HAVE_HIDAPI
