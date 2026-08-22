#include "core/AudioEngine.h"
#include "core/QsoRecorder.h"
#include "models/RadioModel.h"
#include "core/AutomationServer.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>

#include <cstdio>

namespace AetherSDR {

class AutomationServerTestAccess
{
public:
    static QJsonObject handleLine(AutomationServer& server, const QByteArray& line)
    {
        return server.handleLine(line, nullptr);
    }
};

} // namespace AetherSDR

namespace {

int failures = 0;

void check(bool condition, const char* description)
{
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", description);
    if (!condition) {
        ++failures;
    }
}

QJsonObject request(AetherSDR::AutomationServer& server, const QString& line)
{
    return AetherSDR::AutomationServerTestAccess::handleLine(server, line.toUtf8());
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    AetherSDR::AutomationServer server;

    const QJsonObject list = request(server, QStringLiteral("devices list"));
    check(list.value(QStringLiteral("ok")).toBool()
              && list.value(QStringLiteral("diagnostics")).toArray().contains(
                  QStringLiteral("ulanzi")),
          "devices list advertises the Ulanzi diagnostic");

    const QJsonObject unavailable = request(server, QStringLiteral("devices ulanzi"));
    check(!unavailable.value(QStringLiteral("ok")).toBool()
              && unavailable.value(QStringLiteral("error")).toString().contains(
                  QStringLiteral("unavailable")),
          "devices ulanzi fails clearly without a provider");

    int calls = 0;
    server.setDeviceDiagnosticsHandler([&calls](const QString& diagnostic) {
        ++calls;
        return QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("diagnostic"), diagnostic},
            {QStringLiteral("matchedCount"), 1},
            {QStringLiteral("unexpectedMatchCount"), 0},
            {QStringLiteral("exclusiveClaimActive"), true},
        };
    });

    server.setReadOnly(true);
    const QJsonObject snapshot = request(server, QStringLiteral("devices ulanzi"));
    check(snapshot.value(QStringLiteral("ok")).toBool()
              && snapshot.value(QStringLiteral("diagnostic")).toString()
                     == QLatin1String("ulanzi")
              && snapshot.value(QStringLiteral("matchedCount")).toInt() == 1
              && snapshot.value(QStringLiteral("unexpectedMatchCount")).toInt() == 0
              && snapshot.value(QStringLiteral("exclusiveClaimActive")).toBool()
              && calls == 1,
          "devices ulanzi dispatches its provider in observe-only mode");

    const QJsonObject unknown = request(server, QStringLiteral("devices trackpad"));
    check(!unknown.value(QStringLiteral("ok")).toBool()
              && unknown.value(QStringLiteral("error")).toString().contains(
                  QStringLiteral("list|ulanzi"))
              && calls == 1,
          "unknown device diagnostics are rejected before provider dispatch");

    return failures == 0 ? 0 : 1;
}
