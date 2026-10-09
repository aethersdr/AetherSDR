#include "core/GoClusterSettings.h"
#include "core/AppSettings.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace AetherSDR {

namespace {

const QString kRootKey = QStringLiteral("GoCluster");
const QString kFieldHideUnverified = QStringLiteral("hideUnverified");

QJsonObject readObj()
{
    const QString json = AppSettings::instance().value(kRootKey, QString{}).toString();
    if (json.isEmpty()) {
        return {};
    }
    return QJsonDocument::fromJson(json.toUtf8()).object();
}

} // namespace

bool GoClusterSettings::hideUnverified(const QString& feed)
{
    return readObj().value(kFieldHideUnverified).toObject().value(feed).toBool(false);
}

void GoClusterSettings::setHideUnverified(const QString& feed, bool on)
{
    // Read-modify-write the whole object so every feed's flag is persisted
    // together.
    QJsonObject o = readObj();
    QJsonObject hide = o.value(kFieldHideUnverified).toObject();
    hide[feed] = on;
    o[kFieldHideUnverified] = hide;
    auto& s = AppSettings::instance();
    s.setValue(kRootKey, QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
    s.save();
}

} // namespace AetherSDR
