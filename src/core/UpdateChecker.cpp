#include "UpdateChecker.h"
#include "VersionNumber.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace AetherSDR {

static constexpr char kReleasesApiUrl[] =
    "https://api.github.com/repos/aethersdr/AetherSDR/releases/latest";

UpdateChecker::UpdateChecker(QObject* parent)
    : QObject(parent)
{}

void UpdateChecker::checkNow()
{
    if (m_inFlight) return;
    m_inFlight = true;

    QNetworkRequest req{QUrl{QString(kReleasesApiUrl)}};
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    req.setRawHeader("User-Agent",
        QByteArrayLiteral("AetherSDR/") + QCoreApplication::applicationVersion().toUtf8());
    req.setTransferTimeout(15000);

    auto* reply = m_nam.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        m_inFlight = false;

        if (reply->error() != QNetworkReply::NoError) {
            emit checkFailed();
            return;
        }

        const auto doc = QJsonDocument::fromJson(reply->readAll());
        if (doc.isNull()) { emit checkFailed(); return; }

        const QString tag = doc.object().value("tag_name").toString();
        if (tag.isEmpty()) { emit checkFailed(); return; }

        const QString current = QCoreApplication::applicationVersion();
        const auto latest  = VersionNumber::parse(tag);
        const auto running = VersionNumber::parse(current);

        if (latest.isNull()) { emit checkFailed(); return; }

        if (latest > running)
            emit updateAvailable(tag.startsWith('v') ? tag.mid(1) : tag);
        else
            emit upToDate(current);
    });
}

} // namespace AetherSDR
