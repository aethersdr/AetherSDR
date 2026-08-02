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
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocalSocket>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QThread>
#include <QVector>
#include <QWidget>

#include <cmath>
#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failed = 0;

struct RecordedMouseEvent {
    QEvent::Type type{QEvent::None};
    QPoint position;
    Qt::MouseButton button{Qt::NoButton};
    Qt::MouseButtons buttons{Qt::NoButton};
    Qt::KeyboardModifiers modifiers{Qt::NoModifier};
};

class RecordingWidget final : public QWidget
{
public:
    using QWidget::QWidget;

    QVector<RecordedMouseEvent> mouseEvents;

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        record(event);
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        record(event);
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        record(event);
        event->accept();
    }

private:
    void record(const QMouseEvent* event)
    {
        mouseEvents.append({event->type(), event->position().toPoint(),
                            event->button(), event->buttons(),
                            event->modifiers()});
    }
};

void report(const char* name, bool ok, const QString& detail = QString())
{
    std::printf("%s %-46s %s\n",
                ok ? "[ OK ]" : "[FAIL]",
                name,
                qPrintable(detail));
    if (!ok) {
        ++g_failed;
    }
}

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
        if (!response.contains('\n')) {
            QThread::msleep(1);
        }
    }

    if (!response.contains('\n')) {
        return QJsonObject{{QStringLiteral("testError"),
                            QStringLiteral("timed out waiting for bridge response")}};
    }

    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(response.trimmed(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return QJsonObject{{QStringLiteral("testError"), error.errorString()}};
    }
    return document.object();
}

bool hasExpectedDrag(const QJsonObject& response)
{
    return response.value(QStringLiteral("ok")).toBool()
           && response.value(QStringLiteral("target")).toString()
                  == QStringLiteral("automationDragTarget")
           && response.value(QStringLiteral("x")).toInt() == 10
           && response.value(QStringLiteral("y")).toInt() == 12
           && response.value(QStringLiteral("dx")).toInt() == 3
           && response.value(QStringLiteral("dy")).toInt() == 4
           && response.value(QStringLiteral("modifiers")).toInt()
                  == static_cast<int>(Qt::MetaModifier | Qt::ShiftModifier);
}

bool hasExpectedMouseEvents(const QVector<RecordedMouseEvent>& events)
{
    const Qt::KeyboardModifiers modifiers =
        Qt::MetaModifier | Qt::ShiftModifier;
    const QVector<RecordedMouseEvent> expected{
        {QEvent::MouseButtonPress, QPoint(10, 12),
         Qt::LeftButton, Qt::LeftButton, modifiers},
        {QEvent::MouseMove, QPoint(11, 13),
         Qt::NoButton, Qt::LeftButton, modifiers},
        {QEvent::MouseMove, QPoint(12, 14),
         Qt::NoButton, Qt::LeftButton, modifiers},
        {QEvent::MouseMove, QPoint(13, 16),
         Qt::NoButton, Qt::LeftButton, modifiers},
        {QEvent::MouseButtonRelease, QPoint(13, 16),
         Qt::LeftButton, Qt::NoButton, modifiers},
    };
    if (events.size() != expected.size()) {
        return false;
    }
    for (qsizetype index = 0; index < expected.size(); ++index) {
        const RecordedMouseEvent& actual = events.at(index);
        const RecordedMouseEvent& wanted = expected.at(index);
        if (actual.type != wanted.type || actual.position != wanted.position
            || actual.button != wanted.button || actual.buttons != wanted.buttons
            || actual.modifiers != wanted.modifiers) {
            return false;
        }
    }
    return true;
}

// Count nodes with a given objectName anywhere in a dumpTree subtree. A window
// parented to another widget is enumerated as a root by topLevelWidgets() AND
// was recursed into by its parent's node, so the duplicate shows up as a count
// of 2 rather than as a malformed tree. (#4674)
int countNodes(const QJsonObject& node, const QString& name)
{
    int n = node.value(QStringLiteral("objectName")).toString() == name ? 1 : 0;
    for (const QJsonValue& kid : node.value(QStringLiteral("children")).toArray()) {
        n += countNodes(kid.toObject(), name);
    }
    return n;
}

int countNodesInTree(const QJsonObject& tree, const QString& name)
{
    int n = 0;
    for (const QJsonValue& root : tree.value(QStringLiteral("roots")).toArray()) {
        n += countNodes(root.toObject(), name);
    }
    return n;
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
    qputenv("CFFIXED_USER_HOME", root);
    qputenv("LOCALAPPDATA", root);
    qputenv("XDG_CONFIG_HOME", root);
    qputenv("TMPDIR", root);
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }

    QApplication app(argc, argv);

    RecordingWidget target;
    target.setObjectName(QStringLiteral("automationDragTarget"));
    target.resize(100, 100);
    target.show();
    QCoreApplication::processEvents();

    AutomationServer server;
    double targetTuneMhz = 0.0;
    server.setTargetTuneHandler([&targetTuneMhz](double mhz) {
        targetTuneMhz = mhz;
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("targetTune"), mhz}};
    });
    int activatedMemory = -1;
    QString activatedMemoryPan;
    server.setMemoryActivateHandler(
        [&activatedMemory, &activatedMemoryPan](int memoryIndex,
                                                const QString& panId) {
            activatedMemory = memoryIndex;
            activatedMemoryPan = panId;
            return QJsonObject{{QStringLiteral("ok"), true},
                               {QStringLiteral("memory"), QStringLiteral("activate")},
                               {QStringLiteral("index"), memoryIndex},
                               {QStringLiteral("panId"), panId}};
        });
    const QString serverName = QStringLiteral("aethersdr-drag-at-test-%1")
                                   .arg(QCoreApplication::applicationPid());
    const bool started = server.start(serverName);
    report("bridge starts", started, server.fullServerName());
    if (!started) {
        return 1;
    }

    QLocalSocket socket;
    socket.connectToServer(serverName);
    const bool connected = socket.waitForConnected(2000);
    report("probe connects", connected, socket.errorString());
    if (!connected) {
        server.stop();
        return 1;
    }
    QCoreApplication::processEvents();

    const QJsonObject bare = request(
        socket, QByteArrayLiteral("dragAt automationDragTarget 10 12 3 4 meta,shift"));
    report("bare dragAt preserves all arguments",
           hasExpectedDrag(bare),
           QString::fromUtf8(QJsonDocument(bare).toJson(QJsonDocument::Compact)));
    report("bare dragAt delivers the mouse sequence",
           hasExpectedMouseEvents(target.mouseEvents),
           QStringLiteral("event count=%1").arg(target.mouseEvents.size()));

    target.mouseEvents.clear();
    const QJsonObject jsonRequest{
        {QStringLiteral("cmd"), QStringLiteral("dragAt")},
        {QStringLiteral("target"), QStringLiteral("automationDragTarget")},
        {QStringLiteral("value"), QStringLiteral("10 12 3 4 meta,shift")},
    };
    const QJsonObject json = request(
        socket, QJsonDocument(jsonRequest).toJson(QJsonDocument::Compact));
    report("JSON dragAt preserves all arguments",
           hasExpectedDrag(json),
           QString::fromUtf8(QJsonDocument(json).toJson(QJsonDocument::Compact)));
    report("JSON dragAt delivers the mouse sequence",
           hasExpectedMouseEvents(target.mouseEvents),
           QStringLiteral("event count=%1").arg(target.mouseEvents.size()));

    const QJsonObject targetTune = request(
        socket, QByteArrayLiteral("targettune 146.520"));
    report("targettune reaches commanded-target handler",
           targetTune.value(QStringLiteral("ok")).toBool()
               && std::abs(targetTuneMhz - 146.520) < 0.000001,
           QString::fromUtf8(
               QJsonDocument(targetTune).toJson(QJsonDocument::Compact)));

    const QJsonObject memoryRequest{
        {QStringLiteral("cmd"), QStringLiteral("memory")},
        {QStringLiteral("action"), QStringLiteral("activate")},
        {QStringLiteral("value"), QStringLiteral("12 0x40000000")},
    };
    const QJsonObject memory = request(
        socket, QJsonDocument(memoryRequest).toJson(QJsonDocument::Compact));
    report("memory activate preserves index and pan",
           memory.value(QStringLiteral("ok")).toBool()
               && activatedMemory == 12
               && activatedMemoryPan == QStringLiteral("0x40000000"),
           QString::fromUtf8(
               QJsonDocument(memory).toJson(QJsonDocument::Compact)));

    // ---- a parented window is serialized once, not twice (#4674) ----
    // PanFloatingWindow is a Qt::Window parented to MainWindow for z-order and
    // lifetime. In Qt 6 topLevelWidgets() is exactly the isWindow() set, so such
    // a widget is enumerated as a root AND was recursed into by its parent's
    // node — every widget inside a floated pan appeared twice (two VfoWidgets
    // for one slice). A bare QWidget reproduces it without a radio.
    QWidget floated(&target, Qt::Window);
    floated.setObjectName(QStringLiteral("automationFloatedProbe"));
    floated.resize(80, 60);
    QWidget inner(&floated);
    inner.setObjectName(QStringLiteral("automationFloatedChildProbe"));
    // floors keys off the property, not the concrete type, so a plain widget
    // stands in for a floated pan's SpectrumWidget here.
    inner.setProperty("noiseFloorDbm", -122.5);
    inner.setProperty("panIndex", 1);
    floated.show();
    QCoreApplication::processEvents();

    const QJsonObject tree = request(socket, QByteArrayLiteral("dumpTree"));
    const int windowNodes =
        countNodesInTree(tree, QStringLiteral("automationFloatedProbe"));
    const int childNodes =
        countNodesInTree(tree, QStringLiteral("automationFloatedChildProbe"));
    report("parented window serialized once",
           windowNodes == 1, QStringLiteral("count=%1").arg(windowNodes));
    report("its child serialized once",
           childNodes == 1, QStringLiteral("count=%1").arg(childNodes));

    // Same double-count, different verb: findChildren() also recurses through a
    // parented window, so one spectrum yielded two floors entries with the same
    // panIndex — and floors is documented for 20 Hz polling, so a driver that
    // averages the array silently double-weighted every floated pan. (#4674)
    const QJsonObject floors = request(socket, QByteArrayLiteral("floors"));
    int floorEntries = 0;
    for (const QJsonValue& entry : floors.value(QStringLiteral("floors")).toArray()) {
        if (entry.toObject().value(QStringLiteral("panIndex")).toInt() == 1) {
            ++floorEntries;
        }
    }
    report("floors reports one entry per spectrum",
           floorEntries == 1, QStringLiteral("count=%1").arg(floorEntries));

    target.mouseEvents.clear();
    server.setAuthToken(QStringLiteral("test-token"));
    const QJsonObject authenticatedArgsRequest{
        {QStringLiteral("cmd"), QStringLiteral("dragAt")},
        {QStringLiteral("args"),
         QStringLiteral("automationDragTarget 10 12 3 4 meta,shift")},
        {QStringLiteral("token"), QStringLiteral("test-token")},
    };
    const QJsonObject authenticatedArgs = request(
        socket,
        QJsonDocument(authenticatedArgsRequest).toJson(QJsonDocument::Compact));
    report("authenticated JSON args use the positional registry parser",
           hasExpectedDrag(authenticatedArgs)
               && hasExpectedMouseEvents(target.mouseEvents),
           QString::fromUtf8(
               QJsonDocument(authenticatedArgs).toJson(QJsonDocument::Compact)));

    socket.disconnectFromServer();
    server.stop();

    std::printf("\n%s\n",
                g_failed == 0
                    ? "All tests passed."
                    : qPrintable(QStringLiteral("%1 test(s) failed.").arg(g_failed)));
    return g_failed == 0 ? 0 : 1;
}
