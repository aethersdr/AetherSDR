#include "RemoteEnginePairing.h"
#include "RemoteEngineStore.h"

#include "core/control/ControlPairing.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslError>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>

namespace AetherSDR::aetherd {

namespace {
constexpr int kPairingTimeoutMs = 15000;

QString fingerprintOf(const QWebSocket& socket)
{
    return QString::fromLatin1(socket.sslConfiguration().peerCertificate()
                                   .digest(QCryptographicHash::Sha256).toHex());
}

bool pinnableError(QSslError::SslError error)
{
    switch (error) {
    case QSslError::SelfSignedCertificate:
    case QSslError::SelfSignedCertificateInChain:
    case QSslError::HostNameMismatch:
    case QSslError::CertificateUntrusted:
    case QSslError::UnableToGetLocalIssuerCertificate:
    case QSslError::UnableToVerifyFirstCertificate:
        return true;
    default:
        return false;
    }
}
} // namespace

RemoteEnginePairing::RemoteEnginePairing(QObject* parent) : QObject(parent) {}

RemoteEnginePairing::~RemoteEnginePairing()
{
    ++m_generation;
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
    }
}

bool RemoteEnginePairing::codeMatchesCertificate(const QString& code, const QString& certificateSha256)
{
    const std::optional<QString> digits = control::ControlPairing::digitsOf(code);
    if (!digits || digits->size() != 10 || certificateSha256.size() != 64) { return false; }
    return control::ControlPairing::checkDigits(digits->left(6), certificateSha256) == digits->right(4);
}

void RemoteEnginePairing::start(const QString& lanUrl, const QString& code, const QString& clientName,
                                const QString& engineName)
{
    cancel();
    const std::optional<QString> digits = control::ControlPairing::digitsOf(code);
    if (!RemoteEngineStore::validLanUrl(lanUrl)) {
        emit failed(tr("Pair over the engine's LAN address, wss://<IP address>:<port>."));
        return;
    }
    if (!digits || digits->size() != 10) {
        emit failed(tr("The pairing code is ten digits, as shown on the radio's screen."));
        return;
    }
    const QByteArray name = clientName.trimmed().toUtf8().left(32);
    m_lanUrl = lanUrl;
    m_digits = *digits;
    m_clientName = QString::fromUtf8(name).trimmed();
    if (m_clientName.isEmpty()) { m_clientName = QStringLiteral("AetherSDR"); }
    m_engineName = engineName;
    m_fingerprint.clear();
    m_sent = false;
    const quint64 generation = ++m_generation;
    m_socket = std::make_unique<QWebSocket>(QStringLiteral("aethersdr"));
    QWebSocket* socket = m_socket.get();
    connect(socket, &QWebSocket::sslErrors, this, [this, generation, socket](const QList<QSslError>& errors) {
        if (generation != m_generation) { return; }
        m_fingerprint = fingerprintOf(*socket);
        if (!codeMatchesCertificate(m_digits, m_fingerprint)) {
            finish(tr("The code does not match the engine this address reached, so nothing was sent. "
                      "Check the address and the code on the radio's screen."));
            return;
        }
        for (const QSslError& error : errors) {
            if (!pinnableError(error.error())) {
                finish(tr("The engine's certificate is unusable: %1").arg(error.errorString()));
                return;
            }
        }
        socket->ignoreSslErrors(errors);
    });
    connect(socket, &QWebSocket::connected, this, [this, generation, socket] {
        if (generation != m_generation) { return; }
        m_fingerprint = fingerprintOf(*socket);
        if (!codeMatchesCertificate(m_digits, m_fingerprint)) {
            finish(tr("The code does not match the engine this address reached, so nothing was sent."));
            return;
        }
        m_sent = true;
        const QJsonObject request{{QStringLiteral("v"), 1}, {QStringLiteral("id"), QStringLiteral("pair")},
            {QStringLiteral("method"), QStringLiteral("pairing.complete")},
            {QStringLiteral("params"), QJsonObject{{QStringLiteral("code"), m_digits},
                                                   {QStringLiteral("clientName"), m_clientName}}}};
        socket->sendTextMessage(QString::fromUtf8(QJsonDocument(request).toJson(QJsonDocument::Compact)));
    });
    connect(socket, &QWebSocket::textMessageReceived, this, [this, generation](const QString& text) {
        if (generation != m_generation) { return; }
        const QJsonObject reply = QJsonDocument::fromJson(text.toUtf8()).object();
        if (reply.value(QStringLiteral("id")).toString() != QLatin1String("pair")) { return; }
        const QString code = reply.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toString();
        const QString token = reply.value(QStringLiteral("result")).toObject().value(QStringLiteral("token")).toString();
        if (!code.isEmpty() || token.size() != 64) {
            if (code == QLatin1String("auth.invalid")) {
                finish(tr("The engine refused the code. Check it on the radio's screen and try again."));
            } else if (code == QLatin1String("capability.unavailable")) {
                finish(tr("No pairing is open on the engine. Start one on the radio's screen "
                          "(Settings, Remote clients, Pair a device)."));
            } else {
                finish(tr("Pairing failed (%1).").arg(code.isEmpty() ? tr("no token") : code));
            }
            return;
        }
        RemoteEngineRecord record;
        record.engineId = m_fingerprint;
        record.name = m_engineName;
        record.lanUrl = m_lanUrl;
        record.token = token;
        ++m_generation;
        m_socket->disconnect(this);
        m_socket->abort();
        m_socket.release()->deleteLater();
        emit paired(record);
    });
    connect(socket, &QWebSocket::disconnected, this, [this, generation, socket] {
        if (generation != m_generation) { return; }
        finish(m_sent ? tr("The engine closed the connection without pairing.")
                      : tr("Could not reach the engine: %1").arg(socket->errorString()));
    });
    QTimer::singleShot(kPairingTimeoutMs, this, [this, generation] {
        if (generation == m_generation) { finish(tr("The engine did not answer.")); }
    });
    socket->open(QUrl(lanUrl));
}

void RemoteEnginePairing::cancel()
{
    if (!m_socket) { return; }
    ++m_generation;
    m_socket->disconnect(this);
    m_socket->abort();
    m_socket.release()->deleteLater();
}

void RemoteEnginePairing::finish(const QString& failure)
{
    cancel();
    emit failed(failure);
}

} // namespace AetherSDR::aetherd
