#include "core/backends/OfflineHealthSource.h"

#include <QDebug>

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
    // LAST WINS, and it says so. The header calls a double declaration a
    // programming error, which is only useful if it is visible: two registrars
    // for one family resolve by static-initialisation order, so which one a
    // build gets depends on link order and changes with no source edit. That is
    // the exact class of silent failure the LINKAGE note says this design must
    // avoid, so it is a warning rather than a comment.
    //
    // Not an assert: a test that deliberately substitutes a double for a family
    // is a legitimate caller, and the warning is the right amount of noise for
    // it.
    if (table().contains(family.toLower()))
        qWarning() << "OfflineHealthRegistry: family" << family.toLower()
                   << "was already declared; the later declaration wins";
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
