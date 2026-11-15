#include "core/AutomationServer.h"

#include <QApplication>
#include <QJsonObject>
#include <QPushButton>
#include <QtTest>

namespace AetherSDR {
class AutomationServerTestAccess {
public:
    static QJsonObject request(AutomationServer& server, const QByteArray& line)
    { return server.handleLine(line, nullptr); }
};
}
using namespace AetherSDR;

class AutomationLabTest final : public QObject {
    Q_OBJECT
private slots:
    void permissionAndObserveOnly()
    {
        qunsetenv("AETHER_AUTOMATION_ALLOW_LAB_RF");
        AutomationServer bridge;
        int writes = 0;
        bridge.setLabHandler([&writes](const QString& action, const QString&) {
            if (action != QLatin1String("status") && action != QLatin1String("devices")) { ++writes; }
            return QJsonObject{{"ok", true}, {"action", action}};
        });
        const auto request = [&bridge](const QByteArray& line) {
            return AutomationServerTestAccess::request(bridge, line);
        };
        QVERIFY(!request("lab sweep 14000000 14350000 200 isolated").value("ok").toBool());
        QCOMPARE(writes, 0);
        bridge.setReadOnly(true);
        QVERIFY(request("lab status").value("ok").toBool());
        QVERIFY(request("lab devices").value("ok").toBool());
        QVERIFY(!request("lab connect 0").value("ok").toBool());
        QVERIFY(request("lab stop").value("ok").toBool());
        QVERIFY(!request("lab disconnect").value("ok").toBool());
        QCOMPARE(writes, 1);
        qputenv("AETHER_AUTOMATION_ALLOW_LAB_RF", "1");
        QVERIFY(!request("lab sweep 14000000 14350000 200 isolated").value("ok").toBool());
        bridge.setReadOnly(false);
        QVERIFY(request("lab sweep 14000000 14350000 200 isolated").value("ok").toBool());
        QVERIFY(request("lab disconnect").value("ok").toBool());
        QCOMPARE(writes, 3);
        qunsetenv("AETHER_AUTOMATION_ALLOW_LAB_RF");
    }
    void widgetRfGuard()
    {
        qunsetenv("AETHER_AUTOMATION_ALLOW_LAB_RF");
        QPushButton button;
        button.setObjectName("injectedAnalyzerSweep");
        button.setProperty("aetherLabRf", true);
        button.show();
        int clicks = 0;
        connect(&button, &QPushButton::clicked, &button, [&clicks] { ++clicks; });
        AutomationServer bridge;
        const QJsonObject denied = AutomationServerTestAccess::request(bridge, "invoke injectedAnalyzerSweep click");
        QVERIFY(!denied.value("ok").toBool());
        QCOMPARE(clicks, 0);
        qputenv("AETHER_AUTOMATION_ALLOW_LAB_RF", "1");
        const QJsonObject accepted = AutomationServerTestAccess::request(bridge, "invoke injectedAnalyzerSweep click");
        QVERIFY(!accepted.value("ok").toBool());
        QVERIFY(!AutomationServerTestAccess::request(bridge, "clickAt injectedAnalyzerSweep 3 3").value("ok").toBool());
        QVERIFY(!AutomationServerTestAccess::request(bridge, "doubleClick injectedAnalyzerSweep").value("ok").toBool());
        const QJsonObject drag = AutomationServerTestAccess::request(bridge, "drag injectedAnalyzerSweep 1 1");
        QVERIFY(!drag.value("ok").toBool());
        QVERIFY(drag.value("error").toString().contains("analyzer", Qt::CaseInsensitive));
        QCoreApplication::processEvents();
        QCOMPARE(clicks, 0);
        qunsetenv("AETHER_AUTOMATION_ALLOW_LAB_RF");
    }
};
QTEST_MAIN(AutomationLabTest)
#include "automation_lab_test.moc"
