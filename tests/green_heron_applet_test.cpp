// Offscreen tests for the GHE applet.
//
// The applet shows ONE switch — the one the operator says this radio is fed
// from — rather than the whole matrix the device serves. That choice is what
// these tests pin: the address fields persist as one owned settings object
// (Constitution Principle V), the chooser survives a roster arriving after
// the applet was built, and an antenna held by a DIFFERENT switch renders as
// in-use rather than as a click that would silently do nothing.
//
// Run:  QT_QPA_PLATFORM=offscreen ./build/green_heron_applet_test

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/GreenHeronProtocol.h"
#include "core/PeripheralSettings.h"
#include "gui/GreenHeronApplet.h"
#include "models/GreenHeronModel.h"

#include <QApplication>
#include <QComboBox>
#include <QDeadlineTimer>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTcpServer>
#include <QTcpSocket>

#include <cstdio>
#include <functional>
#include <string>

using namespace AetherSDR;

namespace {

int g_failed = 0;

// The single AppSettings key every peripheral's config lives under — mirrored
// from PeripheralSettings so the test asserts against the STORED shape, not
// just the accessors.
const char* const kPeripheralsRootKey = "Peripherals";

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-62s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.c_str());
    if (!ok) {
        ++g_failed;
    }
}

// Roster in the device's own announcement order (1, 3, 2, 4 — not sorted),
// plus the discriminating lock record: AS-84F-3 holds Beam-15 and the device
// reports it in SLOT 1, which is AS-84F-3's ANNOUNCEMENT index.
const QByteArray kScript =
    QByteArrayLiteral("SWITCHADD\x1f" "1\x1f" "AS-84F-1\x1f" "Beam-15\x1d" "0\x1d" "0\x1d" "false\x1f" "Beam-20\x1d" "0\x1d" "0\x1d" "false\x1f" "OFF\x1d" "0\x1d" "0\x1d" "false\r\n")
    + QByteArrayLiteral("SWITCHADD\x1f" "1\x1f" "AS-84F-3\x1f" "Beam-15\x1d" "0\x1d" "0\x1d" "false\x1f" "Beam-20\x1d" "0\x1d" "0\x1d" "false\x1f" "OFF\x1d" "0\x1d" "0\x1d" "false\r\n")
    + QByteArrayLiteral("SWITCHADD\x1f" "1\x1f" "AS-84F-2\x1f" "Beam-15\x1d" "0\x1d" "0\x1d" "false\x1f" "Beam-20\x1d" "0\x1d" "0\x1d" "false\x1f" "OFF\x1d" "0\x1d" "0\x1d" "false\r\n")
    + QByteArrayLiteral("SWITCHADD\x1f" "1\x1f" "AS-84F-4\x1f" "Beam-15\x1d" "0\x1d" "0\x1d" "false\x1f" "Beam-20\x1d" "0\x1d" "0\x1d" "false\x1f" "OFF\x1d" "0\x1d" "0\x1d" "false\r\n")
    + QByteArrayLiteral("SWITCHUPDATE\x1f" "AS-84F-1\x1f" "Beam-20\x1f" "0\x1f" "-27\r\n")
    + QByteArrayLiteral("SWITCHLOCKS\x1f" "AS-84F-1\x1f" "Beam-20\x1f" "Beam-15\x1f" "OFF\x1f" "OFF\r\n");

class FakeDevice : public QObject {
public:
    FakeDevice()
    {
        m_server.listen(QHostAddress::LocalHost, 0);
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            m_connection = m_server.nextPendingConnection();
            m_connection->write(kScript);
            m_connection->flush();
        });
    }

    quint16 port() const { return m_server.serverPort(); }

private:
    QTcpServer m_server;
    QTcpSocket* m_connection{nullptr};
};

// The same roster, but delivered the way the wire actually delivers it: one
// record per write with event-loop turns in between. FakeDevice's single
// write() lands the whole script in one readyRead, which hides every defect
// that only exists while the roster is half-arrived.
const QList<QByteArray> kRosterRecords = {
    QByteArrayLiteral("SWITCHADD\x1f" "1\x1f" "AS-84F-1\x1f" "Beam-15\x1d" "0\x1d" "0\x1d" "false\x1f" "Beam-20\x1d" "0\x1d" "0\x1d" "false\x1f" "OFF\x1d" "0\x1d" "0\x1d" "false\r\n"),
    QByteArrayLiteral("SWITCHADD\x1f" "1\x1f" "AS-84F-3\x1f" "Beam-15\x1d" "0\x1d" "0\x1d" "false\x1f" "Beam-20\x1d" "0\x1d" "0\x1d" "false\x1f" "OFF\x1d" "0\x1d" "0\x1d" "false\r\n"),
    QByteArrayLiteral("SWITCHADD\x1f" "1\x1f" "AS-84F-2\x1f" "Beam-15\x1d" "0\x1d" "0\x1d" "false\x1f" "Beam-20\x1d" "0\x1d" "0\x1d" "false\x1f" "OFF\x1d" "0\x1d" "0\x1d" "false\r\n"),
    QByteArrayLiteral("SWITCHADD\x1f" "1\x1f" "AS-84F-4\x1f" "Beam-15\x1d" "0\x1d" "0\x1d" "false\x1f" "Beam-20\x1d" "0\x1d" "0\x1d" "false\x1f" "OFF\x1d" "0\x1d" "0\x1d" "false\r\n"),
};

// Writes the roster one record at a time and stops, so the test can inspect the
// applet while only a prefix of the roster has been seen.
class DribblingDevice : public QObject {
public:
    DribblingDevice()
    {
        m_server.listen(QHostAddress::LocalHost, 0);
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            m_connection = m_server.nextPendingConnection();
        });
    }

    quint16 port() const { return m_server.serverPort(); }
    bool connected() const { return m_connection != nullptr; }

    // Sends the next SWITCHADD. Returns false once the roster is exhausted.
    bool sendNext()
    {
        if (m_connection == nullptr || m_next >= kRosterRecords.size()) {
            return false;
        }
        m_connection->write(kRosterRecords.at(m_next++));
        m_connection->flush();
        return true;
    }

private:
    QTcpServer m_server;
    QTcpSocket* m_connection{nullptr};
    int m_next{0};
};

bool waitFor(const std::function<bool()>& predicate, int timeoutMs = 5000)
{
    QDeadlineTimer deadline(timeoutMs);
    while (!deadline.hasExpired()) {
        if (predicate()) {
            return true;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return predicate();
}

QPushButton* portButton(const GreenHeronApplet& applet, const QString& port)
{
    const QList<QPushButton*> buttons = applet.findChildren<QPushButton*>();
    for (QPushButton* button : buttons) {
        if (button->accessibleName() == port) {
            return button;
        }
    }
    return nullptr;
}

void testAppletBasics()
{
    GreenHeronApplet applet;
    report("objectName addressable", applet.objectName() == "greenHeronApplet",
           applet.objectName().toStdString());
    report("owns its own model", applet.model() != nullptr);
    report("fits the 260px applet panel", applet.maximumWidth() == 260,
           std::to_string(applet.maximumWidth()));

    // The three fields the operator fills in.
    report("has an IP field",
           applet.findChild<QLineEdit*>(QStringLiteral("greenHeronHost")) != nullptr);
    report("has a port field",
           applet.findChild<QSpinBox*>(QStringLiteral("greenHeronPort")) != nullptr);
    report("has a switch chooser",
           applet.findChild<QComboBox*>(QStringLiteral("greenHeronSwitch")) != nullptr);

    auto* portSpin = applet.findChild<QSpinBox*>(QStringLiteral("greenHeronPort"));
    report("port defaults to the protocol port",
           portSpin != nullptr && portSpin->value() == GreenHeron::kDefaultPort,
           portSpin != nullptr ? std::to_string(portSpin->value()) : "missing");

    // It must embed as an ordinary child — the container framework supplies
    // the window when the operator floats it.
    QWidget host;
    auto* child = new GreenHeronApplet(&host);
    report("embeds as a child widget", child->parentWidget() == &host);
}

void testShowsOnlyTheChosenSwitch()
{
    FakeDevice device;
    GreenHeronApplet applet;
    auto* host = applet.findChild<QLineEdit*>(QStringLiteral("greenHeronHost"));
    auto* portSpin = applet.findChild<QSpinBox*>(QStringLiteral("greenHeronPort"));
    auto* combo = applet.findChild<QComboBox*>(QStringLiteral("greenHeronSwitch"));
    auto* connectBtn =
        applet.findChild<QPushButton*>(QStringLiteral("greenHeronConnect"));

    host->setText(QStringLiteral("127.0.0.1"));
    portSpin->setValue(device.port());
    connectBtn->click();

    const bool rostered = waitFor([&]() { return combo->count() == 4; });
    report("the chooser fills from the device's roster", rostered,
           std::to_string(combo->count()));

    QStringList shown;
    for (int i = 0; i < combo->count(); ++i) {
        shown.append(combo->itemText(i));
    }
    report("the chooser lists switches in display order",
           shown == QStringList({"AS-84F-1", "AS-84F-2", "AS-84F-3", "AS-84F-4"}),
           shown.join(QLatin1Char(',')).toStdString());

    // One switch on screen, not the matrix: three ports, not twelve.
    combo->setCurrentText(QStringLiteral("AS-84F-1"));
    const bool built = waitFor([&]() {
        return portButton(applet, QStringLiteral("Beam-20")) != nullptr;
    });
    report("the chosen switch's ports are drawn", built);
    report("only the chosen switch is drawn",
           applet.findChildren<QPushButton*>().size()
               == 3 + 1 /* the Connect button */,
           std::to_string(applet.findChildren<QPushButton*>().size()));

    // Selected on this switch → lit and clickable.
    QPushButton* selected = portButton(applet, QStringLiteral("Beam-20"));
    report("the device's selection is the one that lights",
           selected != nullptr && selected->isChecked());
    report("the selection is described for a screen reader",
           selected != nullptr
               && selected->accessibleDescription().contains(
                   QStringLiteral("currently selected")),
           selected != nullptr ? selected->accessibleDescription().toStdString() : "");

    // Held by AS-84F-3 (slot 1 = the SECOND switch ANNOUNCED). Indexing the
    // sorted order instead would name AS-84F-2 here — no crash, nothing
    // logged, and the tile confidently blames the wrong switch.
    QPushButton* locked = portButton(applet, QStringLiteral("Beam-15"));
    report("an antenna held elsewhere is not selectable",
           locked != nullptr && !locked->isEnabled());
    report("the holder named is the switch that ANNOUNCED at that slot",
           locked != nullptr && locked->text().contains(QStringLiteral("AS-84F-3")),
           locked != nullptr ? locked->text().toStdString() : "");

    // A free port stays clickable.
    QPushButton* free = portButton(applet, QStringLiteral("OFF"));
    report("a free antenna stays selectable",
           free != nullptr && free->isEnabled() && !free->isChecked());
}

void testSwitchChoiceAndAddressPersist()
{
    {
        FakeDevice device;
        GreenHeronApplet applet;
        applet.findChild<QLineEdit*>(QStringLiteral("greenHeronHost"))
            ->setText(QStringLiteral("127.0.0.1"));
        applet.findChild<QSpinBox*>(QStringLiteral("greenHeronPort"))
            ->setValue(device.port());
        applet.findChild<QPushButton*>(QStringLiteral("greenHeronConnect"))->click();

        auto* combo = applet.findChild<QComboBox*>(QStringLiteral("greenHeronSwitch"));
        waitFor([&]() { return combo->count() == 4; });
        combo->setCurrentText(QStringLiteral("AS-84F-3"));
        report("the applet reports the chosen switch",
               applet.selectedSwitch() == QLatin1String("AS-84F-3"),
               applet.selectedSwitch().toStdString());
    }

    report("host persisted",
           PeripheralSettings::deviceString(QStringLiteral("GreenHeron"),
                                            QStringLiteral("Host"))
               == QLatin1String("127.0.0.1"));
    report("switch choice persisted",
           PeripheralSettings::deviceString(QStringLiteral("GreenHeron"),
                                            QStringLiteral("Switch"))
               == QLatin1String("AS-84F-3"));

    // A fresh applet restores the remembered switch before any roster exists,
    // so the operator can see what it will re-select.
    GreenHeronApplet restored;
    report("the remembered switch is restored before connecting",
           restored.selectedSwitch() == QLatin1String("AS-84F-3"),
           restored.selectedSwitch().toStdString());
    report("the remembered host is restored",
           restored.findChild<QLineEdit*>(QStringLiteral("greenHeronHost"))->text()
               == QLatin1String("127.0.0.1"));
}

// The roster spans several TCP reads and has no terminator. An operator whose
// radio is fed from a late-announced switch must not lose that choice — nor
// have the loss written to settings — while the earlier records are still
// arriving. Announcing AS-84F-4 last is what makes this discriminating: it is
// absent from the roster for three of the four steps.
void testPartialRosterKeepsTheRememberedSwitch()
{
    PeripheralSettings::setDeviceString(QStringLiteral("GreenHeron"),
                                        QStringLiteral("Switch"),
                                        QStringLiteral("AS-84F-4"));

    DribblingDevice device;
    GreenHeronApplet applet;
    auto* host = applet.findChild<QLineEdit*>(QStringLiteral("greenHeronHost"));
    auto* portSpin = applet.findChild<QSpinBox*>(QStringLiteral("greenHeronPort"));
    auto* combo = applet.findChild<QComboBox*>(QStringLiteral("greenHeronSwitch"));
    auto* connectBtn =
        applet.findChild<QPushButton*>(QStringLiteral("greenHeronConnect"));

    host->setText(QStringLiteral("127.0.0.1"));
    portSpin->setValue(device.port());
    connectBtn->click();
    report("the dribbling device accepted the connection",
           waitFor([&]() { return device.connected(); }));

    // Records 1..3 — AS-84F-4 has not been announced yet at any of these steps.
    for (int sent = 1; sent <= 3; ++sent) {
        device.sendNext();
        const bool arrived =
            waitFor([&]() { return applet.model()->displayOrder().size() == sent; });
        report(("partial roster step " + std::to_string(sent) + " arrived").c_str(),
               arrived,
               std::to_string(applet.model()->displayOrder().size()));

        // The bug: the fallback used to write names.first() back to
        // m_wantedSwitch AND to settings here, discarding the choice for good.
        const QString persisted = PeripheralSettings::deviceString(
            QStringLiteral("GreenHeron"), QStringLiteral("Switch"));
        report(("a partial roster does not overwrite the persisted choice (step "
                + std::to_string(sent) + ")").c_str(),
               persisted == QLatin1String("AS-84F-4"), persisted.toStdString());

        // ...and nothing of ANOTHER switch's is clickable meanwhile. Not
        // persisting the fallback is only half of it: while AS-84F-4 is
        // unannounced, falling back to AS-84F-1 for display builds AS-84F-1's
        // port buttons live on screen, and a click in that window sends
        // SET_SWITCH to a switch the operator is not using. The remembered
        // switch has no roster yet, so the correct tile is an empty one.
        report(("no other switch's antennas are clickable meanwhile (step "
                + std::to_string(sent) + ")").c_str(),
               portButton(applet, QStringLiteral("Beam-20")) == nullptr);
        report(("the tile still names the operator's switch (step "
                + std::to_string(sent) + ")").c_str(),
               applet.selectedSwitch() == QLatin1String("AS-84F-4"),
               applet.selectedSwitch().toStdString());
    }

    // Record 4 announces it; the chooser must snap back on its own.
    device.sendNext();
    const bool restored =
        waitFor([&]() { return combo->currentText() == QLatin1String("AS-84F-4"); });
    report("the late-announced switch is re-selected once it arrives", restored,
           combo->currentText().toStdString());
    report("and it is the switch the tile drives",
           applet.selectedSwitch() == QLatin1String("AS-84F-4"),
           applet.selectedSwitch().toStdString());

    // The combo and the relays must never name different switches: the port
    // buttons send to selectedSwitch(), so a display-only fix would leave the
    // operator clicking AS-84F-4's antennas into AS-84F-1's relays.
    report("the chooser and the driven switch agree",
           combo->currentText() == applet.selectedSwitch(),
           (combo->currentText() + " vs " + applet.selectedSwitch()).toStdString());
}

void testConfigIsOneOwnedObject()
{
    // Constitution Principle V: the feature's configuration is one nested
    // object under one root key, not loose flat keys in the shared namespace.
    auto& settings = AppSettings::instance();
    const QString raw = settings.value(kPeripheralsRootKey, QString{}).toString();
    const QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8());
    report("peripherals config is one JSON object", doc.isObject(), raw.toStdString());

    const QJsonObject green =
        doc.object().value(QStringLiteral("GreenHeron")).toObject();
    report("Green Heron owns a sub-object inside it", !green.isEmpty());
    report("host lives in the object, not a flat key",
           green.contains(QStringLiteral("Host"))
               && !settings.contains("GreenHeron_Host")
               && !settings.contains("GHE_Host"));
    report("switch choice lives in the object",
           green.contains(QStringLiteral("Switch")));
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile("green_heron_applet_test");
    QApplication app(argc, argv);

    testAppletBasics();
    testShowsOnlyTheChosenSwitch();
    testSwitchChoiceAndAddressPersist();
    testPartialRosterKeepsTheRememberedSwitch();
    testConfigIsOneOwnedObject();

    std::printf(g_failed ? "\n%d check(s) FAILED\n" : "\nAll checks passed\n", g_failed);
    return g_failed ? 1 : 0;
}
