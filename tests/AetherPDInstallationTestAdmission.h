#pragma once

#include "models/AetherPDInstallationModel.h"
#include <QHash>
#include <memory>

namespace AetherSDR {
// Same application-thread admission contract without OS lock files. The real
// shared-UID backend is qualified separately with disposable fixture processes.
class AetherPDInstallationTestAccess {
public:
    static std::shared_ptr<void> acquire(const QString& serial)
    {
        static QHash<QString, std::weak_ptr<void>> leases;
        if (!leases.value(serial).expired()) { return {}; }
        const std::shared_ptr<void> held = std::make_shared<int>(0);
        leases.insert(serial, held);
        return held;
    }
    static void inject(AetherPDInstallationModel& model)
    {
        model.m_mutationAuthority = acquire;
    }
};
} // namespace AetherSDR
