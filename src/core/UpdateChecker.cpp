#include "UpdateChecker.h"
#include "LogManager.h"
#include "NetworkDiagnostics.h"
#include "UpdateSettings.h"
#include "VersionNumber.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#ifdef Q_OS_WIN
#include <windows.h>
#include <appmodel.h>
#endif

namespace AetherSDR {

static constexpr char kReleasesApiUrl[] =
    "https://api.github.com/repos/aethersdr/AetherSDR/releases/latest";

namespace {
const char* triggerName(UpdateChecker::Trigger trigger)
{
    return trigger == UpdateChecker::Trigger::Manual ? "manual" : "automatic";
}
} // namespace

UpdateChecker::UpdateChecker(QObject* parent)
    : QObject(parent)
{}

QString UpdateChecker::storeName()
{
#ifdef Q_OS_WIN
    // APPMODEL_ERROR_NO_PACKAGE: an installer or portable build. Anything else
    // (ERROR_INSUFFICIENT_BUFFER for a zero-length probe) means an MSIX package.
    UINT32 nameLength = 0;
    if (GetCurrentPackageFullName(&nameLength, nullptr) != APPMODEL_ERROR_NO_PACKAGE) {
        return QStringLiteral("the Microsoft Store");
    }
#elif defined(Q_OS_LINUX)
    // Every Flatpak sandbox has /.flatpak-info at its root.
    if (QFile::exists(QStringLiteral("/.flatpak-info"))) {
        return QStringLiteral("Flathub");
    }
#endif
    return {};
}

QString UpdateChecker::badgeVersion(const QString& latestVersion, const QString& runningVersion)
{
    const VersionNumber latest = VersionNumber::parse(latestVersion);
    if (latest.isNull() || !(latest > VersionNumber::parse(runningVersion))
        || UpdateSettings::isSkipped(latestVersion) || !storeName().isEmpty()) {
        return {};
    }
    return latest.toString();
}

QString UpdateChecker::knownBadgeVersion()
{
    return badgeVersion(UpdateSettings::latestVersion(),
                        QCoreApplication::applicationVersion());
}

bool UpdateChecker::checkAutomaticallyIfDue()
{
    const QString store = storeName();
    if (!store.isEmpty()) {
        qCInfo(lcUpdater).noquote() << "automatic check skipped: updates come from" << store;
        return false;
    }
    if (!UpdateSettings::automaticCheckDue(QDateTime::currentDateTimeUtc())) {
        qCInfo(lcUpdater) << "automatic check not due; checkOnStartup"
                          << UpdateSettings::checkOnStartup() << "last answer"
                          << UpdateSettings::lastCheckUtc().toString(Qt::ISODate);
        return false;
    }
    checkNow(Trigger::Automatic);
    return true;
}

void UpdateChecker::fail(const QString& reason)
{
    qCWarning(lcUpdater) << triggerName(m_trigger) << "check failed:" << reason;
    emit checkFailed(m_trigger);
}

void UpdateChecker::checkNow(Trigger trigger)
{
    if (m_inFlight) {
        if (trigger == Trigger::Manual) {
            m_trigger = Trigger::Manual;
        }
        return;
    }
    m_inFlight = true;
    m_trigger = trigger;
    qCInfo(lcUpdater) << triggerName(trigger) << "check started";

    QNetworkRequest req{QUrl{QString(kReleasesApiUrl)}};
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    req.setRawHeader("User-Agent",
        QByteArrayLiteral("AetherSDR/") + QCoreApplication::applicationVersion().toUtf8());
    req.setTransferTimeout(15000);

    auto* reply = m_nam.get(req);
    NetworkDiagnostics::watch(reply, "update check");
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        m_inFlight = false;

        if (reply->error() != QNetworkReply::NoError) {
            fail(reply->errorString());
            return;
        }

        const auto doc = QJsonDocument::fromJson(reply->readAll());
        if (doc.isNull()) {
            fail(QStringLiteral("response is not JSON"));
            return;
        }

        const QString tag = doc.object().value("tag_name").toString();
        if (tag.isEmpty()) {
            fail(QStringLiteral("response has no tag_name"));
            return;
        }

        const QString current = QCoreApplication::applicationVersion();
        const auto latest  = VersionNumber::parse(tag);
        const auto running = VersionNumber::parse(current);

        if (latest.isNull()) {
            fail(QStringLiteral("tag_name is not a version"));
            return;
        }

        UpdateSettings::recordAnswer(QDateTime::currentDateTimeUtc(), latest.toString());
        // Log the parsed version, never the raw tag: it is network text.
        if (latest > running) {
            qCInfo(lcUpdater).nospace().noquote() << triggerName(m_trigger) << " check: v"
                << latest.toString() << " is newer than running v" << current;
            emit updateAvailable(tag.startsWith('v') ? tag.mid(1) : tag, m_trigger);
        } else {
            qCInfo(lcUpdater).nospace().noquote() << triggerName(m_trigger) << " check: running v"
                << current << " is current (latest v" << latest.toString() << ")";
            emit upToDate(current, m_trigger);
        }
    });
}

} // namespace AetherSDR
