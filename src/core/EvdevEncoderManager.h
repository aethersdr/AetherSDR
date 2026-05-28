#pragma once
#include <QtGlobal>
#ifdef Q_OS_LINUX

#include <QObject>
#include <QString>
#include <QStringList>

class QSocketNotifier;
class QFileSystemWatcher;
class QTimer;

namespace AetherSDR {

// Linux evdev encoder manager — opens a /dev/input/event* node belonging
// to a recognized BT-HID encoder (Ulanzi Dial first), claims it
// exclusively via EVIOCGRAB so its keystrokes don't leak to the focused
// window, and decodes input_events into semantic signals.
//
// Signal contract is similar to HidEncoderManager (#3232):
//   - tuneSteps(int)            — rotary delta, +1 = CW, -1 = CCW
//   - buttonEvent(sig, action)  — raw event "signature" string for the
//                                 dialog's Learn mode + action dispatch.
//                                 action: 1 = press, 0 = release.
//   - connectionChanged(bool, name)
//
// Signatures are normalized human-readable strings derived from kernel
// key codes:
//   "KEY_PLAYPAUSE", "KEY_MUTE", "KEY_PREVIOUSSONG", "KEY_NEXTSONG",
//   "Ctrl+V", "Ctrl+C", "Ctrl+Y", "Ctrl+Z",
//   "Ctrl+Y+KEY_PREVIOUSSONG"   (compound chord — Mode Cycle on Ulanzi Dial)
//
// The decoder doesn't pre-bake action semantics — the UI maps signatures
// to AetherSDR actions, so adding a new dial firmware or remapping
// requires no manager changes.
class EvdevEncoderManager : public QObject {
    Q_OBJECT

public:
    explicit EvdevEncoderManager(QObject* parent = nullptr);
    ~EvdevEncoderManager() override;

    void start();        // begin scanning + watching for hot-plug
    void stop();         // release grab + close fd

    bool isConnected() const { return m_fd >= 0; }
    QString deviceName() const { return m_deviceName; }
    QString devicePath() const { return m_devicePath; }

signals:
    void tuneSteps(int steps);
    void buttonEvent(const QString& signature, int action);
    void connectionChanged(bool connected, const QString& name);

private slots:
    void onReadable();
    void onInputDirChanged();

private:
    // Scan /dev/input/event* for a device matching one of our name patterns.
    // Returns the path of the first match, or empty string if none found.
    QString findMatchingDevice() const;
    bool openAndGrab(const QString& path);
    void closeFd();
    void emitChord(int keycode, int action);  // chord assembly + signature emit

    int m_fd{-1};
    QString m_devicePath;
    QString m_deviceName;
    QSocketNotifier* m_notifier{nullptr};
    QFileSystemWatcher* m_watcher{nullptr};
    QTimer* m_rescanTimer{nullptr};  // debounced rescan after directory change

    // Decoder state — Ulanzi Dial encodes some buttons as Ctrl+key chords
    // and one as a Ctrl+Y+KEY_PREVIOUSSONG compound. We assemble chords
    // by tracking modifier-down/up around non-modifier presses.
    bool m_ctrlDown{false};
    int m_lastNonModKey{-1};        // most recent non-mod key while Ctrl held
    bool m_prevsongAlongsideCtrl{false};  // KEY_PREVIOUSSONG present in chord window
};

} // namespace AetherSDR

#endif // Q_OS_LINUX
