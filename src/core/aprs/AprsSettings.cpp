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

namespace {

QString truthy(bool on)
{
    return on ? QStringLiteral("True") : QStringLiteral("False");
}

bool readFlag(const QJsonObject& o, const char* key, bool def = false)
{
    const QString v = o.value(QLatin1String(key))
                          .toString(def ? QStringLiteral("True")
                                        : QStringLiteral("False"));
    return v == QLatin1String("True");
}

} // namespace

QString AprsSettings::digiCall()
{
    return readObj().value(QStringLiteral("digiCall")).toString();
}

QString AprsSettings::digiAlias()
{
    const QString a = readObj().value(QStringLiteral("digiAlias")).toString();
    return a.isEmpty() ? QStringLiteral("WIDE1-1") : a;
}

bool AprsSettings::digiAlsoMyCall()
{
    return readFlag(readObj(), "digiAlsoMyCall", true);
}

bool AprsSettings::digiAlsoRelay()
{
    return readFlag(readObj(), "digiAlsoRelay");
}

int AprsSettings::digiDupeWindowSecs()
{
    const int s = readObj().value(QStringLiteral("digiDupeSecs"))
                      .toString(QStringLiteral("30")).toInt();
    return qBound(5, s > 0 ? s : 30, 300);
}

bool AprsSettings::digiBeaconEnabled()
{
    return readFlag(readObj(), "digiBeaconEnabled");
}

int AprsSettings::digiBeaconIntervalMinutes()
{
    const int m = readObj().value(QStringLiteral("digiBeaconIntervalMin"))
                      .toString(QStringLiteral("15")).toInt();
    return qBound(1, m > 0 ? m : 15, 24 * 60);
}

QString AprsSettings::digiBeaconText()
{
    return readObj().value(QStringLiteral("digiBeaconText"))
        .toString(QStringLiteral("AetherDigi online (AX.25)"));
}

QString AprsSettings::digiBeaconPath()
{
    return readObj().value(QStringLiteral("digiBeaconPath"))
        .toString(QStringLiteral("WIDE2-1"));
}

QString AprsSettings::digiBeaconSymbol()
{
    const QString sym = readObj().value(QStringLiteral("digiBeaconSymbol")).toString();
    return sym.size() == 2 ? sym : QStringLiteral("\\#");
}

void AprsSettings::setDigiCall(const QString& call)
{
    setString("digiCall", call.trimmed().toUpper());
}

void AprsSettings::setDigiAlias(const QString& alias)
{
    setString("digiAlias", alias.trimmed().toUpper());
}

void AprsSettings::setDigiAlsoMyCall(bool on)
{
    setString("digiAlsoMyCall", truthy(on));
}

void AprsSettings::setDigiAlsoRelay(bool on)
{
    setString("digiAlsoRelay", truthy(on));
}

void AprsSettings::setDigiDupeWindowSecs(int secs)
{
    setString("digiDupeSecs", QString::number(qBound(5, secs, 300)));
}

void AprsSettings::setDigiBeaconEnabled(bool on)
{
    setString("digiBeaconEnabled", truthy(on));
}

void AprsSettings::setDigiBeaconIntervalMinutes(int minutes)
{
    setString("digiBeaconIntervalMin",
              QString::number(qBound(1, minutes, 24 * 60)));
}

void AprsSettings::setDigiBeaconText(const QString& text)
{
    setString("digiBeaconText", text.trimmed());
}

void AprsSettings::setDigiBeaconPath(const QString& path)
{
    setString("digiBeaconPath", path.trimmed().toUpper());
}

void AprsSettings::setDigiBeaconSymbol(const QString& tableAndCode)
{
    setString("digiBeaconSymbol",
              tableAndCode.size() == 2 ? tableAndCode : QStringLiteral("\\#"));
}

} // namespace AetherSDR
