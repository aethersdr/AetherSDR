#include "core/UlanziDialMacOSManager.h"

#include <QCoreApplication>
#include <QEvent>
#include <QJsonObject>
#include <QJsonValue>
#include <QLoggingCategory>

#include <iostream>

namespace AetherSDR {

Q_LOGGING_CATEGORY(lcDevices, "aether.devices")

struct UlanziDialMacOSManagerTestAccess {
    static void watch(UlanziDialMacOSManager& manager)
    {
        manager.startPresenceWatch();
    }

    static void arrive(UlanziDialMacOSManager& manager)
    {
        manager.presenceMatchedCb(&manager, 0, nullptr, nullptr);
    }

    static bool watching(const UlanziDialMacOSManager& manager)
    {
        return manager.m_presenceManager != nullptr;
    }
};

} // namespace AetherSDR

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    AetherSDR::UlanziDialMacOSManager manager;
    const QJsonObject inventory = manager.diagnostics();
    if (!inventory.value("inventoryAvailable").toBool()
        || inventory.value("matchedCount").toInt() != 0) {
        std::cout << "SKIP: requires a readable inventory with no Ulanzi Dial attached\n";
        return 77;
    }

    using Access = AetherSDR::UlanziDialMacOSManagerTestAccess;
    Access::watch(manager);
    if (!Access::watching(manager)) {
        std::cout << "FAIL: presence watcher was not created\n";
        return 1;
    }
    Access::arrive(manager);
    manager.stop();
    QCoreApplication::sendPostedEvents(&manager, QEvent::MetaCall);
    const bool stopped = !Access::watching(manager)
        && !manager.diagnostics().value("openAttempted").toBool();
    std::cout << (stopped ? "PASS" : "FAIL")
              << ": queued arrival cannot restart a stopped watcher\n";
    return stopped ? 0 : 1;
}
