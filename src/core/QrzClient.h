#pragma once

#include "CallsignInfo.h"

#include <QNetworkAccessManager>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QString>

class QNetworkReply;

namespace AetherSDR {

// Minimal QRZ.com XML API client (https://xmldata.qrz.com/xml/current/).
// Logs in once, reuses the session key, and re-logs in + retries when the
// server invalidates it (keys die on IP change). The key is memory-only;
// credentials come from setCredentials(). One lookup is in flight at a time;
// for caching/coalescing use CallsignLookupService.
class QrzClient : public QObject {
    Q_OBJECT

public:
    enum class Error {
        Network,        // transport failure / timeout
        AuthFailed,     // bad username/password
        NotFound,       // callsign not in the QRZ database
        Provider,       // any other server-reported error
    };
    Q_ENUM(Error)

    explicit QrzClient(QObject* parent = nullptr);

    void setCredentials(const QString& username, const QString& password);
    bool hasCredentials() const { return !m_username.isEmpty() && !m_password.isEmpty(); }

    // Queue a lookup for `call` (normalized internally).  Results arrive
    // via lookupSucceeded/lookupFailed with the same normalized call.
    void lookup(const QString& call);

    // Standalone credential check for the setup tab's "Test" button —
    // logs in with the given credentials without touching the client's
    // stored session.  Result arrives via loginTestFinished.
    void testLogin(const QString& username, const QString& password);

    // Parsed subset of a QRZ XML response — session block + callsign block.
    // Public (with parseXml) so the unit tests can pin the parser against
    // real <QRZDatabase>-wrapped response shapes — the root element bug
    // fixed in #4043 silently broke login end-to-end and no test caught it.
    struct ParsedResponse {
        QString sessionKey;
        QString sessionError;
        QString sessionMessage;
        CallsignInfo info;
    };
    static ParsedResponse parseXml(const QByteArray& xml);

signals:
    // `call` is the queried (normalized) form; `info.call` is QRZ's canonical
    // base call, which can differ for portable/prefixed queries — consumers
    // must key their in-flight/cache state on `call`, not info.call (#3990).
    void lookupSucceeded(const QString& call, const AetherSDR::CallsignInfo& info);
    void lookupFailed(const QString& call, QrzClient::Error error, const QString& message);
    void loginTestFinished(bool ok, const QString& message);

private:
    void startLogin();
    void startNextLookup();
    void handleLoginReply(QNetworkReply* reply);
    void handleLookupReply(QNetworkReply* reply, const QString& call, bool retriedAfterRelogin);
    QNetworkReply* getUrl(const QString& query);

    QNetworkAccessManager m_nam;
    QString m_username;
    QString m_password;
    QString m_sessionKey;
    bool    m_loginInFlight{false};
    bool    m_lookupInFlight{false};
    QQueue<QString> m_pending;
    QSet<QString>   m_retriedAfterRelogin;  // one relogin+retry per call, no loops
};

} // namespace AetherSDR
