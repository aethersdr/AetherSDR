// Every external effect is overridden, including in ordinary mode and mutants.
// The real network manager, TLS socket and keychain jobs never dispatch work.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/AutomationIsolationPolicy.h"
#include "core/backends/flex/SmartLinkClient.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QVariant>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSignalSpy>
#include <cstdio>
#include <cstring>
#include <functional>

namespace AetherSDR {
struct SmartLinkClientTestAccess {
    static void seed(SmartLinkClient& client)
    {
        client.m_idToken = QStringLiteral("synthetic-id");
        client.m_refreshToken = QStringLiteral("synthetic-refresh");
        client.m_authenticated = true;
    }
    static void connected(SmartLinkClient& client) { client.m_serverConnected = true; }
    static bool inProgress(const SmartLinkClient& client) { return client.m_authRequestInProgress; }
    static void server(SmartLinkClient& client) { client.connectToServer(); }
    static void tlsReady(SmartLinkClient& client) { client.onSslConnected(); }
    static void ping(SmartLinkClient& client) { client.onPingTimer(); }
    static void stored(SmartLinkClient& client) { client.acceptStoredToken(QStringLiteral("late-token")); }
    static void loginReply(SmartLinkClient& client, QNetworkReply* reply) { client.finishLogin(reply); }
    static void refreshReply(SmartLinkClient& client, QNetworkReply* reply) { client.finishRefresh(reply); }
};
}
using namespace AetherSDR;
namespace {
int checks = 0;
int failures = 0;
void check(bool passed, const char* description)
{
    ++checks;
    failures += !passed;
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", description);
}
class MemoryReply final : public QNetworkReply {
public:
    explicit MemoryReply(QObject* parent) : QNetworkReply(parent)
    {
        open(QIODevice::ReadOnly);
    }
    int reads = 0;
    QByteArray response;
    void abort() override {}
    void finish(const QByteArray& data)
    {
        response = data;
        setFinished(true);
        emit finished();
    }
    void fail()
    {
        setError(QNetworkReply::AuthenticationRequiredError, QStringLiteral("synthetic auth failure"));
        finish(R"({"error_description":"synthetic refusal"})");
    }
    qint64 bytesAvailable() const override { return response.size() + QNetworkReply::bytesAvailable(); }
protected:
    qint64 readData(char* data, qint64 maximum) override
    {
        ++reads;
        const qint64 size = qMin(maximum, qint64(response.size()));
        if (!size) { return -1; }
        std::memcpy(data, response.constData(), size_t(size));
        response.remove(0, size);
        return size;
    }
};
class IsolatedClient final : public SmartLinkClient {
public:
    int posts = 0;
    int reads = 0;
    int saves = 0;
    int deletes = 0;
    int tls = 0;
    QByteArray lastPost;
    QString saved;
    QList<QByteArray> writes;
    std::function<void(const QString&)> readCompleted;
    MemoryReply* reply = nullptr;
protected:
    QNetworkReply* postAuthRequest(const QNetworkRequest&, const QByteArray& body) override
    {
        ++posts;
        lastPost = body;
        reply = new MemoryReply(this);
        return reply;
    }
    void readStoredToken(const std::function<void(const QString&)>& completed) override
    {
        ++reads;
        readCompleted = completed;
    }
    void writeStoredToken(const QString& token) override { ++saves; saved = token; }
    void deleteStoredToken() override { ++deletes; }
    void startServerConnection() override { ++tls; }
    void writeServerData(const QByteArray& data) override { writes.append(data); }
};
const QByteArray success = R"({"id_token":"new-id","refresh_token":"rotated-refresh"})";
}
int main(int argc, char** argv)
{
    const bool restricted = argc > 1 && QByteArray(argv[1]) == "restricted";
    qunsetenv("AETHER_AUTOMATION_DEMO_ONLY");
    if (restricted) { qputenv("AETHER_AUTOMATION_DEMO_ONLY", "0"); }
    TestSettingsProfile profile(QStringLiteral("smartlink-demo-isolation"));
    if (!profile.isValid()) { return 1; }
    QCoreApplication app(argc, argv);
    AppSettings& settings = AppSettings::instance();
    settings.load();
    settings.setValue("SmartLinkEmail", QStringLiteral("preserve-synthetic-email"));
    settings.save();
    IsolatedClient client;
    QSignalSpy errors(&client, &SmartLinkClient::authFailed);
    QSignalSpy authenticated(&client, &SmartLinkClient::authenticated);
    QSignalSpy connected(&client, &SmartLinkClient::serverConnected);
    if (restricted) {
        client.tryAutoLogin();
        check(client.reads == 0 && errors.size() == 1, "startup refuses before credential read");
        client.login(QStringLiteral("synthetic@example.invalid"), QStringLiteral("synthetic-password"));
        check(client.posts == 0 && settings.value("SmartLinkEmail") == "preserve-synthetic-email",
              "manual login refuses before HTTP or email persistence");
        client.loginWithRefreshToken(QStringLiteral("synthetic-refresh"));
        check(client.posts == 0 && !SmartLinkClientTestAccess::inProgress(client),
              "refresh refuses before request or in-flight state");
        SmartLinkClientTestAccess::seed(client);
        client.saveCredentials();
        check(client.saves == 0, "save refuses before credential write");
        client.clearCredentials();
        check(client.deletes == 0 && settings.value("SmartLinkEmail") == "preserve-synthetic-email",
              "clear refuses before credential deletion or email removal");
        SmartLinkClientTestAccess::server(client);
        check(client.tls == 0, "server entry refuses TLS even with a token");
        SmartLinkClientTestAccess::connected(client);
        const int previousErrors = errors.size();
        client.reconnect();
        check(errors.size() == previousErrors + 1 && client.posts == 0 && client.tls == 0,
              "reconnect refuses before socket or token state handling");
        client.logout();
        check(!client.isAuthenticated() && client.idToken().isEmpty() && client.refreshToken().isEmpty()
                  && !client.isConnected(), "logout clears only local session state");
        check(client.deletes == 0 && settings.value("SmartLinkEmail") == "preserve-synthetic-email",
              "logout preserves stored credentials and email");
        SmartLinkClientTestAccess::stored(client);
        check(client.posts == 0 && client.refreshToken().isEmpty(), "late stored-token callback cannot start authentication");
        auto* loginReply = new MemoryReply(&client);
        loginReply->response = success;
        SmartLinkClientTestAccess::loginReply(client, loginReply);
        auto* refreshReply = new MemoryReply(&client);
        refreshReply->response = success;
        SmartLinkClientTestAccess::refreshReply(client, refreshReply);
        check(loginReply->reads == 0 && refreshReply->reads == 0 && client.idToken().isEmpty()
                  && client.refreshToken().isEmpty() && authenticated.isEmpty() && client.saves == 0 && client.tls == 0,
              "late HTTP callbacks refuse before reading tokens, accepting auth, saving or TLS");
        SmartLinkClientTestAccess::tlsReady(client);
        check(!client.isConnected() && connected.isEmpty() && client.writes.isEmpty(),
              "late TLS callback cannot register a token or signal connection");
        SmartLinkClientTestAccess::connected(client);
        SmartLinkClientTestAccess::ping(client);
        client.requestConnect(QStringLiteral("synthetic-radio"));
        client.disconnectRadioClients(QStringLiteral("synthetic-radio"), {1});
        check(client.writes.isEmpty(), "restricted keepalive and radio requests never write");
        bool exactErrors = !errors.isEmpty();
        for (const QList<QVariant>& error : errors) {
            exactErrors = exactErrors && error.at(0).toString() == AutomationIsolationPolicy::refusal();
        }
        check(exactErrors, "all refusals use the visible demo-only reason");
    } else {
        client.tryAutoLogin();
        check(client.reads == 1 && bool(client.readCompleted), "ordinary startup requests stored token through injected store");
        client.readCompleted(QStringLiteral("synthetic-stored-token"));
        check(client.posts == 1 && QJsonDocument::fromJson(client.lastPost).object().value("refresh_token") == "synthetic-stored-token",
              "ordinary stored token drives refresh POST");
        client.reply->finish(success);
        check(client.isAuthenticated() && client.idToken() == "new-id" && client.refreshToken() == "rotated-refresh"
                  && authenticated.size() == 1 && client.saves == 1 && client.saved == "rotated-refresh" && client.tls == 1,
              "ordinary refresh accepts auth, persists rotated token and starts TLS");
        check(!SmartLinkClientTestAccess::inProgress(client), "ordinary refresh completion releases in-flight latch");
        client.login(QStringLiteral("synthetic@example.invalid"), QStringLiteral("synthetic-password"));
        check(client.posts == 2 && settings.value("SmartLinkEmail") == QString::fromUtf8(QByteArray("synthetic@example.invalid").toBase64()),
              "ordinary manual login preserves email persistence and POST");
        client.reply->finish(success);
        check(authenticated.size() == 2 && client.saves == 2 && client.tls == 2, "ordinary login completion saves and connects");
        SmartLinkClientTestAccess::tlsReady(client);
        SmartLinkClientTestAccess::ping(client);
        check(client.isConnected() && connected.size() == 1 && client.writes.size() == 2
                  && client.writes.first().contains("token=new-id") && client.writes.last() == "ping from client\n",
              "ordinary TLS callback registers and keepalive writes through injected transport");
        client.disconnect();
        client.reconnect();
        check(client.posts == 3, "ordinary reconnect uses refresh authentication");
        client.reply->finish(success);
        client.saveCredentials();
        check(client.saves == 4, "ordinary explicit credential save is retained");
        client.logout();
        check(client.deletes == 1 && settings.value("SmartLinkEmail").toString().isEmpty()
                  && !client.isAuthenticated() && client.idToken().isEmpty() && client.refreshToken().isEmpty(),
              "ordinary logout clears stored credentials, email and local tokens");
        check(errors.isEmpty(), "ordinary successful lifecycle emits no auth failures");
        IsolatedClient rejected;
        QSignalSpy rejectedErrors(&rejected, &SmartLinkClient::authFailed);
        rejected.login(QStringLiteral("synthetic@example.invalid"), QStringLiteral("synthetic-password"));
        rejected.reply->fail();
        check(rejectedErrors.size() == 1 && rejectedErrors.last().at(0).toString() == "synthetic refusal"
                  && !rejected.isAuthenticated() && rejected.saves == 0 && rejected.tls == 0,
              "ordinary login error preserves error handling without save or TLS");
        rejected.loginWithRefreshToken(QStringLiteral("synthetic-refresh"));
        rejected.reply->fail();
        check(rejectedErrors.size() == 2 && !SmartLinkClientTestAccess::inProgress(rejected)
                  && !rejected.isAuthenticated() && rejected.saves == 0 && rejected.tls == 0,
              "ordinary refresh error releases latch without save or TLS");
        rejected.login(QStringLiteral("synthetic@example.invalid"), QStringLiteral("synthetic-password"));
        rejected.reply->finish("{}");
        check(rejectedErrors.size() == 3 && rejectedErrors.last().at(0).toString() == "No id_token in response"
                  && !rejected.isAuthenticated() && rejected.saves == 0 && rejected.tls == 0,
              "ordinary login response missing token remains rejected");
        rejected.loginWithRefreshToken(QStringLiteral("synthetic-refresh"));
        rejected.reply->finish("{}");
        check(rejectedErrors.size() == 4 && rejectedErrors.last().at(0).toString() == "No id_token in refresh response"
                  && !SmartLinkClientTestAccess::inProgress(rejected) && !rejected.isAuthenticated()
                  && rejected.saves == 0 && rejected.tls == 0,
              "ordinary refresh response missing token remains rejected");
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
