// Text-view content on the bridge (#5078): dump_tree's capped `value` with the
// valueTruncated signal, the `text` verb's full document + line count, the
// empty-but-present case, and the non-text error path.
#include "TestSettingsProfile.h"
#include "TestEventLoop.h"
#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/QsoRecorder.h"
#include "models/RadioModel.h"
#include "core/AutomationServer.h"

#include <QApplication>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdio>
#include <functional>

using namespace AetherSDR;

namespace {
int gFailures = 0;
void expect(const char* name, bool condition)
{
    std::printf("[%s] %s\n", condition ? " OK " : "FAIL", name);
    if (!condition) ++gFailures;
}
bool waitUntil(const std::function<bool()>& c, int ms = 2000) { return AetherTest::waitFor(c, ms); }
QJsonObject request(QLocalSocket* s, QJsonObject o)
{
    o[QStringLiteral("token")] = QStringLiteral("test-token");
    s->write(QJsonDocument(o).toJson(QJsonDocument::Compact)); s->write("\n"); s->flush();
    if (!waitUntil([s]() { return s->canReadLine(); }))
        return QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("timeout")}};
    return QJsonDocument::fromJson(s->readLine()).object();
}
QJsonObject text(QLocalSocket* s, const QString& target)
{
    return request(s, QJsonObject{{QStringLiteral("cmd"), QStringLiteral("text")}, {QStringLiteral("target"), target}});
}
// Find the node with the given objectName anywhere in a dumpTree reply.
QJsonObject findNode(const QJsonValue& v, const QString& name)
{
    if (v.isObject()) {
        const QJsonObject o = v.toObject();
        if (o.value(QStringLiteral("objectName")).toString() == name) return o;
        const QJsonObject r = findNode(o.value(QStringLiteral("children")), name);
        if (!r.isEmpty()) return r;
    } else if (v.isArray()) {
        for (const QJsonValue& c : v.toArray()) {
            const QJsonObject r = findNode(c, name);
            if (!r.isEmpty()) return r;
        }
    }
    return {};
}
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("aether-automation-text-view-test"));
    if (!settingsProfile.isValid()) {
        std::fprintf(stderr, "FAIL could not create isolated settings profile\n");
        return 1;
    }
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("AetherSDR-test"));
    QCoreApplication::setApplicationName(QStringLiteral("AetherSDR-test"));
    AppSettings::instance().load();

    QWidget window; window.resize(400, 300);
    auto* layout = new QVBoxLayout(&window);
    QPlainTextEdit longPane; longPane.setObjectName(QStringLiteral("longPane"));
    const QString longDoc = QString(3000, QLatin1Char('x')) + QStringLiteral("\ntail\n");
    longPane.setPlainText(longDoc);
    layout->addWidget(&longPane);
    QPlainTextEdit logPane; logPane.setObjectName(QStringLiteral("logPane"));
    logPane.setPlainText(QStringLiteral("DE W1AW\nCQ CQ\n"));   // trailing newline: 2 lines
    layout->addWidget(&logPane);
    QTextEdit emptyPane; emptyPane.setObjectName(QStringLiteral("emptyPane"));
    layout->addWidget(&emptyPane);
    QPushButton button(QStringLiteral("Not a text view")); button.setObjectName(QStringLiteral("plainButton"));
    layout->addWidget(&button);
    window.show(); waitUntil([&window]() { return window.isVisible(); });

    AutomationServer server; server.setAuthToken(QStringLiteral("test-token"));
#if defined(Q_OS_UNIX)
    const QString name = QStringLiteral("/tmp/aether-textview-%1").arg(QCoreApplication::applicationPid());
#else
    const QString name = QStringLiteral("aether-textview-%1").arg(QCoreApplication::applicationPid());
#endif
    expect("server starts", server.start(name));
    QLocalSocket client; client.connectToServer(server.fullServerName());
    expect("client connects", waitUntil([&client]() { return client.state() == QLocalSocket::ConnectedState; }));

    // dump_tree: capped value + machine-readable truncation signal.
    const QJsonObject tree = request(&client, QJsonObject{{QStringLiteral("cmd"), QStringLiteral("dumpTree")}});
    const QJsonObject longNode = findNode(tree.value(QStringLiteral("roots")), QStringLiteral("longPane"));
    const QString longVal = longNode.value(QStringLiteral("value")).toString();
    expect("over-cap view carries a 2048-char prefix plus the marker",
           longVal.startsWith(QString(2048, QLatin1Char('x'))) && longVal.endsWith(QStringLiteral("…<truncated>"))
               && longVal.size() == 2048 + QStringLiteral("…<truncated>").size());
    expect("over-cap view carries valueTruncated: true", longNode.value(QStringLiteral("valueTruncated")).toBool());
    const QJsonObject logNode = findNode(tree.value(QStringLiteral("roots")), QStringLiteral("logPane"));
    expect("under-cap view carries the whole document and no valueTruncated",
           logNode.value(QStringLiteral("value")).toString() == QStringLiteral("DE W1AW\nCQ CQ\n")
               && !logNode.contains(QStringLiteral("valueTruncated")));
    const QJsonObject emptyNode = findNode(tree.value(QStringLiteral("roots")), QStringLiteral("emptyPane"));
    expect("empty-but-present view serializes as \"\" (a real assertion, not absence)",
           emptyNode.contains(QStringLiteral("value")) && emptyNode.value(QStringLiteral("value")).toString().isEmpty());

    // text verb: full document, lines as the pane shows them, error path.
    const QJsonObject full = text(&client, QStringLiteral("longPane"));
    expect("text returns the full document", full.value(QStringLiteral("ok")).toBool()
               && full.value(QStringLiteral("text")).toString() == longDoc
               && full.value(QStringLiteral("length")).toInt() == longDoc.size());
    const QJsonObject log = text(&client, QStringLiteral("logPane"));
    expect("trailing newline ends the last line (2 lines, not 3)", log.value(QStringLiteral("lines")).toInt() == 2);
    logPane.setPlainText(QStringLiteral("a\nb"));
    expect("no trailing newline still counts the last line (2)",
           text(&client, QStringLiteral("logPane")).value(QStringLiteral("lines")).toInt() == 2);
    const QJsonObject empty = text(&client, QStringLiteral("emptyPane"));
    expect("empty view: ok, length 0, lines 0", empty.value(QStringLiteral("ok")).toBool()
               && empty.value(QStringLiteral("length")).toInt() == 0 && empty.value(QStringLiteral("lines")).toInt() == 0);
    const QJsonObject notText = text(&client, QStringLiteral("plainButton"));
    expect("non-text target answers 'not a text view'",
           !notText.value(QStringLiteral("ok")).toBool()
               && notText.value(QStringLiteral("error")).toString().startsWith(QStringLiteral("not a text view")));

    // The getText alias resolves to the same verb.
    const QJsonObject viaAlias = request(&client,
        QJsonObject{{QStringLiteral("cmd"), QStringLiteral("getText")},
                    {QStringLiteral("target"), QStringLiteral("logPane")}});
    expect("getText alias answers like text", viaAlias.value(QStringLiteral("ok")).toBool()
               && viaAlias.value(QStringLiteral("lines")).toInt() == 2);

    // The observe-only rail: `text` is a pure read and must stay serviced in
    // read-only mode — this is the assertion that pins the isReadOnlyRequest
    // kSafe entry (deleting "text" from that list fails here).
    server.setReadOnly(true);
    const QJsonObject readOnlyText = text(&client, QStringLiteral("logPane"));
    expect("text is serviced in observe-only mode (kSafe)",
           readOnlyText.value(QStringLiteral("ok")).toBool()
               && readOnlyText.value(QStringLiteral("lines")).toInt() == 2);
    server.setReadOnly(false);

    std::printf("%s\n", gFailures == 0 ? "ALL PASS" : "FAILURES");
    return gFailures == 0 ? 0 : 1;
}
