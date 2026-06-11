#include "core/aprs/AprsSettings.h"

#include "core/AppSettings.h"

#include <QJsonDocument>
#include <QtGlobal>

namespace AetherSDR {

namespace {
const QString kAprsSettingsKey = QStringLiteral("AetherModemAprs");
} // namespace

QJsonObject AprsSettings::readObj()
{
    const QString json =
        AppSettings::instance().value(kAprsSettingsKey, QString{}).toString();
    if (json.isEmpty())
        return {};
    return QJsonDocument::fromJson(json.toUtf8()).object();
}

void AprsSettings::write(const QJsonObject& o)
{
    auto& s = AppSettings::instance();
    s.setValue(kAprsSettingsKey,
               QString::fromUtf8(
                   QJsonDocument(o).toJson(QJsonDocument::Compact)));
    s.save();
}

void AprsSettings::setString(const char* key, const QString& value)
{
    QJsonObject o = readObj();
    o[QLatin1String(key)] = value;
    write(o);
}

QString AprsSettings::myCall()
{
    return readObj().value(QStringLiteral("myCall")).toString();
}

bool AprsSettings::modemAutostart()
{
    return readObj().value(QStringLiteral("modemAutostart"))
               .toString(QStringLiteral("False")) == QLatin1String("True");
}

bool AprsSettings::beaconEnabled()
{
    return readObj().value(QStringLiteral("beaconEnabled"))
               .toString(QStringLiteral("False")) == QLatin1String("True");
}

int AprsSettings::beaconIntervalMinutes()
{
    const int minutes = readObj().value(QStringLiteral("beaconIntervalMin"))
        .toString(QString::number(kDefaultBeaconIntervalMin)).toInt();
    return qBound(1, minutes > 0 ? minutes : kDefaultBeaconIntervalMin, 24 * 60);
}

QString AprsSettings::beaconText()
{
    return readObj().value(QStringLiteral("beaconText"))
        .toString(QStringLiteral("AetherSDR"));
}

QString AprsSettings::symbol()
{
    const QString sym = readObj().value(QStringLiteral("symbol")).toString();
    return sym.size() == 2 ? sym : QStringLiteral("/-");
}

QString AprsSettings::path()
{
    return readObj().value(QStringLiteral("path"))
        .toString(QStringLiteral("WIDE1-1,WIDE2-1"));
}

QString AprsSettings::manualLat()
{
    return readObj().value(QStringLiteral("manualLat")).toString();
}

QString AprsSettings::manualLon()
{
    return readObj().value(QStringLiteral("manualLon")).toString();
}

void AprsSettings::setMyCall(const QString& call)
{
    setString("myCall", call.trimmed().toUpper());
}

void AprsSettings::setModemAutostart(bool on)
{
    setString("modemAutostart",
              on ? QStringLiteral("True") : QStringLiteral("False"));
}

void AprsSettings::setBeaconEnabled(bool on)
{
    setString("beaconEnabled",
              on ? QStringLiteral("True") : QStringLiteral("False"));
}

void AprsSettings::setBeaconIntervalMinutes(int minutes)
{
    setString("beaconIntervalMin", QString::number(qBound(1, minutes, 24 * 60)));
}

void AprsSettings::setBeaconText(const QString& text)
{
    setString("beaconText", text.trimmed());
}

void AprsSettings::setSymbol(const QString& tableAndCode)
{
    setString("symbol",
              tableAndCode.size() == 2 ? tableAndCode : QStringLiteral("/-"));
}

void AprsSettings::setPath(const QString& path)
{
    setString("path", path.trimmed().toUpper());
}

void AprsSettings::setManualPosition(const QString& lat, const QString& lon)
{
    QJsonObject o = readObj();
    o[QStringLiteral("manualLat")] = lat.trimmed();
    o[QStringLiteral("manualLon")] = lon.trimmed();
    write(o);
}

} // namespace AetherSDR
