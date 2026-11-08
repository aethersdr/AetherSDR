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
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <appmodel.h>
#endif

namespace AetherSDR {

static constexpr char kReleasesApiUrl[] =
    "https://api.github.com/repos/aethersdr/AetherSDR/releases/latest";

namespace {
// A real answer is tens of KB (the release body plus its asset list). The
// transfer timeout is an idle timeout, so a trickling peer is stopped here.
constexpr qint64 kMaxResponseBytes = 1024 * 1024;

const char* triggerName(UpdateChecker::Trigger trigger)
{
    return trigger == UpdateChecker::Trigger::Manual ? "manual" : "automatic";
}

QString probeStoreName()
{
#ifdef Q_OS_WIN
    // APPMODEL_ERROR_NO_PACKAGE: an installer or portable build. Anything else
    // (ERROR_INSUFFICIENT_BUFFER for a zero-length probe) means an MSIX package.
    UINT32 nameLength = 0;
    if (GetCurrentPackageFullName(&nameLength, nullptr) != APPMODEL_ERROR_NO_PACKAGE) {
        return QStringLiteral("the Microsoft Store");
    }
#elif defined(Q_OS_LINUX)
    // Every Flatpak sandbox has /.flatpak-info at its root, whichever remote
    // (or local build) it came from.
    if (QFile::exists(QStringLiteral("/.flatpak-info"))) {
        return QStringLiteral("Flatpak");
    }
#endif
    return {};
}
} // namespace

UpdateChecker::UpdateChecker(QObject* parent, QNetworkAccessManager* network)
    : QObject(parent)
    , m_nam(network ? network : new QNetworkAccessManager(this))
{
    const VersionNumber saved = VersionNumber::parse(UpdateSettings::latestVersion());
    if (!saved.isNull()) {
        m_latest.version = saved.toString();
        m_latest.fromSettings = true;
    }
}

QString UpdateChecker::storeName()
{
    // How this copy was installed cannot change while it runs.
    static const QString name = probeStoreName();
    return name;
}

bool UpdateChecker::checkOnStartup()
{
    return UpdateSettings::checkOnStartup();
}

void UpdateChecker::setCheckOnStartup(bool enabled)
{
    UpdateSettings::setCheckOnStartup(enabled);
    qCInfo(lcUpdater) << "automatic check turned" << (enabled ? "on" : "off");
}

void UpdateChecker::skipVersion(const QString& version)
{
    const VersionNumber parsed = VersionNumber::parse(version);
    if (parsed.isNull()) {
        return;
    }
    UpdateSettings::setSkippedVersion(parsed.toString());
    qCInfo(lcUpdater).noquote() << "operator skipped v" + parsed.toString();
}

bool UpdateChecker::isSkipped(const QString& version)
{
    return UpdateSettings::isSkipped(version);
}

void UpdateChecker::unskipVersion(const QString& version)
{
    if (!isSkipped(version)) {
        return;
    }
    UpdateSettings::setSkippedVersion(QString());
    qCInfo(lcUpdater).noquote() << "operator installed skipped v" + version + "; no longer skipped";
}

bool UpdateChecker::isNewer(const QString& a, const QString& b)
{
    const VersionNumber va = VersionNumber::parse(a);
    return !va.isNull() && va > VersionNumber::parse(b);
}

QString UpdateChecker::badgeVersion(const QString& latestVersion, const QString& runningVersion)
{
    const VersionNumber latest = VersionNumber::parse(latestVersion);
    if (!isNewer(latestVersion, runningVersion) || !UpdateSettings::checkOnStartup()
        || UpdateSettings::isSkipped(latestVersion) || !storeName().isEmpty()) {
        return {};
    }
    return latest.toString();
}

QString UpdateChecker::knownBadgeVersion()
{
    const QString known = badgeVersion(UpdateSettings::latestVersion(),
                                       QCoreApplication::applicationVersion());
    if (!known.isEmpty()) {
        qCInfo(lcUpdater).noquote() << "known update v" + known + " restored without a request";
    }
    return known;
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
                          << UpdateSettings::checkOnStartup() << "last attempt"
                          << UpdateSettings::lastCheckUtc().toString(Qt::ISODate);
        return false;
    }
    return checkNow(Trigger::Automatic);
}

void UpdateChecker::fail(const QString& reason)
{
    qCWarning(lcUpdater) << triggerName(m_trigger) << "check failed:" << reason;
    // A failed automatic check still starts the daily wait, so a blocking
    // proxy or a rate-limited reply costs one request a day, not one a launch.
    // The known release is left as it was.
    if (m_startedAutomatically) {
        UpdateSettings::setLastCheckUtc(QDateTime::currentDateTimeUtc());
    }
    emit checkFailed(m_trigger);
}

bool UpdateChecker::checkNow(Trigger trigger)
{
    if (m_inFlight) {
        if (trigger == Trigger::Manual) {
            m_trigger = Trigger::Manual;
        }
        return false;
    }
    m_inFlight = true;
    m_tooLarge = false;
    m_startedAutomatically = trigger == Trigger::Automatic;
    m_trigger = trigger;
    qCInfo(lcUpdater) << triggerName(trigger) << "check started";
    emit checkStarted();

    QNetworkRequest req{QUrl{QString(kReleasesApiUrl)}};
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    req.setRawHeader("User-Agent",
        QByteArrayLiteral("AetherSDR/") + QCoreApplication::applicationVersion().toUtf8());
    req.setTransferTimeout(15000);
    // One fixed endpoint, read only for tag_name: a redirect is never a
    // legitimate answer here. Set per request, so a supplied manager is left
    // as its owner configured it; the 3xx is refused when the reply finishes.
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::ManualRedirectPolicy);

    auto* reply = m_nam->get(req);
    NetworkDiagnostics::watch(reply, "update check");
    connect(reply, &QNetworkReply::downloadProgress, this,
            [this, reply](qint64 received, qint64) {
        if (received > kMaxResponseBytes && !m_tooLarge) {
            m_tooLarge = true;
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        m_inFlight = false;

        if (m_tooLarge) {
            fail(QStringLiteral("response larger than %1 bytes").arg(kMaxResponseBytes));
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            fail(reply->errorString());
            return;
        }
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status >= 300 && status < 400) {
            fail(QStringLiteral("endpoint answered with redirect %1").arg(status));
            return;
        }

        const QByteArray body = reply->readAll();
        const auto doc = QJsonDocument::fromJson(body);
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

        // From here on only the parsed digits travel: the tag is network text.
        const QString latestText = latest.toString();
        UpdateSettings::recordAnswer(QDateTime::currentDateTimeUtc(), latestText);
        m_latest = parseRelease(body);
        m_latest.answered = QDateTime::currentDateTimeUtc();
        emit releaseInfoChanged();
        if (latest > running) {
            qCInfo(lcUpdater).nospace().noquote() << triggerName(m_trigger) << " check: v"
                << latestText << " is newer than running v" << current;
            if (UpdateSettings::isSkipped(latestText)) {
                qCInfo(lcUpdater).noquote() << "v" + latestText + " is the release the operator skipped";
            }
            emit updateAvailable(latestText, m_trigger);
        } else {
            qCInfo(lcUpdater).nospace().noquote() << triggerName(m_trigger) << " check: running v"
                << current << " is current (latest v" << latestText << ")";
            emit upToDate(current, m_trigger);
        }
    });
    return true;
}

} // namespace AetherSDR
