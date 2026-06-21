#include "KiwiPublicDirectory.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QRegularExpressionMatchIterator>
#include <QUrl>

namespace AetherSDR {

namespace {
// Per-request transfer timeout so a stalled server can't leave the picker stuck
// on "Loading…" forever (the only escape would otherwise be Cancel).
constexpr int kDirectoryFetchTimeoutMs = 15000;
} // namespace

QByteArray KiwiPublicDirectory::userAgent()
{
    // Honest identity — we are AetherSDR, not a browser.  If an operator
    // chooses to block this, that is their answer and we honor it.  The version
    // comes from the build (AETHERSDR_VERSION) so the identity can't go stale.
#ifdef AETHERSDR_VERSION
    return QByteArrayLiteral("AetherSDR/" AETHERSDR_VERSION
                             " (+https://github.com/aethersdr/AetherSDR)");
#else
    return QByteArrayLiteral("AetherSDR (+https://github.com/aethersdr/AetherSDR)");
#endif
}

QString KiwiPublicReceiver::apiBadge() const
{
    switch (apiPolicy()) {
        case ApiPolicy::Disabled: return QStringLiteral("Web only (API disabled by operator)");
        case ApiPolicy::Limited:  return QStringLiteral("API: %1 of %2 channels").arg(extApi).arg(usersMax);
        case ApiPolicy::Open:     return QStringLiteral("API: %1 channels").arg(extApi);
        case ApiPolicy::Unknown:  break;
    }
    return QStringLiteral("API policy unknown");
}

bool KiwiPublicReceiver::advertisesConnectionLimit() const
{
    // Match "Limits" as a whole whitespace-delimited token, not a substring, so
    // a free-form hardware/firmware descriptor that merely contains those letters
    // (e.g. "Unlimited", "NoLimitsBeta") can't false-positive. The public
    // directory appends the marker as its own token in sdr_hw.
    const QStringList tokens =
        sdrHw.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    for (const QString& token : tokens) {
        if (token.compare(QStringLiteral("Limits"), Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

QString KiwiPublicReceiver::connectionLimitBadge() const
{
    return advertisesConnectionLimit()
        ? QStringLiteral("Limits")
        : QString();
}

QVector<KiwiPublicReceiver> KiwiPublicDirectory::parse(const QByteArray& directoryHtml)
{
    QVector<KiwiPublicReceiver> out;
    const QString html = QString::fromUtf8(directoryHtml);

    // Each receiver entry is an <a href='http://host:port' target='_blank'>
    // anchor followed by a block of <!-- key=value --> metadata comments,
    // running up to the next anchor.
    static const QRegularExpression anchorRe(
        QStringLiteral("<a href='(https?://[^']+)'[^>]*target='_blank'[^>]*>"));
    static const QRegularExpression fieldRe(
        QStringLiteral("<!--\\s*([A-Za-z_]+)=(.*?)\\s*-->"));

    QRegularExpressionMatchIterator anchors = anchorRe.globalMatch(html);
    while (anchors.hasNext()) {
        const QRegularExpressionMatch a = anchors.next();
        const int blockStart = a.capturedEnd();
        // Block ends at the next anchor (peek without consuming the iterator).
        const QRegularExpressionMatch nextA = anchorRe.match(html, blockStart);
        const int blockEnd = nextA.hasMatch() ? nextA.capturedStart() : html.size();
        const QString block = html.mid(blockStart, blockEnd - blockStart);

        KiwiPublicReceiver rx;
        rx.url = a.captured(1).trimmed();
        bool haveName = false, haveMax = false;

        QRegularExpressionMatchIterator fields = fieldRe.globalMatch(block);
        while (fields.hasNext()) {
            const QRegularExpressionMatch f = fields.next();
            const QString key = f.captured(1);
            const QString val = f.captured(2).trimmed();
            if      (key == QLatin1String("name"))      { rx.name = val; haveName = true; }
            else if (key == QLatin1String("loc"))       rx.location = val;
            else if (key == QLatin1String("antenna"))   rx.antenna = val;
            else if (key == QLatin1String("sdr_hw"))    rx.sdrHw = val;
            else if (key == QLatin1String("grid"))      rx.grid = val;
            else if (key == QLatin1String("gps"))       rx.gps = val;
            else if (key == QLatin1String("bands"))     rx.bands = val;
            else if (key == QLatin1String("snr"))       rx.snr = val;
            else if (key == QLatin1String("users"))     rx.users = val.toInt();
            else if (key == QLatin1String("users_max")) { rx.usersMax = val.toInt(); haveMax = true; }
            else if (key == QLatin1String("ext_api"))   rx.extApi = val.toInt();
            else if (key == QLatin1String("offline"))   rx.offline = (val == QLatin1String("yes"));
        }

        // Skip non-receiver anchors (nav links etc.) — a real entry always
        // publishes at least a name and a channel count.
        if (haveName && haveMax)
            out.push_back(rx);
    }
    return out;
}

KiwiPublicDirectory::KiwiPublicDirectory(QObject* parent)
    : QObject(parent)
    , m_net(new QNetworkAccessManager(this))
{
}

void KiwiPublicDirectory::fetch()
{
    // Step 1 — load the gate page and read its one-time interactive token,
    // exactly as a browser does before the user clicks "show receivers".
    QNetworkRequest gateReq{QUrl(QString::fromLatin1(kDirectoryUrl))};
    gateReq.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
    gateReq.setTransferTimeout(kDirectoryFetchTimeoutMs);
    QNetworkReply* gate = m_net->get(gateReq);

    connect(gate, &QNetworkReply::finished, this, [this, gate]() {
        gate->deleteLater();
        if (gate->error() != QNetworkReply::NoError) {
            emit failed(QStringLiteral("gate: ") + gate->errorString());
            return;
        }
        const QByteArray body = gate->readAll();

        // If this IP is already past the (IP-based) interactive gate, the very
        // first GET is the directory itself — parse it directly, no token dance.
        QVector<KiwiPublicReceiver> direct = parse(body);
        if (!direct.isEmpty()) {
            emit ready(direct);
            return;
        }

        const QString page = QString::fromUtf8(body);
        static const QRegularExpression tokenRe(
            QStringLiteral("x-kiwi-auth'\\s*,\\s*'([0-9a-fA-F]+)'"));
        const QRegularExpressionMatch tm = tokenRe.match(page);
        if (!tm.hasMatch()) {
            emit failed(QStringLiteral("gate token not found"));
            return;
        }
        const QByteArray token = tm.captured(1).toLatin1();

        // Step 2 — replay the documented unlock request (token + honest UA),
        // the network equivalent of the user clicking the button.
        QNetworkRequest unlockReq{QUrl(QString::fromLatin1(kDirectoryUrl))};
        unlockReq.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
        unlockReq.setTransferTimeout(kDirectoryFetchTimeoutMs);
        unlockReq.setRawHeader("x-kiwi-auth", token);
        QNetworkReply* unlock = m_net->get(unlockReq);

        connect(unlock, &QNetworkReply::finished, this, [this, unlock]() {
            unlock->deleteLater();
            if (unlock->error() != QNetworkReply::NoError) {
                emit failed(QStringLiteral("unlock: ") + unlock->errorString());
                return;
            }
            // Step 3 — fetch the now-unlocked directory listing.
            QNetworkRequest listReq{QUrl(QString::fromLatin1(kDirectoryUrl))};
            listReq.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
            listReq.setTransferTimeout(kDirectoryFetchTimeoutMs);
            QNetworkReply* list = m_net->get(listReq);

            connect(list, &QNetworkReply::finished, this, [this, list]() {
                list->deleteLater();
                if (list->error() != QNetworkReply::NoError) {
                    emit failed(QStringLiteral("list: ") + list->errorString());
                    return;
                }
                emit ready(parse(list->readAll()));
            });
        });
    });
}

} // namespace AetherSDR
