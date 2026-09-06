#pragma once
#include <QtGlobal>
#ifdef Q_OS_MAC

#include "core/UlanziChordDecoder.h"

#include <QJsonObject>
#include <QObject>
#include <QString>

namespace AetherSDR {

// macOS backend for the Ulanzi Dial using IOKit HID Manager.  Mirrors
// the Linux evdev / Windows hidapi backends' Qt signal contract.
//
// Key advantage over Windows: IOHIDManagerOpen with kIOHIDOptionsTypeSeizeDevice
// is the documented exclusive-claim mechanism on Darwin.  When seized,
// the dial's input is delivered only to AetherSDR — the OS keyboard
// stack stops receiving it — so the dial's media keys don't leak to the
// focused window. If macOS specifically denies the exclusive claim, the
// manager reopens the same matched device in shared mode and temporarily
// suppresses that service's system key mapping instead.
class UlanziDialMacOSManager : public QObject {
    Q_OBJECT
public:
    explicit UlanziDialMacOSManager(QObject* parent = nullptr);
    ~UlanziDialMacOSManager() override;

    void start();
    void stop();

    bool isConnected() const { return m_anyOpen; }
    QString deviceName() const { return m_deviceName; }
    QJsonObject diagnostics() const;

signals:
    void tuneSteps(int steps);
    void buttonEvent(const QString& signature, int action);
    void connectionChanged(bool connected, const QString& name);

private:
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

    // Chord assembly state — same logic as the other two backends.
    void emitKeyTransition(int linuxKey, int value);
    QString accessModeName() const;
    void applySystemEventSuppression();
    void restoreSystemEventSuppression();
    void discardSystemEventSuppression();

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
};

} // namespace AetherSDR

#endif // Q_OS_MAC
