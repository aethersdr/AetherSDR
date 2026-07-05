#include "QrzClient.h"

#include "CallsignUtils.h"
#include "LogManager.h"

#include <QCoreApplication>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>
#include <QXmlStreamReader>

namespace AetherSDR {

namespace {
constexpr const char* kApiBase = "https://xmldata.qrz.com/xml/current/";
constexpr int kTimeoutMs = 15000;

bool isSessionInvalidError(const QString& err)
{
    // Server strings per the XML spec: "Session Timeout", "Invalid session key".
    return err.contains(QLatin1String("session"), Qt::CaseInsensitive)
        && (err.contains(QLatin1String("timeout"), Qt::CaseInsensitive)
            || err.contains(QLatin1String("invalid"), Qt::CaseInsensitive));
}
} // namespace

QrzClient::QrzClient(QObject* parent)
    : QObject(parent)
{
}

void QrzClient::setCredentials(const QString& username, const QString& password)
{
    if (username == m_username && password == m_password)
        return;
    m_username = username;
    m_password = password;
    m_sessionKey.clear();  // force fresh login with the new credentials
}

QNetworkReply* QrzClient::getUrl(const QString& query)
{
    QUrl url{QString::fromLatin1(kApiBase)};
    url.setQuery(query);
    QNetworkRequest req{url};
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("AetherSDR/%1").arg(QCoreApplication::applicationVersion()));
    req.setTransferTimeout(kTimeoutMs);
    return m_nam.get(req);
}

void QrzClient::lookup(const QString& call)
{
    const QString norm = Callsigns::normalized(call);
    if (norm.isEmpty())
        return;
    if (!hasCredentials()) {
        emit lookupFailed(norm, Error::AuthFailed,
                          QStringLiteral("QRZ credentials are not configured"));
        return;
    }
    if (!m_pending.contains(norm))
        m_pending.enqueue(norm);
    startNextLookup();
}

void QrzClient::startNextLookup()
{
    if (m_lookupInFlight || m_loginInFlight || m_pending.isEmpty())
        return;
    if (m_sessionKey.isEmpty()) {
        startLogin();
        return;
    }

    const QString call = m_pending.dequeue();
    m_lookupInFlight = true;

    QUrlQuery q;
    q.addQueryItem(QStringLiteral("s"), m_sessionKey);
    q.addQueryItem(QStringLiteral("callsign"), call);
    auto* reply = getUrl(q.toString(QUrl::FullyEncoded));
    const bool retried = m_retriedAfterRelogin.contains(call);
    connect(reply, &QNetworkReply::finished, this, [this, reply, call, retried] {
        handleLookupReply(reply, call, retried);
    });
}

void QrzClient::startLogin()
{
    if (m_loginInFlight)
        return;
    m_loginInFlight = true;

    QUrlQuery q;
    q.addQueryItem(QStringLiteral("username"), m_username);
    q.addQueryItem(QStringLiteral("password"), m_password);
    q.addQueryItem(QStringLiteral("agent"),
                   QStringLiteral("AetherSDR%1").arg(QCoreApplication::applicationVersion()));
    auto* reply = getUrl(q.toString(QUrl::FullyEncoded));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        handleLoginReply(reply);
    });
}

void QrzClient::handleLoginReply(QNetworkReply* reply)
{
    reply->deleteLater();
    m_loginInFlight = false;

    auto failAll = [this](Error error, const QString& message) {
        qCWarning(lcQrz) << "QRZ login failed:" << message;
        m_retriedAfterRelogin.clear();   // all pending are failing — don't leak retry flags (#3990)
        while (!m_pending.isEmpty())
            emit lookupFailed(m_pending.dequeue(), error, message);
    };

    if (reply->error() != QNetworkReply::NoError) {
        failAll(Error::Network, reply->errorString());
        return;
    }

    const ParsedResponse resp = parseXml(reply->readAll());
    if (resp.sessionKey.isEmpty()) {
        const QString msg = resp.sessionError.isEmpty()
            ? QStringLiteral("QRZ login rejected (no session key)")
            : resp.sessionError;
        failAll(Error::AuthFailed, msg);
        return;
    }

    m_sessionKey = resp.sessionKey;
    qCDebug(lcQrz) << "QRZ session established";
    startNextLookup();
}

void QrzClient::handleLookupReply(QNetworkReply* reply, const QString& call,
                                  bool retriedAfterRelogin)
{
    reply->deleteLater();
    m_lookupInFlight = false;

    if (reply->error() != QNetworkReply::NoError) {
        m_retriedAfterRelogin.remove(call);   // don't leak the retry flag (#3990)
        emit lookupFailed(call, Error::Network, reply->errorString());
        startNextLookup();
        return;
    }

    ParsedResponse resp = parseXml(reply->readAll());

    // A response without our session key means the server invalidated it.
    // Re-login once and retry this callsign; a second failure is terminal
    // for this lookup (avoids a login loop on flapping sessions).
    if (resp.sessionKey.isEmpty() || isSessionInvalidError(resp.sessionError)) {
        m_sessionKey.clear();
        if (!retriedAfterRelogin && !resp.info.isValid()) {
            qCDebug(lcQrz) << "QRZ session expired; re-authenticating for" << call;
            m_retriedAfterRelogin.insert(call);
            m_pending.prepend(call);
            startNextLookup();
            return;
        }
    }

    m_retriedAfterRelogin.remove(call);
    if (resp.info.isValid()) {
        resp.info.fetchedUtc = QDateTime::currentSecsSinceEpoch();
        emit lookupSucceeded(call, resp.info);
    } else {
        const QString err = resp.sessionError.isEmpty()
            ? QStringLiteral("No data returned") : resp.sessionError;
        const bool notFound = err.contains(QLatin1String("not found"), Qt::CaseInsensitive);
        emit lookupFailed(call, notFound ? Error::NotFound : Error::Provider, err);
    }
    startNextLookup();
}

void QrzClient::testLogin(const QString& username, const QString& password)
{
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("username"), username);
    q.addQueryItem(QStringLiteral("password"), password);
    q.addQueryItem(QStringLiteral("agent"),
                   QStringLiteral("AetherSDR%1").arg(QCoreApplication::applicationVersion()));
    auto* reply = getUrl(q.toString(QUrl::FullyEncoded));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit loginTestFinished(false, reply->errorString());
            return;
        }
        const ParsedResponse resp = parseXml(reply->readAll());
        if (resp.sessionKey.isEmpty()) {
            emit loginTestFinished(false, resp.sessionError.isEmpty()
                ? QStringLiteral("Login rejected") : resp.sessionError);
        } else {
            // SubExp arrives in sessionMessage when the server sends it —
            // "non-subscriber" logins still get a key but limited fields.
            emit loginTestFinished(true, resp.sessionMessage);
        }
    });
}

QrzClient::ParsedResponse QrzClient::parseXml(const QByteArray& xml)
{
    ParsedResponse resp;
    QXmlStreamReader r(xml);

    enum class Block { None, Session, Callsign };
    Block block = Block::None;
    bool hasLat = false, hasLon = false;   // require BOTH before trusting geo (#3990)

    while (!r.atEnd()) {
        const QXmlStreamReader::TokenType tok = r.readNext();
        if (tok == QXmlStreamReader::StartElement) {
            const auto name = r.name();
            if (name == QLatin1String("Session")) { block = Block::Session; continue; }
            if (name == QLatin1String("Callsign")) { block = Block::Callsign; continue; }
            if (block == Block::None)
                continue;  // outside any recognized block (e.g. the <QRZDatabase> root) —
                           // readElementText() would read to ITS end tag and swallow every
                           // child, including <Session>/<Callsign>, before we ever see them

            const QString text = r.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
            if (block == Block::Session) {
                if (name == QLatin1String("Key"))          resp.sessionKey = text;
                else if (name == QLatin1String("Error"))   resp.sessionError = text;
                else if (name == QLatin1String("Message")) resp.sessionMessage = text;
                else if (name == QLatin1String("SubExp"))  {
                    if (resp.sessionMessage.isEmpty())
                        resp.sessionMessage = QStringLiteral("Subscription: %1").arg(text);
                }
            } else if (block == Block::Callsign) {
                CallsignInfo& i = resp.info;
                if (name == QLatin1String("call"))          i.call = text.toUpper();
                else if (name == QLatin1String("fname"))    i.firstName = text;
                else if (name == QLatin1String("name"))     i.lastName = text;
                else if (name == QLatin1String("name_fmt")) i.nameFmt = text;
                else if (name == QLatin1String("nickname")) i.nickname = text;
                else if (name == QLatin1String("addr2"))    i.city = text;
                else if (name == QLatin1String("state"))    i.state = text;
                else if (name == QLatin1String("country"))  i.country = text;
                else if (name == QLatin1String("county"))   i.county = text;
                else if (name == QLatin1String("grid"))     i.grid = text;
                else if (name == QLatin1String("class"))    i.licenseClass = text;
                else if (name == QLatin1String("email"))    i.email = text;
                else if (name == QLatin1String("url"))      i.url = text;
                else if (name == QLatin1String("image"))    i.imageUrl = text;
                else if (name == QLatin1String("lat")) {
                    bool ok = false;
                    const double v = text.toDouble(&ok);
                    if (ok) { i.latitude = v; hasLat = true; }
                } else if (name == QLatin1String("lon")) {
                    bool ok = false;
                    const double v = text.toDouble(&ok);
                    if (ok) { i.longitude = v; hasLon = true; }
                }
                else if (name == QLatin1String("lotw"))     i.lotw = (text == QLatin1String("1"));
                else if (name == QLatin1String("eqsl"))     i.eqsl = (text == QLatin1String("1"));
                else if (name == QLatin1String("mqsl"))     i.mailQsl = (text == QLatin1String("1"));
            }
        }
    }
    // lat without lon (or vice-versa) is useless — a valid lat with a missing
    // lon would otherwise leave a bogus (lat, 0.0). Require both. (#3990)
    resp.info.hasLatLon = hasLat && hasLon;
    return resp;
}

} // namespace AetherSDR
