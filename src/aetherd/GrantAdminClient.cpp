#include "GrantAdminClient.h"

#include "core/control/ControlCredentialVault.h"
#include "core/control/LocalControlServer.h"
#include "core/control/LocalCredentialHandshake.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QTextStream>
#include <QTimer>
#include <QSet>
#include <QUuid>
#include <array>
#include <tuple>

namespace AetherSDR::aetherd {
namespace {
constexpr int kTimeoutMs = 5000;

std::optional<QJsonObject> exchange(QLocalSocket& socket, const QJsonObject& request)
{
    const QByteArray frame = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
    if (socket.write(frame) != frame.size()) {
        return {};
    }
    socket.flush();
    QElapsedTimer timer;
    timer.start();
    QByteArray bytes;
    while (timer.elapsed() < kTimeoutMs) {
        bytes += socket.read(control::ProtocolLimits::kMaxMessageBytes + 1 - bytes.size());
        if (bytes.size() > control::ProtocolLimits::kMaxMessageBytes) {
            return {};
        }
        const qsizetype newline = bytes.indexOf('\n');
        if (newline >= 0) {
            const QJsonDocument document = QJsonDocument::fromJson(bytes.left(newline));
            if (!document.isObject()) {
                return {};
            }
            const QJsonObject reply = document.object();
            if (reply.value(QStringLiteral("id")) != request.value(QStringLiteral("id"))
                || reply.value(QStringLiteral("v")).toInt() != 1
                || reply.contains(QStringLiteral("error")) || !reply.value(QStringLiteral("result")).isObject()) {
                return {};
            }
            return reply.value(QStringLiteral("result")).toObject();
        }
        if (!socket.waitForReadyRead(static_cast<int>(kTimeoutMs - timer.elapsed()))) {
            return {};
        }
    }
    return {};
}

QJsonObject safeStatus(const QJsonObject& value)
{
    QJsonObject safe;
    for (const auto& field : {QStringLiteral("clientId"), QStringLiteral("principalId"),
                             QStringLiteral("radioGeneration"), QStringLiteral("grantId"),
                             QStringLiteral("operationId")}) {
        const QString id = value.value(field).toString();
        if (control::ControlCredentialVault::validAuthorityId(id)) {
            safe.insert(field, id);
        }
    }
    for (const auto& field : {QStringLiteral("grantLive"), QStringLiteral("intentActive"),
                             QStringLiteral("ready"), QStringLiteral("recovering"), QStringLiteral("accepted")}) {
        if (value.value(field).isBool()) {
            safe.insert(field, value.value(field));
        }
    }
    return safe;
}
} // namespace

void addGrantAdminOptions(QCommandLineParser& parser)
{
    parser.addOption({QStringLiteral("tx-admin"),
        QStringLiteral("One live grant-admin action: list, grant, revoke, or stop. Never transmits."), QStringLiteral("action")});
    parser.addOption({QStringLiteral("admin-credential"), QStringLiteral("OS-vault grant-admin credential ID (not its secret)."), QStringLiteral("id")});
    parser.addOption({QStringLiteral("tx-client"), QStringLiteral("Exact connected client ID to grant."), QStringLiteral("id")});
    parser.addOption({QStringLiteral("tx-grant"), QStringLiteral("Exact grant ID to revoke."), QStringLiteral("id")});
    parser.addOption({QStringLiteral("radio-generation"), QStringLiteral("Generation shown by tx-admin list; never inferred for mutations."), QStringLiteral("id")});
    parser.addOption({QStringLiteral("tx-operation-ms"), QStringLiteral("Explicit maximum duration per acquired operation."), QStringLiteral("milliseconds")});
    parser.addOption({QStringLiteral("tx-lifetime-ms"), QStringLiteral("Explicit absolute grant lifetime."), QStringLiteral("milliseconds")});
    parser.addOption({QStringLiteral("tx-keepalive-ms"), QStringLiteral("Explicit client liveness deadline."), QStringLiteral("milliseconds")});
}

int runGrantAdmin(const QCommandLineParser& parser)
{
    const auto fail = [] {
        QTextStream(stderr) << "aetherd: grant-admin action failed; check IDs, policy, vault access and daemon state\n";
        return 1;
    };
    const QString action = parser.value(QStringLiteral("tx-admin"));
    const QString authority = parser.value(QStringLiteral("credential-authority"));
    const QString credential = parser.value(QStringLiteral("admin-credential"));
    if (!control::ControlCredentialVault::validAuthorityId(authority)
        || !control::ControlCredentialVault::validAuthorityId(credential)) {
        return fail();
    }
    QSet<QString> allowed{QStringLiteral("tx-admin"), QStringLiteral("admin-credential"),
        QStringLiteral("credential-authority"), QStringLiteral("socket"), QStringLiteral("s")};
    if (action != QLatin1String("list")) {
        allowed.insert(QStringLiteral("radio-generation"));
    }
    if (action == QLatin1String("grant")) {
        allowed.unite({QStringLiteral("tx-client"), QStringLiteral("tx-operation-ms"),
            QStringLiteral("tx-lifetime-ms"), QStringLiteral("tx-keepalive-ms")});
    } else if (action == QLatin1String("revoke")) {
        allowed.insert(QStringLiteral("tx-grant"));
    } else if (action != QLatin1String("list") && action != QLatin1String("stop")) {
        return fail();
    }
    for (const QString& option : parser.optionNames()) {
        if (!allowed.contains(option) || parser.optionNames().count(option) != 1) {
            return fail();
        }
    }
    if (!parser.positionalArguments().isEmpty()) {
        return fail();
    }
    QJsonObject params;
    if (action != QLatin1String("list")) {
        const QString generation = parser.value(QStringLiteral("radio-generation"));
        if (!control::ControlCredentialVault::validAuthorityId(generation)) {
            return fail();
        }
        params = {{QStringLiteral("radioSession"), QStringLiteral("radio-1")},
                  {QStringLiteral("radioGeneration"), generation}};
    }
    QString method = QStringLiteral("txAdmin.listClients");
    if (action == QLatin1String("grant")) {
        const QString client = parser.value(QStringLiteral("tx-client"));
        if (!control::ControlCredentialVault::validAuthorityId(client)) {
            return fail();
        }
        params.insert(QStringLiteral("clientId"), client);
        params.insert(QStringLiteral("activities"), QJsonArray{QStringLiteral("mox")});
        const std::array<std::tuple<const char*, const char*, qint64>, 3> durations{{
            {"tx-operation-ms", "maximumOperationMs", TxGrantManager::kMaximumOperationMs},
            {"tx-lifetime-ms", "lifetimeMs", TxGrantManager::kMaximumGrantMs},
            {"tx-keepalive-ms", "keepAliveMs", TxGrantManager::kMaximumKeepAliveMs}}};
        for (const auto& [option, field, maximum] : durations) {
            const QString text = parser.value(QString::fromLatin1(option));
            bool ok = false;
            const qint64 value = text.toLongLong(&ok);
            if (!ok || value <= 0 || value > maximum || QString::number(value) != text) {
                return fail();
            }
            params.insert(QString::fromLatin1(field), value);
        }
        method = QStringLiteral("txAdmin.issueGrant");
    } else if (action == QLatin1String("revoke")) {
        const QString grant = parser.value(QStringLiteral("tx-grant"));
        if (!control::ControlCredentialVault::validAuthorityId(grant)) {
            return fail();
        }
        params.insert(QStringLiteral("grantId"), grant);
        method = QStringLiteral("txAdmin.revokeGrant");
    } else if (action == QLatin1String("stop")) {
        method = QStringLiteral("txAdmin.emergencyStop");
    }
    control::ControlCredentialVault vault(authority);
    QByteArray secret;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    bool done = false;
    vault.load([&](control::ControlCredentialVault::Result result) {
        if (result.error == control::ControlCredentialVault::Error::None) {
            for (const auto& record : result.records) {
                if (record.id == credential && record.role == control::ControlCredentials::Role::GrantAdmin) {
                    secret = record.secret;
                }
            }
        }
        done = true;
        loop.quit();
    });
    timeout.start(30000);
    if (!done) {
        loop.exec();
    }
    if (!done || secret.size() != control::ControlCredentials::kSecretBytes) {
        return fail();
    }
    const QString endpoint = control::LocalControlServer::clientEndpoint(parser.value(QStringLiteral("socket")));
    if (endpoint.isEmpty()) {
        return fail();
    }
    QLocalSocket socket;
    socket.setReadBufferSize(control::ProtocolLimits::kMaxMessageBytes + 1);
    socket.connectToServer(endpoint);
    if (!socket.waitForConnected(kTimeoutMs)) {
        return fail();
    }
    const auto hello = control::exchangeCredentialHello(secret,
        [&socket] { return socket.state() == QLocalSocket::ConnectedState
            && control::localServerIsCurrentUser(socket.socketDescriptor()); },
        [&socket](const QJsonObject& request) { return exchange(socket, request); });
    // Shorten this reference's lifetime, not secure erasure: Qt's JSON/socket
    // buffers and the native vault job also hold copies. See the credential
    // memory limits in docs/aetherd-stage4-client-grants.md.
    secret.clear();
    const QString sessionId = hello ? hello->value(QStringLiteral("sessionId")).toString() : QString{};
    if (!hello || QUuid(sessionId).isNull()
        || QUuid(sessionId).toString(QUuid::WithoutBraces) != sessionId) {
        return fail();
    }
    const auto result = exchange(socket, {{QStringLiteral("v"), 1}, {QStringLiteral("id"), QStringLiteral("admin-action")},
        {QStringLiteral("sessionId"), hello->value(QStringLiteral("sessionId"))},
        {QStringLiteral("method"), method}, {QStringLiteral("params"), params}});
    if (!result) {
        return fail();
    }
    QJsonObject safe = safeStatus(*result);
    if (action == QLatin1String("list")) {
        QJsonArray clients;
        for (const QJsonValue& value : result->value(QStringLiteral("clients")).toArray()) {
            clients.append(safeStatus(value.toObject()));
        }
        safe.insert(QStringLiteral("clients"), clients);
    }
    QTextStream(stdout) << QJsonDocument(safe).toJson(QJsonDocument::Compact) << '\n';
    return 0;
}

} // namespace AetherSDR::aetherd
