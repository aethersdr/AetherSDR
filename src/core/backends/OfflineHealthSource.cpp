#include "core/backends/OfflineHealthSource.h"

namespace AetherSDR {

QHash<QString, OfflineHealthRegistry::Factory>& OfflineHealthRegistry::table()
{
    // Function-local static, so a registrar running during static
    // initialisation cannot race the table's own construction. A namespace-
    // scope QHash here would be an initialisation-order bug that appears only
    // when the link order changes.
    static QHash<QString, Factory> t;
    return t;
}

void OfflineHealthRegistry::declare(const QString& family, Factory make)
{
    if (family.isEmpty() || !make)
        return;
    table().insert(family.toLower(), std::move(make));
}

bool OfflineHealthRegistry::declaredFor(const QString& family)
{
    return table().contains(family.toLower());
}

std::unique_ptr<IOfflineHealthSource>
OfflineHealthRegistry::create(const QString& family, QObject* parent)
{
    const auto it = table().constFind(family.toLower());
    if (it == table().constEnd())
        return nullptr;
    return (*it)(parent);
}

}  // namespace AetherSDR
