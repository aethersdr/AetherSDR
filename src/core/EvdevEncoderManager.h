#pragma once
#include <QtGlobal>
#ifdef Q_OS_LINUX

#include "core/UlanziChordDecoder.h"

#include <QObject>
#include <QString>
#include <QStringList>

class QSocketNotifier;
class QFileSystemWatcher;
class QTimer;

namespace AetherSDR {

// Linux evdev manager for recognized BT-HID encoders (Ulanzi Dial first): grabs
// the /dev/input/event* node with EVIOCGRAB so keys don't leak to the focused
// window. Signals (as HidEncoderManager, #3232):
//   - tuneSteps(int)           rotary delta, +1 = CW
//   - buttonEvent(sig, action) signature string; action 1 = press, 0 = release
//   - connectionChanged(bool, name)
// Signatures are key-code names: "KEY_PLAYPAUSE", "KEY_MUTE", "Ctrl+V", ...,
// "Ctrl+Y+KEY_PREVIOUSSONG" (chord: Mode Cycle on Ulanzi Dial). The UI maps them
// to actions, so new firmware needs no manager change.
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
    // Emitted when a recognized dial is present but its /dev/input node can't
    // be opened (EACCES) — i.e. the udev access rule isn't installed. The UI
    // uses this to offer a one-click, polkit-authenticated rule install.
    void accessRequired(const QString& deviceName);

private slots:
    void onReadable();
    void onInputDirChanged();

private:
    // Scan /dev/input/event* for a device matching one of our name patterns.
    // Returns the path of the first openable match, or empty string if none.
    // If a name-matching device is found but its node can't be opened
    // (EACCES), its name is written to *blockedName (when non-null).
    QString findMatchingDevice(QString* blockedName = nullptr) const;
    bool openAndGrab(const QString& path);
    void closeFd();

    int m_fd{-1};
    QString m_devicePath;
    QString m_deviceName;
    QSocketNotifier* m_notifier{nullptr};
    QFileSystemWatcher* m_watcher{nullptr};
    QTimer* m_rescanTimer{nullptr};  // debounced rescan after directory change
    bool m_accessRequiredEmitted{false};  // de-dupe accessRequired across rescans

    // Chord assembly and signature formatting are shared with the macOS and
    // Windows backends (ulanzi_chord_decoder_test covers all three).
    UlanziChordDecoder m_decoder;
};

} // namespace AetherSDR

#endif // Q_OS_LINUX
