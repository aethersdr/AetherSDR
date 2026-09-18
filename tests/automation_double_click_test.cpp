// doubleClick / doubleClickAt verbs (#5068).
//
// The bridge could synthesize every other pointer gesture but never built a
// MouseButtonDblClick, so a control that opens on double-click — the VFO DIG
// offset inline editor, the TX filter cut readouts — could not be driven at
// all. Two clickAt calls do NOT substitute: Qt does not promote a pair of
// synthetic press/release sequences into a double-click, and a widget that
// overrides mouseDoubleClickEvent never hears one. That is the assertion this
// file exists for.

// AutomationServer's inline QPointer setters require these QObject-derived
// types to be complete before its header is parsed.
#include "core/AudioEngine.h"
#include "core/QsoRecorder.h"
#include "models/RadioModel.h"
#include "core/AutomationServer.h"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QThread>
#include <QVector>
#include <QWidget>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const QString& detail = QString())
{
    std::printf("%s %-52s %s\n", ok ? "[ OK ]" : "[FAIL]", name, qPrintable(detail));
    if (!ok)
        ++g_failed;
}

struct Recorded {
    QEvent::Type type{QEvent::None};
    QPoint position;
};

// Counts double-clicks separately, because that is the whole point: a widget
// only ever learns about one through mouseDoubleClickEvent.
class RecordingWidget final : public QWidget
{
public:
    using QWidget::QWidget;

    QVector<Recorded> events;
    int doubleClicks{0};

protected:
    void mousePressEvent(QMouseEvent* e) override       { record(e); e->accept(); }
    void mouseReleaseEvent(QMouseEvent* e) override     { record(e); e->accept(); }
    void mouseDoubleClickEvent(QMouseEvent* e) override { ++doubleClicks; record(e); e->accept(); }

private:
    void record(const QMouseEvent* e)
    {
        events.append({e->type(), e->position().toPoint()});
    }
};

QJsonObject request(QLocalSocket& socket, const QByteArray& line)
{
    socket.write(line + '\n');
    socket.flush();

    QByteArray response;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 2000 && !response.contains('\n')) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        response.append(socket.readAll());
        if (!response.contains('\n'))
            QThread::msleep(1);
    }
    if (!response.contains('\n'))
        return QJsonObject{{QStringLiteral("testError"), QStringLiteral("timeout")}};

    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(response.trimmed(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return QJsonObject{{QStringLiteral("testError"), error.errorString()}};
    return doc.object();
}

// The bridge defers the synthetic events onto the GUI loop, so let them land.
void settle()
{
    for (int i = 0; i < 8; ++i)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
}

bool isDoubleClickSequence(const QVector<Recorded>& e, QPoint at)
{
    // Qt's own order for a double-click: Press, Release, DblClick, Release.
    // The window system sends the DblClick INSTEAD of the second Press.
    const QVector<QEvent::Type> want{QEvent::MouseButtonPress,
                                     QEvent::MouseButtonRelease,
                                     QEvent::MouseButtonDblClick,
                                     QEvent::MouseButtonRelease};
    if (e.size() != want.size())
        return false;
    for (qsizetype i = 0; i < want.size(); ++i) {
        if (e.at(i).type != want.at(i) || e.at(i).position != at)
            return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    QTemporaryDir testRoot;
    if (!testRoot.isValid()) {
        std::printf("[FAIL] create temporary test root\n");
        return 1;
    }
    const QByteArray root = testRoot.path().toUtf8();
    qputenv("HOME", root);
    qputenv("XDG_CONFIG_HOME", root);
    qputenv("TMPDIR", root);
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);

    RecordingWidget target;
    target.setObjectName(QStringLiteral("automationDoubleClickTarget"));
    target.resize(100, 80);
    target.show();
    QCoreApplication::processEvents();

    AutomationServer server;
    const QString serverName = QStringLiteral("aethersdr-dblclick-test-%1")
                                   .arg(QCoreApplication::applicationPid());
    const bool started = server.start(serverName);
    report("bridge starts", started, server.fullServerName());
    if (!started)
        return 1;

    QLocalSocket socket;
    socket.connectToServer(serverName);
    const bool connected = socket.waitForConnected(2000);
    report("probe connects", connected, socket.errorString());
    if (!connected) {
        server.stop();
        return 1;
    }
    QCoreApplication::processEvents();

    // ── doubleClick <target> — centre by default ─────────────────────────
    target.events.clear();
    target.doubleClicks = 0;
    const QJsonObject centre =
        request(socket, QByteArrayLiteral("doubleClick automationDoubleClickTarget"));
    settle();
    report("doubleClick <target> is accepted",
           centre.value(QStringLiteral("ok")).toBool(),
           QString::fromUtf8(QJsonDocument(centre).toJson(QJsonDocument::Compact)));
    report("doubleClick lands on the widget centre",
           isDoubleClickSequence(target.events, target.rect().center()),
           QStringLiteral("events=%1").arg(target.events.size()));

    // THE row this verb exists for. A widget only ever learns about a
    // double-click through mouseDoubleClickEvent, and no amount of clickAt
    // produces one.
    report("mouseDoubleClickEvent actually fires", target.doubleClicks == 1,
           QStringLiteral("count=%1").arg(target.doubleClicks));

    // ── doubleClickAt <target> <x> <y> — explicit point ──────────────────
    target.events.clear();
    target.doubleClicks = 0;
    const QJsonObject at = request(
        socket, QByteArrayLiteral("doubleClickAt automationDoubleClickTarget 10 12"));
    settle();
    report("doubleClickAt <target> <x> <y> is accepted",
           at.value(QStringLiteral("ok")).toBool(),
           QString::fromUtf8(QJsonDocument(at).toJson(QJsonDocument::Compact)));
    report("doubleClickAt lands on the named point",
           isDoubleClickSequence(target.events, QPoint(10, 12)),
           QStringLiteral("events=%1").arg(target.events.size()));
    report("doubleClickAt raises mouseDoubleClickEvent too",
           target.doubleClicks == 1,
           QStringLiteral("count=%1").arg(target.doubleClicks));

    // ── clickAt is unchanged: single click, no DblClick ──────────────────
    target.events.clear();
    target.doubleClicks = 0;
    request(socket, QByteArrayLiteral("clickAt automationDoubleClickTarget 10 12"));
    settle();
    report("clickAt still sends press+release only",
           target.events.size() == 2
               && target.events.at(0).type == QEvent::MouseButtonPress
               && target.events.at(1).type == QEvent::MouseButtonRelease,
           QStringLiteral("events=%1").arg(target.events.size()));
    report("clickAt raises no double-click", target.doubleClicks == 0,
           QStringLiteral("count=%1").arg(target.doubleClicks));

    // ── JSON / MCP request form (#5069 review) ───────────────────────────
    // MCP's `bridge_command` forwards a raw JSON object, so this — not the
    // bare line — is the surface every MCP caller actually reaches. handleLine
    // used to fold numeric x/y only when the request string was literally
    // "clickAt" or "clickat", so doubleClickAt was rejected with "clickAt
    // needs both x and y", its aliases with it, and doubleClick silently
    // clicked the centre while reporting ok. Reverting the fold to the
    // clickAt-only spelling fails every row below except the last two.
    target.events.clear();
    target.doubleClicks = 0;
    const QJsonObject jsonAt = request(
        socket,
        QByteArrayLiteral(R"({"cmd":"doubleClickAt","target":"automationDoubleClickTarget","x":10,"y":12})"));
    settle();
    report("JSON doubleClickAt <target> x/y is accepted",
           jsonAt.value(QStringLiteral("ok")).toBool(),
           QString::fromUtf8(QJsonDocument(jsonAt).toJson(QJsonDocument::Compact)));
    report("JSON doubleClickAt lands on the named point",
           isDoubleClickSequence(target.events, QPoint(10, 12)),
           QStringLiteral("events=%1").arg(target.events.size()));

    // The alias spellings resolve through the same registry entry, so they
    // must normalize identically — the old literal match covered neither.
    target.events.clear();
    target.doubleClicks = 0;
    const QJsonObject jsonAlias = request(
        socket,
        QByteArrayLiteral(R"({"cmd":"dblClickAt","target":"automationDoubleClickTarget","x":20,"y":22})"));
    settle();
    report("JSON alias dblClickAt normalizes x/y too",
           jsonAlias.value(QStringLiteral("ok")).toBool()
               && isDoubleClickSequence(target.events, QPoint(20, 22)),
           QString::fromUtf8(QJsonDocument(jsonAlias).toJson(QJsonDocument::Compact)));

    target.events.clear();
    const QJsonObject jsonLowerAlias = request(
        socket,
        QByteArrayLiteral(R"({"cmd":"doubleclickat","target":"automationDoubleClickTarget","x":30,"y":24})"));
    settle();
    report("JSON alias doubleclickat normalizes x/y too",
           jsonLowerAlias.value(QStringLiteral("ok")).toBool()
               && isDoubleClickSequence(target.events, QPoint(30, 24)),
           QString::fromUtf8(QJsonDocument(jsonLowerAlias).toJson(QJsonDocument::Compact)));

    // doubleClick's failure was the quiet one: ok:true at the wrong point.
    target.events.clear();
    target.doubleClicks = 0;
    const QJsonObject jsonDouble = request(
        socket,
        QByteArrayLiteral(R"({"cmd":"doubleClick","target":"automationDoubleClickTarget","x":40,"y":30})"));
    settle();
    report("JSON doubleClick honours x/y instead of clicking the centre",
           jsonDouble.value(QStringLiteral("ok")).toBool()
               && isDoubleClickSequence(target.events, QPoint(40, 30)),
           QStringLiteral("centre=%1,%2 events=%3")
               .arg(target.rect().center().x())
               .arg(target.rect().center().y())
               .arg(target.events.size()));

    // clickAt's own JSON form must be untouched by the generalization.
    target.events.clear();
    target.doubleClicks = 0;
    const QJsonObject jsonClickAt = request(
        socket,
        QByteArrayLiteral(R"({"cmd":"clickAt","target":"automationDoubleClickTarget","x":11,"y":13})"));
    settle();
    report("JSON clickAt is unchanged (press+release at the point)",
           jsonClickAt.value(QStringLiteral("ok")).toBool()
               && target.events.size() == 2
               && target.events.at(0).position == QPoint(11, 13)
               && target.doubleClicks == 0,
           QStringLiteral("events=%1").arg(target.events.size()));

    // Explicit `value` stays authoritative even when x/y would otherwise be
    // malformed. Generic clients may send both representations while moving
    // between schemas; the documented precedence must be deterministic.
    target.events.clear();
    const QJsonObject jsonValueWins = request(
        socket,
        QByteArrayLiteral(R"({"cmd":"doubleClickAt","target":"automationDoubleClickTarget","value":"50 40","x":"ignored"})"));
    settle();
    report("explicit value wins over malformed JSON x/y",
           jsonValueWins.value(QStringLiteral("ok")).toBool()
               && isDoubleClickSequence(target.events, QPoint(50, 40)),
           QString::fromUtf8(QJsonDocument(jsonValueWins).toJson(QJsonDocument::Compact)));

    // A string-typed coordinate must NOT coerce to 0 and click the corner.
    target.events.clear();
    const QJsonObject jsonStringCoord = request(
        socket,
        QByteArrayLiteral(R"({"cmd":"doubleClickAt","target":"automationDoubleClickTarget","x":"10","y":12})"));
    settle();
    report("a string-typed JSON coordinate is refused, not coerced to 0",
           !jsonStringCoord.value(QStringLiteral("ok")).toBool()
               && target.events.isEmpty(),
           QString::fromUtf8(QJsonDocument(jsonStringCoord).toJson(QJsonDocument::Compact)));

    // doubleClick's coordinates are optional only when neither key is sent.
    // Once a caller supplies either key, malformed or partial input must fail
    // closed instead of collapsing to the intentional centre-click default.
    target.events.clear();
    const QJsonObject jsonDoubleStringCoord = request(
        socket,
        QByteArrayLiteral(R"({"cmd":"doubleClick","target":"automationDoubleClickTarget","x":"10","y":12})"));
    settle();
    report("doubleClick refuses a string-typed JSON coordinate",
           !jsonDoubleStringCoord.value(QStringLiteral("ok")).toBool()
               && target.events.isEmpty(),
           QString::fromUtf8(
               QJsonDocument(jsonDoubleStringCoord).toJson(QJsonDocument::Compact)));

    target.events.clear();
    const QJsonObject jsonDoubleXOnly = request(
        socket,
        QByteArrayLiteral(R"({"cmd":"doubleClick","target":"automationDoubleClickTarget","x":10})"));
    settle();
    report("doubleClick refuses JSON x without y",
           !jsonDoubleXOnly.value(QStringLiteral("ok")).toBool()
               && target.events.isEmpty(),
           QString::fromUtf8(QJsonDocument(jsonDoubleXOnly).toJson(QJsonDocument::Compact)));

    target.events.clear();
    const QJsonObject jsonDoubleYOnly = request(
        socket,
        QByteArrayLiteral(R"({"cmd":"doubleClick","target":"automationDoubleClickTarget","y":12})"));
    settle();
    report("doubleClick refuses JSON y without x",
           !jsonDoubleYOnly.value(QStringLiteral("ok")).toBool()
               && target.events.isEmpty(),
           QString::fromUtf8(QJsonDocument(jsonDoubleYOnly).toJson(QJsonDocument::Compact)));

    // ── refusals ─────────────────────────────────────────────────────────
    const QJsonObject noTarget = request(socket, QByteArrayLiteral("doubleClick"));
    report("doubleClick without a target is refused",
           !noTarget.value(QStringLiteral("ok")).toBool());

    const QJsonObject missing =
        request(socket, QByteArrayLiteral("doubleClick automationNoSuchWidget"));
    report("doubleClick on an unknown widget is refused",
           !missing.value(QStringLiteral("ok")).toBool());

    // Disabled widgets drop input events, so a click there is a silent no-op
    // that must not be reported as ok — the guard is inherited from clickAt.
    target.setEnabled(false);
    target.events.clear();
    const QJsonObject disabled =
        request(socket, QByteArrayLiteral("doubleClick automationDoubleClickTarget"));
    settle();
    report("doubleClick on a disabled widget is refused",
           !disabled.value(QStringLiteral("ok")).toBool()
               && target.events.isEmpty(),
           QString::fromUtf8(QJsonDocument(disabled).toJson(QJsonDocument::Compact)));
    target.setEnabled(true);

    // ── the verb is discoverable ─────────────────────────────────────────
    const QJsonObject verbs = request(socket, QByteArrayLiteral("verbs"));
    const QString verbText =
        QString::fromUtf8(QJsonDocument(verbs).toJson(QJsonDocument::Compact));
    report("doubleClick is listed by the verbs registry",
           verbText.contains(QStringLiteral("doubleClick")));

    socket.disconnectFromServer();
    server.stop();

    std::printf("%s\n", g_failed == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return g_failed == 0 ? 0 : 1;
}
