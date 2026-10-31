#include "core/UpdateSettings.h"

#include "core/AppSettings.h"
#include "core/VersionNumber.h"

#include <QJsonDocument>
#include <QJsonValue>
#include <QString>

namespace AetherSDR {

namespace {
const QString kUpdatesKey = QStringLiteral("Updates");
const QString kCheckOnStartupField = QStringLiteral("checkOnStartup");
const QString kSkippedVersionField = QStringLiteral("skippedVersion");
const QString kLastCheckUtcField = QStringLiteral("lastCheckUtc");
const QString kLatestVersionField = QStringLiteral("latestVersion");
const QString kAcceptedManifestField = QStringLiteral("acceptedManifestUtc");
const QString kRolledBackVersionField = QStringLiteral("rolledBackVersion");
const QString kQuitSwitchVersionField = QStringLiteral("quitSwitchVersion");
const QString kQuitSwitchTriesField = QStringLiteral("quitSwitchTries");
} // namespace

QJsonObject UpdateSettings::readObj()
{
    const QString json =
        AppSettings::instance().value(kUpdatesKey, QString{}).toString();
    if (json.isEmpty()) {
        return {};
    }
    return QJsonDocument::fromJson(json.toUtf8()).object();
}

void UpdateSettings::write(const QJsonObject& o)
{
    auto& s = AppSettings::instance();
    s.setValue(kUpdatesKey,
               QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
    s.save();
}

bool UpdateSettings::checkOnStartup()
{
    return readObj().value(kCheckOnStartupField).toBool(true);
}

void UpdateSettings::setCheckOnStartup(bool enabled)
{
    QJsonObject o = readObj();
    o[kCheckOnStartupField] = enabled;
    write(o);
}

QString UpdateSettings::skippedVersion()
{
    return readObj().value(kSkippedVersionField).toString();
}

void UpdateSettings::setSkippedVersion(const QString& version)
{
    QJsonObject o = readObj();
    if (version.isEmpty()) {
        o.remove(kSkippedVersionField);
    } else {
        o[kSkippedVersionField] = version;
    }
    write(o);
}

bool UpdateSettings::isSkipped(const QString& version)
{
    const VersionNumber skipped = VersionNumber::parse(skippedVersion());
    return !skipped.isNull() && skipped == VersionNumber::parse(version);
}

QDateTime UpdateSettings::lastCheckUtc()
{
    return QDateTime::fromString(readObj().value(kLastCheckUtcField).toString(),
                                 Qt::ISODate);
}

void UpdateSettings::setLastCheckUtc(const QDateTime& when)
{
    QJsonObject o = readObj();
    o[kLastCheckUtcField] = when.toUTC().toString(Qt::ISODate);
    write(o);
}

QString UpdateSettings::latestVersion()
{
    return readObj().value(kLatestVersionField).toString();
}

void UpdateSettings::recordAnswer(const QDateTime& when, const QString& latestVersion)
{
    QJsonObject o = readObj();
    o[kLastCheckUtcField] = when.toUTC().toString(Qt::ISODate);
    o[kLatestVersionField] = latestVersion;
    write(o);
}

QDateTime UpdateSettings::acceptedManifestUtc()
{
    return QDateTime::fromString(readObj().value(kAcceptedManifestField).toString(), Qt::ISODate);
}

void UpdateSettings::setAcceptedManifestUtc(const QDateTime& generatedAt)
{
    QJsonObject o = readObj();
    o[kAcceptedManifestField] = generatedAt.toUTC().toString(Qt::ISODate);
    write(o);
}

QString UpdateSettings::rolledBackVersion()
{
    return readObj().value(kRolledBackVersionField).toString();
}

void UpdateSettings::setRolledBackVersion(const QString& version)
{
    QJsonObject o = readObj();
    o[kRolledBackVersionField] = VersionNumber::parse(version).isNull()
                                     ? QString()
                                     : VersionNumber::parse(version).toString();
    write(o);
}

int UpdateSettings::quitSwitchTries(const QString& version)
{
    const QJsonObject o = readObj();
    return o.value(kQuitSwitchVersionField).toString() == version ? o.value(kQuitSwitchTriesField).toInt() : 0;
}

void UpdateSettings::setQuitSwitchTries(const QString& version, int tries)
{
    QJsonObject o = readObj();
    o[kQuitSwitchVersionField] = version;
    o[kQuitSwitchTriesField] = tries;
    write(o);
}

bool UpdateSettings::automaticCheckDue(const QDateTime& nowUtc)
{
    if (!checkOnStartup()) {
        return false;
    }
    const QDateTime last = lastCheckUtc();
    if (!last.isValid() || last > nowUtc) {
        return true;
    }
    return last.secsTo(nowUtc) >= kAutomaticCheckIntervalHours * 3600;
}

} // namespace AetherSDR
