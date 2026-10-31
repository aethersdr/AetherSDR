// `get tracedebug <pan> bins` at the bridge (#5806): the refusals, and the
// meta-call name the bridge looks the method up by.

#include "core/AutomationServer.h"

#include <QApplication>
#include <QFile>
#include <QJsonObject>

#include <cstdio>

namespace AetherSDR {
class AutomationServerTestAccess
{
public:
    static QJsonObject request(AutomationServer& server, const QByteArray& line)
    {
        return server.handleLine(line, nullptr);
    }
};
}

namespace {
int failures = 0;

void check(bool ok, const char* description)
{
    std::fprintf(stderr, "%s: %s\n", ok ? "PASS" : "FAIL", description);
    if (!ok) {
        ++failures;
    }
}

QByteArray readSource(const char* relativePath)
{
    QFile f(QString::fromLatin1(AETHER_SOURCE_DIR) + QString::fromLatin1(relativePath));
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
}  // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    AetherSDR::AutomationServer server(nullptr);
    const auto request = [&server](const QByteArray& line) {
        return AetherSDR::AutomationServerTestAccess::request(server, line);
    };

    QJsonObject r = request(R"({"cmd":"get","model":"tracedebug","property":"bins"})");
    check(!r.value("ok").toBool(), "bins without a pan is refused");
    check(r.value("error").toString().contains(QLatin1String("needs one pan")),
          "and the refusal says a pan is needed");

    r = request(R"({"cmd":"get","model":"tracedebug","selector":"0","property":"bins"})");
    check(!r.value("ok").toBool(), "bins for a pan that does not exist is refused");
    check(r.value("error").toString().contains(QLatin1String("no pan matches '0'")),
          "and the refusal names the selector");

    r = request(R"({"cmd":"get","model":"tracedebug"})");
    check(r.value("ok").toBool() && r.value("pans").isArray(),
          "tracedebug without a property still answers with the pans array");

    // The bridge reaches the widget by method NAME; a misspelling would only
    // ever show as "no pan matches". Pin the name on both sides.
    const QByteArray server_src = readSource("/src/core/AutomationServer.cpp");
    const QByteArray widget_h = readSource("/src/gui/SpectrumWidget.h");
    check(!server_src.isEmpty() && !widget_h.isEmpty(), "the test can read both sources");
    check(server_src.contains("\"traceBinsSnapshot\""), "the bridge invokes traceBinsSnapshot");
    check(widget_h.contains("Q_INVOKABLE QVariantMap traceBinsSnapshot() const;"),
          "SpectrumWidget declares traceBinsSnapshot as Q_INVOKABLE");

    if (failures == 0) {
        std::fprintf(stderr, "automation_tracedebug_bins_test: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
