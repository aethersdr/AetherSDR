#pragma once
#include <QtGlobal>
#if defined(Q_OS_WIN) && defined(HAVE_HIDAPI)

#include "core/UlanziChordDecoder.h"

#include <QMutex>
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
    // lets a dialog that opens after start() recover the state that
    // unsupportedVariantChanged already reported.  On Windows this object
    // lives on the ExtControllers thread while callers are on the GUI
    // thread, so the read is mutex-guarded rather than a bare member
    // access: rescan() rewrites m_variantSeen from the hotplug timer. (#3485)
    QString unsupportedVariantName() const;

signals:
    void tuneSteps(int steps);
    void buttonEvent(const QString& signature, int action);
    void connectionChanged(bool connected, const QString& name);
    // State change for a known Ulanzi OEM variant whose firmware cannot be
    // driven over this HID backend at all (vendor collection silent outside
    // Ulanzi Studio; keyboard/mouse collections OS-captured) — e.g. the
    // KEHWIN "Dial_Lite" D100H or the Zkswe D200.  Emitted with the variant
    // name when one appears and with an EMPTY string when it goes away
    // (unplug, or stop()), so consumers can withdraw the advisory instead of
    // claiming the device is still there. (#3485)
    void unsupportedVariantChanged(const QString& name);

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
    void publishVariantState();         // emit unsupportedVariantChanged on transitions only
    void closeAll();
    void handleReport(OpenDevice& dev, const unsigned char* data, int len);

    // Chord assembly — mirrors EvdevEncoderManager's logic but operates
    // on HID-usage-code-derived keycodes.
    void emitKeyTransition(int linuxKeycode, int value);

    QVector<OpenDevice> m_devices;
    QString m_deviceName;
    // Both members go through m_variantMutex.  m_variantSeen is written by
    // rescan()/stop() on the owning (Ext Controllers) thread and read by
    // unsupportedVariantName() from the GUI thread.  m_variantNotified is
    // only reached from the owning thread on Windows today — the one GUI
    // caller of stop(), the automation bridge's ulanzi-stop diagnostic at
    // MainWindow_Session.cpp, is inside #ifdef Q_OS_MAC and does not compile
    // here — but guarding it costs nothing (publishVariantState() already
    // holds the lock to read m_variantSeen) and stops the invariant from
    // depending on a call site in another platform's branch. (#3485)
    mutable QMutex m_variantMutex;
    QString m_variantSeen;          // OEM-variant display name from the last rescan
    QString m_variantNotified;      // last state we emitted, to emit on transitions only
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
