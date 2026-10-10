#pragma once
#include <QtGlobal>
#ifdef Q_OS_MACOS

#include "core/UlanziChordDecoder.h"

#include <QJsonObject>
#include <QObject>
#include <QString>

namespace AetherSDR {

// macOS Ulanzi Dial backend (IOKit HID Manager), same signal contract as the
// Linux evdev / Windows hidapi backends. Seizes the device
// (kIOHIDOptionsTypeSeizeDevice) so media keys don't reach the focused window;
// if the exclusive claim is denied, reopens shared and temporarily suppresses
// that service's system key mapping.
class UlanziDialMacOSManager : public QObject {
    Q_OBJECT
public:
    explicit UlanziDialMacOSManager(QObject* parent = nullptr);
    ~UlanziDialMacOSManager() override;

    void start();
    void stop();
    // Re-announce the current state through stateReported() for the mapper
    // dialog, which opens long after the edges it would otherwise have heard.
    // Deliberately NOT connectionChanged(): that edge also drives the dial's
    // TX disconnect fence, and a re-report must never look like a detach.
    void reportState();

    bool isConnected() const { return m_anyOpen; }
    QString deviceName() const { return m_deviceName; }
    QJsonObject diagnostics() const;

signals:
    void tuneSteps(int steps);
    void buttonEvent(const QString& signature, int action);
    void connectionChanged(bool connected, const QString& name);
    // Current state, emitted only from reportState(). Not an edge.
    void stateReported(bool connected, const QString& name);

private:
    friend struct UlanziDialMacOSManagerTestAccess;

    enum class AccessMode {
        None,
        Exclusive,
        Shared,
    };

    // Wired from the IOKit C callback shims.  Each call is one usage
    // transition (page + usage + value).
    void onHidValue(int usagePage, int usage, int value);
    void onDeviceMatching(const QString& productName);
    void onDeviceRemoval();
    void startPresenceWatch();
    void stopPresenceWatch();

    // Chord assembly state — same logic as the other two backends.
    void emitKeyTransition(int linuxKey, int value);
    QString accessModeName() const;
    void applySystemEventSuppression();
    void restoreSystemEventSuppression();
    void discardSystemEventSuppression();

    // Unopened probe manager whose matching callback announces a dial that
    // arrives after start(); start() then claims it. Null once claimed.
    void* m_presenceManager{nullptr};   // IOHIDManagerRef
    void* m_manager{nullptr};   // IOHIDManagerRef
    void* m_eventSystemClient{nullptr}; // IOHIDEventSystemClientRef
    void* m_suppressedService{nullptr}; // IOHIDServiceClientRef
    void* m_previousUserKeyMapping{nullptr}; // CFTypeRef
    QString m_deviceName;
    bool m_anyOpen{false};
    AccessMode m_accessMode{AccessMode::None};
    bool m_openAttempted{false};
    qint32 m_lastOpenResult{0};
    qint32 m_exclusiveOpenResult{0};
    bool m_sharedOpenAttempted{false};
    qint32 m_sharedOpenResult{0};
    bool m_systemEventsSuppressed{false};
    bool m_previousMappingPreserved{false};
    QString m_suppressionStatus{QStringLiteral("notNeeded")};
    QString m_restorationStatus{QStringLiteral("notNeeded")};

    // Chord assembly and signature formatting are shared with the Linux and
    // Windows backends (ulanzi_chord_decoder_test covers all three).
    UlanziChordDecoder m_decoder;

    // Static C-API callback shims forward to the instance via context.
    static void hidValueCb(void* ctx, int /*result*/, void* /*sender*/, void* value);
    static void devMatchedCb(void* ctx, int /*result*/, void* /*sender*/, void* device);
    static void devRemovedCb(void* ctx, int /*result*/, void* /*sender*/, void* device);
    static void presenceMatchedCb(void* ctx, int /*result*/, void* /*sender*/, void* device);
};

} // namespace AetherSDR

#endif // Q_OS_MACOS
