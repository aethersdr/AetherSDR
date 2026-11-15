#include "CallsignInfo.h"

#include <QJsonValue>
#include <QStringList>

namespace AetherSDR {

QString CallsignInfo::displayName() const
{
    if (!nameFmt.isEmpty())
        return nameFmt;
    const QString joined = QStringList{firstName, lastName}.join(QLatin1Char(' ')).trimmed();
    if (!joined.isEmpty())
        return joined;
    return call;
}

QString CallsignInfo::displayLocation() const
{
    QStringList parts;
    if (!city.isEmpty())    parts << city;
    if (!state.isEmpty())   parts << state;
    if (!country.isEmpty()) parts << country;
    return parts.join(QStringLiteral(", "));
}

QJsonObject CallsignInfo::toJson() const
{
    QJsonObject o;
    o[QStringLiteral("call")]      = call;
    o[QStringLiteral("fname")]     = firstName;
    o[QStringLiteral("lname")]     = lastName;
    o[QStringLiteral("nameFmt")]   = nameFmt;
    o[QStringLiteral("nickname")]  = nickname;
    o[QStringLiteral("city")]      = city;
    o[QStringLiteral("state")]     = state;
    o[QStringLiteral("country")]   = country;
    o[QStringLiteral("county")]    = county;
    o[QStringLiteral("grid")]      = grid;
    o[QStringLiteral("class")]     = licenseClass;
    o[QStringLiteral("email")]     = email;
    o[QStringLiteral("url")]       = url;
    o[QStringLiteral("imageUrl")]  = imageUrl;
    if (hasLatLon) {
        o[QStringLiteral("lat")] = latitude;
        o[QStringLiteral("lon")] = longitude;
    }
    o[QStringLiteral("lotw")]      = lotw;
    o[QStringLiteral("eqsl")]      = eqsl;
    o[QStringLiteral("mqsl")]      = mailQsl;
    o[QStringLiteral("fetched")]   = static_cast<double>(fetchedUtc);
    return o;
}

CallsignInfo CallsignInfo::fromJson(const QJsonObject& o)
{
    CallsignInfo info;
    info.call         = o.value(QLatin1String("call")).toString();
    info.firstName    = o.value(QLatin1String("fname")).toString();
    info.lastName     = o.value(QLatin1String("lname")).toString();
    info.nameFmt      = o.value(QLatin1String("nameFmt")).toString();
    info.nickname     = o.value(QLatin1String("nickname")).toString();
    info.city         = o.value(QLatin1String("city")).toString();
    info.state        = o.value(QLatin1String("state")).toString();
    info.country      = o.value(QLatin1String("country")).toString();
    info.county       = o.value(QLatin1String("county")).toString();
    info.grid         = o.value(QLatin1String("grid")).toString();
    info.licenseClass = o.value(QLatin1String("class")).toString();
    info.email        = o.value(QLatin1String("email")).toString();
    info.url          = o.value(QLatin1String("url")).toString();
    info.imageUrl     = o.value(QLatin1String("imageUrl")).toString();
    info.hasLatLon    = o.contains(QLatin1String("lat")) && o.contains(QLatin1String("lon"));
    if (info.hasLatLon) {
        info.latitude  = o.value(QLatin1String("lat")).toDouble();
        info.longitude = o.value(QLatin1String("lon")).toDouble();
    }
    info.lotw       = o.value(QLatin1String("lotw")).toBool();
    info.eqsl       = o.value(QLatin1String("eqsl")).toBool();
    info.mailQsl    = o.value(QLatin1String("mqsl")).toBool();
    info.fetchedUtc = static_cast<qint64>(o.value(QLatin1String("fetched")).toDouble());
    return info;
}

} // namespace AetherSDR
