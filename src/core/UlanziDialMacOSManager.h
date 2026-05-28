#pragma once
#include <QtGlobal>
#ifdef Q_OS_MAC

#include <QObject>
#include <QString>

namespace AetherSDR {

// macOS backend for the Ulanzi Dial using IOKit HID Manager.  Mirrors
// the Linux evdev / Windows hidapi backends' Qt signal contract.
//
// Key advantage over Windows: IOHIDDeviceOpen with kIOHIDOptionsTypeSeizeDevice
// is the documented exclusive-claim mechanism on Darwin.  When seized,
// the dial's input is delivered only to AetherSDR — the OS keyboard
// stack stops receiving it — so the dial's media keys don't leak to the
// focused window.
class UlanziDialMacOSManager : public QObject {
    Q_OBJECT
public:
    explicit UlanziDialMacOSManager(QObject* parent = nullptr);
    ~UlanziDialMacOSManager() override;

    void start();
    void stop();

    bool isConnected() const { return m_anyOpen; }
    QString deviceName() const { return m_deviceName; }

signals:
    void tuneSteps(int steps);
    void buttonEvent(const QString& signature, int action);
    void connectionChanged(bool connected, const QString& name);

private:
    // Wired from the IOKit C callback shims.  Each call is one usage
    // transition (page + usage + value).
    void onHidValue(int usagePage, int usage, int value);
    void onDeviceMatching(const QString& productName);
    void onDeviceRemoval();

    // Chord assembly state — same logic as the other two backends.
    void emitKeyTransition(int linuxKey, int value);

    void* m_manager{nullptr};   // IOHIDManagerRef
    QString m_deviceName;
    bool m_anyOpen{false};

    bool m_ctrlDown{false};
    int  m_lastNonModKey{-1};
    bool m_prevsongAlongsideCtrl{false};

    // Static C-API callback shims forward to the instance via context.
    static void hidValueCb(void* ctx, int /*result*/, void* /*sender*/, void* value);
    static void devMatchedCb(void* ctx, int /*result*/, void* /*sender*/, void* device);
    static void devRemovedCb(void* ctx, int /*result*/, void* /*sender*/, void* device);
};

} // namespace AetherSDR

#endif // Q_OS_MAC
