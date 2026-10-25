#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/UpdateChecker.h"
#include "core/UpdateSettings.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>

// The Updates document (RFC #5304): the automatic-check default, the
// once-a-day throttle, skip matching, the known update restored across a
// restart, and that every field shares one nested object under one key.
int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("update-settings"));
    if (!profile.isValid()) { return 1; }
    QCoreApplication app(argc, argv);
    using namespace AetherSDR;
    AppSettings& settings = AppSettings::instance();
    settings.load();
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        if (!ok) { qCritical() << "FAIL:" << label; ++failures; }
    };

    const QDateTime now = QDateTime::fromString(
        QStringLiteral("2026-10-23T12:00:00Z"), Qt::ISODate);

    check(UpdateSettings::checkOnStartup(), "automatic check defaults on");
    check(!UpdateSettings::lastCheckUtc().isValid(), "no answer recorded yet");
    check(UpdateSettings::automaticCheckDue(now), "due when never checked");

    UpdateSettings::setLastCheckUtc(now.addSecs(-3600));
    check(!UpdateSettings::automaticCheckDue(now), "not due one hour after an answer");
    UpdateSettings::setLastCheckUtc(now.addSecs(-24 * 3600 + 1));
    check(!UpdateSettings::automaticCheckDue(now), "not due one second short of a day");
    UpdateSettings::setLastCheckUtc(now.addSecs(-24 * 3600));
    check(UpdateSettings::automaticCheckDue(now), "due a full day after an answer");
    UpdateSettings::setLastCheckUtc(now.addSecs(3600));
    check(UpdateSettings::automaticCheckDue(now), "due when the last answer is in the future");

    UpdateSettings::setLastCheckUtc(now.addSecs(-3600));
    settings.load();
    check(UpdateSettings::lastCheckUtc() == now.addSecs(-3600), "last answer survives reload");

    UpdateSettings::setLastCheckUtc(now.addSecs(-48 * 3600));
    UpdateSettings::setCheckOnStartup(false);
    check(!UpdateSettings::automaticCheckDue(now), "never due when turned off");
    settings.load();
    check(!UpdateSettings::checkOnStartup(), "off survives reload");
    UpdateSettings::setCheckOnStartup(true);
    check(UpdateSettings::automaticCheckDue(now), "due again when turned back on");

    check(!UpdateSettings::isSkipped(QStringLiteral("26.10.4")), "nothing skipped by default");
    UpdateSettings::setSkippedVersion(QStringLiteral("26.10.4"));
    check(UpdateSettings::isSkipped(QStringLiteral("26.10.4")), "skipped release matches");
    check(UpdateSettings::isSkipped(QStringLiteral("v26.10.4")), "skip ignores a leading v");
    check(!UpdateSettings::isSkipped(QStringLiteral("26.10.5")), "a newer release is not skipped");
    check(!UpdateSettings::isSkipped(QStringLiteral("26.10.4.1")), "a hotfix is not skipped");
    check(!UpdateSettings::isSkipped(QString()), "an empty version is never skipped");
    settings.load();
    check(UpdateSettings::skippedVersion() == QLatin1String("26.10.4"), "skip survives reload");
    UpdateSettings::setSkippedVersion(QString());
    check(!UpdateSettings::isSkipped(QStringLiteral("26.10.4")), "clearing the skip");

    // A known update survives a restart inside the throttle (no new request),
    // and the badge rule drops it once skipped or once the running version
    // catches up. The store branch needs a packaged build; not reachable here.
    UpdateSettings::setSkippedVersion(QString());
    UpdateSettings::recordAnswer(now, QStringLiteral("26.10.3"));
    settings.load();
    check(!UpdateSettings::automaticCheckDue(now.addSecs(3600)),
          "restart inside the throttle makes no request");
    check(UpdateSettings::latestVersion() == QLatin1String("26.10.3"),
          "latest release survives reload");
    QCoreApplication::setApplicationVersion(QStringLiteral("26.10.2"));
    check(UpdateChecker::knownBadgeVersion() == QLatin1String("26.10.3"),
          "restored badge names the known newer release");
    check(UpdateChecker::badgeVersion(QStringLiteral("v26.10.3<b>x"), QStringLiteral("26.10.2"))
              == QLatin1String("26.10.3"),
          "badge shows parsed digits, never the raw tag");
    // The automatic-check toggle governs the badge, restored or live.
    UpdateChecker::setCheckOnStartup(false);
    check(UpdateChecker::knownBadgeVersion().isEmpty(),
          "no restored badge with the automatic check off");
    check(UpdateChecker::badgeVersion(QStringLiteral("26.10.3"), QStringLiteral("26.10.2")).isEmpty(),
          "no live badge with the automatic check off");
    UpdateChecker::setCheckOnStartup(true);
    check(UpdateChecker::knownBadgeVersion() == QLatin1String("26.10.3"),
          "turning the check back on restores the known badge");
    UpdateChecker::skipVersion(QStringLiteral("v26.10.3<b>x"));
    check(UpdateSettings::skippedVersion() == QLatin1String("26.10.3"),
          "Skip stores parsed digits, never the raw tag");
    check(UpdateChecker::knownBadgeVersion().isEmpty(), "no restored badge for a skipped release");
    UpdateSettings::setSkippedVersion(QString());
    QCoreApplication::setApplicationVersion(QStringLiteral("26.10.3"));
    check(UpdateChecker::knownBadgeVersion().isEmpty(), "no restored badge after upgrading to it");
    QCoreApplication::setApplicationVersion(QStringLiteral("26.11.1"));
    check(UpdateChecker::knownBadgeVersion().isEmpty(), "no restored badge when running newer");
    check(UpdateChecker::badgeVersion(QString(), QStringLiteral("26.10.2")).isEmpty(),
          "no badge before the first answer");
    UpdateSettings::setLastCheckUtc(now.addSecs(-48 * 3600));

    // Pin the stored shape: one object under "Updates", JSON-typed fields.
    // The accessors alone cannot catch a key typo'd the same way in both
    // getter and setter.
    UpdateSettings::setSkippedVersion(QStringLiteral("26.10.4"));
    UpdateSettings::setCheckOnStartup(false);
    const QJsonObject stored = QJsonDocument::fromJson(
        settings.value("Updates").toString().toUtf8()).object();
    check(stored.value("checkOnStartup").isBool()
              && !stored.value("checkOnStartup").toBool(),
          "checkOnStartup stored as a JSON bool");
    check(stored.value("skippedVersion").toString() == QLatin1String("26.10.4"),
          "skippedVersion stored under the expected key");
    check(QDateTime::fromString(stored.value("lastCheckUtc").toString(), Qt::ISODate)
              == now.addSecs(-48 * 3600),
          "lastCheckUtc stored as an ISO 8601 UTC string");
    check(stored.value("latestVersion").toString() == QLatin1String("26.10.3"),
          "latestVersion stored under the expected key");
    check(!settings.contains("checkOnStartup") && !settings.contains("UpdatesCheckOnStartup"),
          "no flat keys");

    return failures ? 1 : 0;
}
