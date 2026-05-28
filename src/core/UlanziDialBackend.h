#pragma once
// Per-platform alias for the Ulanzi Dial backend manager.  Each
// concrete implementation exposes the same Qt signal contract
// (tuneSteps / buttonEvent / connectionChanged) and the same
// start / stop / isConnected / deviceName surface, so consumers (the
// mapper dialog and MainWindow's dispatcher) only need this alias
// and do not have to switch on platform themselves.

#include <QtGlobal>

#ifdef Q_OS_LINUX
    #include "core/EvdevEncoderManager.h"
    namespace AetherSDR { using UlanziDialBackend = EvdevEncoderManager; }
#elif defined(Q_OS_WIN) && defined(HAVE_HIDAPI)
    #include "core/UlanziDialWindowsManager.h"
    namespace AetherSDR { using UlanziDialBackend = UlanziDialWindowsManager; }
#elif defined(Q_OS_MAC)
    #include "core/UlanziDialMacOSManager.h"
    namespace AetherSDR { using UlanziDialBackend = UlanziDialMacOSManager; }
#else
    // Fallback (e.g. Windows builds without hidapi): a no-op backend
    // that just sits there.  Keeps the mapper dialog and MainWindow
    // dispatcher compilable without per-platform branches at the call
    // site.  Live backend support requires a real implementation.
    #include <QObject>
    #include <QString>
    namespace AetherSDR {
    class UlanziDialBackend : public QObject {
        Q_OBJECT
    public:
        explicit UlanziDialBackend(QObject* parent = nullptr) : QObject(parent) {}
        void start() {}
        void stop()  {}
        bool isConnected() const { return false; }
        QString deviceName() const { return {}; }
    signals:
        void tuneSteps(int steps);
        void buttonEvent(const QString& signature, int action);
        void connectionChanged(bool connected, const QString& name);
    };
    } // namespace AetherSDR
#endif
