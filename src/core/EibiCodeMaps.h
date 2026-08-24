#pragma once

#include <QHash>
#include <QString>

namespace AetherSDR {

const QHash<QString, QString>& getEibiCountryMap();
const QHash<QString, QString>& getEibiLangMap();
const QHash<QString, QString>& getEibiSiteMap();

} // namespace AetherSDR
