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
    // Non-empty while an unsupported OEM variant is the only dial present —
    // lets a dialog that opens after start() recover the state the one-shot
    // unsupportedVariantDetected signal already reported. (#3485)
    QString unsupportedVariantName() const { return m_variantSeen; }

signals:
    void tuneSteps(int steps);
    void buttonEvent(const QString& signature, int action);
    void connectionChanged(bool connected, const QString& name);
    // A known Ulanzi OEM variant is present whose firmware cannot be driven
    // over this HID backend at all (vendor collection silent outside Ulanzi
    // Studio; keyboard/mouse collections OS-captured) — e.g. the KEHWIN
    // "Dial_Lite" D100H or the Zkswe D200. Consumers should point the user
    // at the bundled Ulanzi Studio plugin instead of showing a bare
    // "Disconnected". (#3485)
    void unsupportedVariantDetected(const QString& name);

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
    void notifyVariantIfSeen();         // emit unsupportedVariantDetected once per variant
    void closeAll();
    void handleReport(OpenDevice& dev, const unsigned char* data, int len);

    // Chord assembly — mirrors EvdevEncoderManager's logic but operates
    // on HID-usage-code-derived keycodes.
    void emitKeyTransition(int linuxKeycode, int value);

    QVector<OpenDevice> m_devices;
    QString m_deviceName;
    QString m_variantSeen;          // OEM-variant display name from the last rescan
    QString m_variantNotified;      // last variant we emitted for, to avoid re-spamming
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
