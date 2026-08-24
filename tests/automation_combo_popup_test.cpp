// invoke <combo> showPopup / hidePopup (#5080): the drop-down is held open under
// bridge control and the open container is named aetherComboPopup; the name is
// cleared again on hidePopup and on a self-close, so it is only ever true of an
// open list. Offscreen: popup *visibility* is not asserted, the deferred reply,
// the name, and the name reset are.
#include "TestSettingsProfile.h"
#include "TestEventLoop.h"
#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/QsoRecorder.h"
#include "models/RadioModel.h"
#include "core/AutomationServer.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
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
QJsonObject invoke(QLocalSocket* s, const QString& target, const QString& action)
{
    return request(s, QJsonObject{{QStringLiteral("cmd"), QStringLiteral("invoke")},
                                  {QStringLiteral("target"), target},
                                  {QStringLiteral("action"), action}});
}
QWidget* popupOf(QComboBox* cb) { return cb->view() ? cb->view()->window() : nullptr; }
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("aether-automation-combo-popup-test"));
    if (!settingsProfile.isValid()) {
        std::fprintf(stderr, "FAIL could not create isolated settings profile\n");
        return 1;
    }
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("AetherSDR-test"));
    QCoreApplication::setApplicationName(QStringLiteral("AetherSDR-test"));
    AppSettings::instance().load();
    QWidget window; window.resize(300, 120);
    auto* layout = new QVBoxLayout(&window);
    QComboBox emptyCombo; emptyCombo.setObjectName(QStringLiteral("emptyCombo"));
    layout->addWidget(&emptyCombo);
    QComboBox combo; combo.setObjectName(QStringLiteral("modeCombo"));
    combo.addItems({QStringLiteral("USB"), QStringLiteral("CW"), QStringLiteral("AM")});
    layout->addWidget(&combo);
    QComboBox combo2; combo2.setObjectName(QStringLiteral("bandCombo"));
    combo2.addItems({QStringLiteral("20m"), QStringLiteral("40m")});
    layout->addWidget(&combo2);
    window.show(); waitUntil([&window]() { return window.isVisible(); });

    AutomationServer server; server.setAuthToken(QStringLiteral("test-token"));
#if defined(Q_OS_UNIX)
    const QString name = QStringLiteral("/tmp/aether-combo-%1").arg(QCoreApplication::applicationPid());
#else
    const QString name = QStringLiteral("aether-combo-%1").arg(QCoreApplication::applicationPid());
#endif
    expect("server starts", server.start(name));
    QLocalSocket client; client.connectToServer(server.fullServerName());
    expect("client connects", waitUntil([&client]() { return client.state() == QLocalSocket::ConnectedState; }));

    const QJsonObject notApplicable = invoke(&client, QStringLiteral("modeCombo"), QStringLiteral("bogusAction"));
    expect("unknown action is refused", !notApplicable.value(QStringLiteral("ok")).toBool());

    const QJsonObject emptyShown = invoke(&client, QStringLiteral("emptyCombo"), QStringLiteral("showPopup"));
    expect("showPopup on an empty combo is refused, not deferred",
           !emptyShown.value(QStringLiteral("ok")).toBool()
               && emptyShown.value(QStringLiteral("error")).toString().contains(QStringLiteral("no items")));
    AetherTest::waitFor([]() { return false; }, 50);   // one loop turn; nothing should get named
    expect("an empty combo never acquires the name",
           !popupOf(&emptyCombo) || popupOf(&emptyCombo)->objectName().isEmpty());

    const QJsonObject shown = invoke(&client, QStringLiteral("modeCombo"), QStringLiteral("showPopup"));
    expect("showPopup answers ok+deferred",
           shown.value(QStringLiteral("ok")).toBool() && shown.value(QStringLiteral("deferred")).toBool());
    expect("open container is named aetherComboPopup after one loop turn",
           waitUntil([&combo]() { QWidget* p = popupOf(&combo); return p && p->objectName() == QLatin1String("aetherComboPopup"); }));

    const QJsonObject hidden = invoke(&client, QStringLiteral("modeCombo"), QStringLiteral("hidePopup"));
    expect("hidePopup answers ok+deferred",
           hidden.value(QStringLiteral("ok")).toBool() && hidden.value(QStringLiteral("deferred")).toBool());
    expect("name is cleared by hidePopup",
           waitUntil([&combo]() { QWidget* p = popupOf(&combo); return p && p->objectName().isEmpty(); }));

    // Self-close path: open again, then close it the way a user would (not via the bridge).
    invoke(&client, QStringLiteral("modeCombo"), QStringLiteral("showPopup"));
    expect("re-open names the container again",
           waitUntil([&combo]() { QWidget* p = popupOf(&combo); return p && p->objectName() == QLatin1String("aetherComboPopup"); }));
    combo.hidePopup();   // item pick / Esc / click-away all end here
    expect("name is cleared on a self-close too",
           waitUntil([&combo]() { QWidget* p = popupOf(&combo); return p && p->objectName().isEmpty(); }));

    // Exactly one holder: opening a second combo's popup while the first is
    // still up must strip the name from the first container, whether or not
    // the platform auto-closes it (#5080 review).
    invoke(&client, QStringLiteral("modeCombo"), QStringLiteral("showPopup"));
    expect("first popup holds the name",
           waitUntil([&combo]() { QWidget* p = popupOf(&combo); return p && p->objectName() == QLatin1String("aetherComboPopup"); }));
    invoke(&client, QStringLiteral("bandCombo"), QStringLiteral("showPopup"));
    expect("the name moves to the second popup",
           waitUntil([&combo2]() { QWidget* p = popupOf(&combo2); return p && p->objectName() == QLatin1String("aetherComboPopup"); }));
    expect("the first container no longer answers to it",
           waitUntil([&combo]() { QWidget* p = popupOf(&combo); return p && p->objectName().isEmpty(); }));
    invoke(&client, QStringLiteral("bandCombo"), QStringLiteral("hidePopup"));
    expect("second popup's name clears on hide",
           waitUntil([&combo2]() { QWidget* p = popupOf(&combo2); return p && p->objectName().isEmpty(); }));

    std::printf("%s\n", gFailures == 0 ? "ALL PASS" : "FAILURES");
    return gFailures == 0 ? 0 : 1;
}
