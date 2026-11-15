// Socket-free bridge pointer checks: doubleClick/doubleClickAt and clickAt
// event sequences, JSON x/y folding, dragAt argument and event fidelity,
// parented-window de-duplication in dumpTree and floors, and the
// authenticated positional `args` form. Requests go straight to handleLine.
#include "TestSettingsProfile.h"
// AutomationServer's QPointer members require these complete types.
#include "core/AudioEngine.h"
#include "core/QsoRecorder.h"
#include "models/RadioModel.h"
#include "core/AutomationServer.h"

#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QIntValidator>
#include <QLineEdit>
#include <QMouseEvent>
#include <QStringList>
#include <QVector>
#include <QWidget>

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
} // namespace AetherSDR

using namespace AetherSDR;

namespace {

int failures = 0;

void check(bool ok, const QString& description, const QJsonObject& reply = {})
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", qPrintable(description));
    if (!ok) {
        ++failures;
        if (!reply.isEmpty()) {
            std::printf("     reply: %s\n",
                        QJsonDocument(reply).toJson(QJsonDocument::Compact).constData());
        }
    }
}

bool ok(const QJsonObject& reply) { return reply.value(QStringLiteral("ok")).toBool(); }

struct Recorded {
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
    QVector<Recorded> events;
    int doubleClicks{0};

protected:
    void mousePressEvent(QMouseEvent* e) override { record(e); }
    void mouseMoveEvent(QMouseEvent* e) override { record(e); }
    void mouseReleaseEvent(QMouseEvent* e) override { record(e); }
    void mouseDoubleClickEvent(QMouseEvent* e) override { ++doubleClicks; record(e); }

private:
    void record(QMouseEvent* e)
    {
        events.append({e->type(), e->position().toPoint(), e->button(), e->buttons(),
                       e->modifiers()});
        e->accept();
    }
};

// Click verbs deliver on a later main-loop turn.
void settle()
{
    for (int i = 0; i < 8; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
}

bool isDoubleClick(const QVector<Recorded>& events, QPoint at)
{
    const QVector<QEvent::Type> want{QEvent::MouseButtonPress, QEvent::MouseButtonRelease,
                                     QEvent::MouseButtonDblClick, QEvent::MouseButtonRelease};
    if (events.size() != want.size()) {
        return false;
    }
    for (qsizetype i = 0; i < want.size(); ++i) {
        if (events.at(i).type != want.at(i) || events.at(i).position != at) {
            return false;
        }
    }
    return true;
}

bool isDrag(const QJsonObject& reply, const QVector<Recorded>& events)
{
    const Qt::KeyboardModifiers mods = Qt::MetaModifier | Qt::ShiftModifier;
    const bool replyOk = ok(reply)
        && reply.value(QStringLiteral("target")).toString() == QStringLiteral("dragTarget")
        && reply.value(QStringLiteral("x")).toInt() == 10
        && reply.value(QStringLiteral("y")).toInt() == 12
        && reply.value(QStringLiteral("dx")).toInt() == 3
        && reply.value(QStringLiteral("dy")).toInt() == 4
        && reply.value(QStringLiteral("modifiers")).toInt() == static_cast<int>(mods);
    const QVector<Recorded> want{
        {QEvent::MouseButtonPress, QPoint(10, 12), Qt::LeftButton, Qt::LeftButton, mods},
        {QEvent::MouseMove, QPoint(11, 13), Qt::NoButton, Qt::LeftButton, mods},
        {QEvent::MouseMove, QPoint(12, 14), Qt::NoButton, Qt::LeftButton, mods},
        {QEvent::MouseMove, QPoint(13, 16), Qt::NoButton, Qt::LeftButton, mods},
        {QEvent::MouseButtonRelease, QPoint(13, 16), Qt::LeftButton, Qt::NoButton, mods},
    };
    if (!replyOk || events.size() != want.size()) {
        return false;
    }
    for (qsizetype i = 0; i < want.size(); ++i) {
        const Recorded& a = events.at(i);
        const Recorded& b = want.at(i);
        if (a.type != b.type || a.position != b.position || a.button != b.button
            || a.buttons != b.buttons || a.modifiers != b.modifiers) {
            return false;
        }
    }
    return true;
}

int countNodes(const QJsonObject& node, const QString& name)
{
    int n = node.value(QStringLiteral("objectName")).toString() == name ? 1 : 0;
    for (const QJsonValue& kid : node.value(QStringLiteral("children")).toArray()) {
        n += countNodes(kid.toObject(), name);
    }
    return n;
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aether-automation-boundary-widgets"));
    if (!profile.isValid()) {
        std::fprintf(stderr, "cannot create isolated settings profile\n");
        return 1;
    }
    QApplication app(argc, argv);
    AutomationServer server; // Never start(): no listener.
    const auto send = [&](const QByteArray& line) {
        const QJsonObject reply = AutomationServerTestAccess::request(server, line);
        settle();
        return reply;
    };

    RecordingWidget target;
    target.setObjectName(QStringLiteral("clickTarget"));
    target.resize(100, 80);
    target.show();
    settle();
    const auto fresh = [&] {
        target.events.clear();
        target.doubleClicks = 0;
    };

    fresh();
    QJsonObject reply = send("doubleClick clickTarget");
    check(ok(reply) && isDoubleClick(target.events, target.rect().center())
              && target.doubleClicks == 1,
          "doubleClick sends Press/Release/DblClick/Release at the centre", reply);
    fresh();
    reply = send("doubleClickAt clickTarget 10 12");
    check(ok(reply) && isDoubleClick(target.events, QPoint(10, 12)) && target.doubleClicks == 1,
          "doubleClickAt sends the double-click sequence at the point", reply);
    fresh();
    reply = send("clickAt clickTarget 10 12");
    check(ok(reply) && target.events.size() == 2
              && target.events.at(0).type == QEvent::MouseButtonPress
              && target.events.at(1).type == QEvent::MouseButtonRelease
              && target.doubleClicks == 0,
          "clickAt sends press+release and never a DblClick", reply);

    struct Fold { QByteArray json; QPoint at; const char* what; };
    const Fold folds[] = {
        {R"({"cmd":"doubleClickAt","target":"clickTarget","x":10,"y":12})", {10, 12},
         "JSON doubleClickAt folds x/y"},
        {R"({"cmd":"dblClickAt","target":"clickTarget","x":20,"y":22})", {20, 22},
         "JSON alias dblClickAt folds x/y"},
        {R"({"cmd":"doubleclickat","target":"clickTarget","x":30,"y":24})", {30, 24},
         "JSON alias doubleclickat folds x/y"},
        {R"({"cmd":"doubleClick","target":"clickTarget","x":40,"y":30})", {40, 30},
         "JSON doubleClick honours x/y instead of the centre"},
        {R"({"cmd":"doubleClickAt","target":"clickTarget","value":"50 40","x":"ignored"})",
         {50, 40}, "explicit value wins over malformed x/y"},
    };
    for (const Fold& fold : folds) {
        fresh();
        reply = send(fold.json);
        check(ok(reply) && isDoubleClick(target.events, fold.at), fold.what, reply);
    }
    fresh();
    reply = send(R"({"cmd":"clickAt","target":"clickTarget","x":11,"y":13})");
    check(ok(reply) && target.events.size() == 2
              && target.events.at(0).position == QPoint(11, 13) && target.doubleClicks == 0,
          "JSON clickAt folds x/y into a single click", reply);

    for (const QByteArray& line : {
             QByteArray(R"({"cmd":"doubleClickAt","target":"clickTarget","x":"10","y":12})"),
             QByteArray(R"({"cmd":"doubleClick","target":"clickTarget","x":"10","y":12})"),
             QByteArray(R"({"cmd":"doubleClick","target":"clickTarget","x":10})"),
             QByteArray(R"({"cmd":"doubleClick","target":"clickTarget","y":12})")}) {
        fresh();
        reply = send(line);
        check(!ok(reply) && reply.value(QStringLiteral("error")).toString()
                                .contains(QStringLiteral("numeric x and y"))
                  && target.events.isEmpty(),
              QStringLiteral("string or partial coordinates refused: ") + QString::fromUtf8(line),
              reply);
    }

    check(!ok(send("doubleClick")), "doubleClick without a target is refused");
    check(!ok(send("doubleClick noSuchWidget")), "doubleClick on a missing widget is refused");
    target.setEnabled(false);
    fresh();
    reply = send("doubleClick clickTarget");
    check(!ok(reply) && reply.value(QStringLiteral("disabled")).toBool() && target.events.isEmpty(),
          "doubleClick on a disabled widget is refused", reply);
    target.setEnabled(true);

    RecordingWidget dragTarget;
    dragTarget.setObjectName(QStringLiteral("dragTarget"));
    dragTarget.resize(100, 100);
    dragTarget.show();
    settle();
    reply = send("dragAt dragTarget 10 12 3 4 meta,shift");
    check(isDrag(reply, dragTarget.events), "bare dragAt keeps its args and mouse sequence", reply);
    dragTarget.events.clear();
    reply = send(R"({"cmd":"dragAt","target":"dragTarget","value":"10 12 3 4 meta,shift"})");
    check(isDrag(reply, dragTarget.events), "JSON dragAt keeps its args and mouse sequence", reply);

    // A Qt::Window parented to another widget is both a top-level and a child.
    QWidget floated(&dragTarget, Qt::Window);
    floated.setObjectName(QStringLiteral("floatedProbe"));
    floated.resize(80, 60);
    QWidget inner(&floated);
    inner.setObjectName(QStringLiteral("floatedChildProbe"));
    inner.setProperty("noiseFloorDbm", -122.5);
    inner.setProperty("panIndex", 7);
    floated.show();
    settle();
    const QJsonObject tree = send("dumpTree");
    int windows = 0;
    int children = 0;
    for (const QJsonValue& root : tree.value(QStringLiteral("roots")).toArray()) {
        windows += countNodes(root.toObject(), QStringLiteral("floatedProbe"));
        children += countNodes(root.toObject(), QStringLiteral("floatedChildProbe"));
    }
    check(ok(tree) && windows == 1 && children == 1,
          QStringLiteral("dumpTree serializes a parented window once (window=%1 child=%2)")
              .arg(windows).arg(children));
    int floorEntries = 0;
    for (const QJsonValue& entry : send("floors").value(QStringLiteral("floors")).toArray()) {
        floorEntries += entry.toObject().value(QStringLiteral("panIndex")).toInt() == 7 ? 1 : 0;
    }
    check(floorEntries == 1,
          QStringLiteral("floors counts a parented window's spectrum once (%1)").arg(floorEntries));

    server.setAuthToken(QStringLiteral("test-token"));
    dragTarget.events.clear();
    check(!ok(send("dragAt dragTarget 10 12 3 4 meta,shift")) && dragTarget.events.isEmpty(),
          "an unauthenticated dragAt is refused");
    reply = send(R"({"cmd":"dragAt","args":"dragTarget 10 12 3 4 meta,shift","token":"test-token"})");
    check(isDrag(reply, dragTarget.events),
          "authenticated JSON args use the positional dragAt parser", reply);
    server.setAuthToken({});

    QLineEdit field(QStringLiteral("Original"));
    field.setObjectName(QStringLiteral("commitProbe"));
    field.show();
    settle();
    QStringList committed;
    int submitted = 0;
    QObject::connect(&field, &QLineEdit::returnPressed, [&] { ++submitted; });
    QObject::connect(&field, &QLineEdit::editingFinished, [&] {
        if (field.isModified()) {
            committed.append(field.text());
            field.setModified(false);
        }
    });
    check(!ok(send("invoke commitProbe editText Changed"))
              && !ok(send("invoke commitProbe commit"))
              && field.text() == QStringLiteral("Original") && committed.isEmpty(),
          "editing and commit require owner opt-in");
    field.setProperty("aetherAutomationEditCommit", true);
    check(ok(send("invoke commitProbe setText Seed")) && !field.isModified()
              && committed.isEmpty() && submitted == 0,
          "setText still neither marks an edit nor submits");
    check(ok(send(R"({"cmd":"invoke","target":"commitProbe","action":"editText","value":""})"))
              && field.text().isEmpty() && field.isModified() && committed.isEmpty(),
          "empty editText arms an edit without committing");
    reply = send("invoke commitProbe commit");
    check(ok(reply) && reply.value(QStringLiteral("deferred")).toBool()
              && committed == QStringList{QString()} && !field.isModified() && submitted == 0,
          "commit reaches editingFinished, including empty text, without returnPressed", reply);
    check(ok(send("invoke commitProbe commit"))
              && ok(send(R"({"cmd":"invoke","target":"commitProbe","action":"editText","value":""})"))
              && ok(send("invoke commitProbe commit")) && committed.size() == 1,
          "unchanged edits and repeated commits do not re-arm the owner's guard");
    check(!ok(send("invoke commitProbe commit Unexpected")) && field.text().isEmpty(),
          "commit refuses a value rather than silently replacing the field");
    field.setReadOnly(true);
    check(!ok(send("invoke commitProbe editText Changed"))
              && !ok(send("invoke commitProbe commit")) && field.text().isEmpty(),
          "read-only field refuses editing and commit");
    field.setReadOnly(false);
    field.setEnabled(false);
    check(!ok(send("invoke commitProbe editText Changed"))
              && !ok(send("invoke commitProbe commit")), "disabled field refuses both actions");
    field.setEnabled(true);
    field.hide();
    check(!ok(send("invoke commitProbe editText Changed"))
              && !ok(send("invoke commitProbe commit")), "hidden field refuses both actions");
    field.show();
    settle();
    server.setReadOnly(true);
    check(!ok(send("invoke commitProbe editText Changed"))
              && !ok(send("invoke commitProbe commit")), "observe-only refuses both actions");
    server.setReadOnly(false);
    server.setAuthToken(QStringLiteral("test-token"));
    check(!ok(send("invoke commitProbe editText Changed"))
              && !ok(send("invoke commitProbe commit")), "unauthenticated edits and commits refused");
    server.setAuthToken({});
    check(ok(send("invoke commitProbe editText Pending")), "later edit remains drivable");
    const auto queueCommit = [&] {
        return AutomationServerTestAccess::request(server, "invoke commitProbe commit");
    };
    reply = queueCommit();
    field.setProperty("aetherAutomationEditCommit", false);
    settle();
    check(ok(reply) && committed.size() == 1 && field.isModified(),
          "revoked owner opt-in cancels a queued commit");
    field.setProperty("aetherAutomationEditCommit", true);
    reply = queueCommit();
    server.setReadOnly(true);
    settle();
    check(ok(reply) && committed.size() == 1 && field.isModified(),
          "observe-only enabled after admission cancels a queued commit");
    server.setReadOnly(false);
    reply = queueCommit();
    server.setAuthToken(QStringLiteral("rotated-token"));
    server.setAuthToken({});
    settle();
    check(ok(reply) && committed.size() == 1 && field.isModified(),
          "token rotation cancels an admitted commit even after authorization is restored");
    reply = queueCommit();
    field.setText(QStringLiteral("Newer"));
    field.setModified(true);
    settle();
    check(ok(reply) && committed.size() == 1 && field.isModified(),
          "queued commit does not consume a newer edit");
    check(ok(send("invoke commitProbe commit"))
              && committed == QStringList{QString(), QStringLiteral("Newer")},
          "a fresh authorized commit consumes the preserved pending edit");
    QIntValidator validator(0, 100);
    field.setValidator(&validator);
    check(ok(send("invoke commitProbe editText invalid"))
              && !ok(send("invoke commitProbe commit")) && committed.size() == 2,
          "commit honors the field validator instead of bypassing Qt acceptance");
    field.setValidator(nullptr);
    for (int state = 0; state < 3; ++state) {
        reply = queueCommit();
        if (state == 0) { field.setEnabled(false); }
        if (state == 1) { field.hide(); }
        if (state == 2) { field.setReadOnly(true); }
        settle();
        check(ok(reply) && committed.size() == 2 && field.isModified(),
              "queued commit rechecks field enabled/visible/read-only state");
        field.setEnabled(true);
        field.setReadOnly(false);
        field.show();
        settle();
    }

    std::printf("\n%s\n", failures == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return failures == 0 ? 0 : 1;
}
