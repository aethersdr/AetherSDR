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
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QPushButton>
#include <QSpinBox>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>

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
            ++m_connections;
            connect(m_connection, &QTcpSocket::readyRead, this, [this]() {
                m_received += m_connection->readAll();
            });
            if (m_autoReplay) {
                replay();
            }
        });
    }

    quint16 port() const { return m_server.serverPort(); }
    int connections() const { return m_connections; }
    // Whatever the tile has sent us. The keepalive NUL is in here too.
    QByteArray received() const { return m_received; }

    void send(const QByteArray& data)
    {
        if (m_connection != nullptr) {
            m_connection->write(data);
            m_connection->flush();
        }
    }

    // Accept the next connection but stay silent — the reconnect window the
    // tile has to survive without going live on a pre-drop roster.
    void setAutoReplay(bool on) { m_autoReplay = on; }

    void replay()
    {
        if (m_connection != nullptr) {
            m_connection->write(kScript);
            m_connection->flush();
        }
    }

    void drop()
    {
        if (m_connection != nullptr) {
            m_connection->abort();
            m_connection->deleteLater();
            m_connection = nullptr;
        }
    }

private:
    QTcpServer m_server;
    QTcpSocket* m_connection{nullptr};
    QByteArray m_received;
    int m_connections{0};
    bool m_autoReplay{true};
};

// The rotator half of the same connection.
const QByteArray kRotorPoint =
    QByteArrayLiteral("POINT\x1f" "Rotor\x1f" "50.4\x1f" "0\x1f" "0\r\n");

QByteArray point(const char* name, const char* heading)
{
    return QByteArrayLiteral("POINT\x1f") + name + QByteArrayLiteral("\x1f")
           + heading + QByteArrayLiteral("\x1f" "0\x1f" "0\r\n");
}

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
    // Scoped to the port container rather than the whole tile: the tile also
    // carries Connect and, when a rotator is reporting, Turn, and counting
    // those made this check a tally of unrelated widgets.
    auto* portHost = applet.findChild<QWidget*>(QStringLiteral("greenHeronPorts"));
    report("only the chosen switch is drawn",
           portHost != nullptr && portHost->findChildren<QPushButton*>().size() == 3,
           std::to_string(portHost == nullptr
                              ? -1
                              : portHost->findChildren<QPushButton*>().size()));

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

void testReconnectLeavesTheTileDeadUntilReplay()
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
    combo->setCurrentText(QStringLiteral("AS-84F-1"));
    const bool live = waitFor([&]() {
        QPushButton* b = portButton(applet, QStringLiteral("OFF"));
        return b != nullptr && b->isEnabled();
    });
    report("the tile is live once the device has replayed", live);

    // The link blips and comes back, but the device has not spoken yet.
    device.setAutoReplay(false);
    device.drop();
    const bool back = waitFor(
        [&]() { return device.connections() >= 2 && applet.model()->isConnected(); },
        10000);
    report("the tile's link comes back before any replay", back,
           std::to_string(device.connections()));

    // The buttons are still on screen — they are the best information there
    // is — but they must not be clickable. They name a roster from before the
    // drop, and clicking one sends SET_SWITCH to real relays.
    QPushButton* retained = portButton(applet, QStringLiteral("OFF"));
    report("the retained buttons are still shown", retained != nullptr);
    report("but nothing on the retained panel is clickable",
           retained != nullptr && !retained->isEnabled());

    device.replay();
    report("the tile comes back to life once the device replays",
           waitFor([&]() {
               QPushButton* b = portButton(applet, QStringLiteral("OFF"));
               return b != nullptr && b->isEnabled();
           }));
}

// The cold-start half of testSwitchChoiceAndAddressPersist(), run in a SECOND
// PROCESS against the same settings directory. Everything the parent can assert
// about persistence reads back through the AppSettings singleton, which caches:
// a value that never reached SQLite and one that did are indistinguishable from
// inside the process that wrote it. Only a cold open can tell them apart, and
// "the operator's switch is still there tomorrow" is a claim about the file,
// not about a cache (review 4957410474, non-blocking finding).
//
// Deliberately no AppSettings::save() in the parent before it spawns: the
// production write path is supposed to commit, and if it ever stops doing so
// this test is the thing that should go red.
int runRestartChild()
{
    // What main.cpp does at startup (main.cpp:617). AppSettings::instance() is
    // a bare singleton — it does NOT read the database on first touch — so
    // without this the child would be asserting against an empty cache and
    // would pass or fail for the wrong reason.
    AppSettings::instance().load();
    GreenHeronApplet restored;
    report("a cold process restores the remembered switch",
           restored.selectedSwitch() == QLatin1String("AS-84F-3"),
           restored.selectedSwitch().toStdString());
    report("a cold process restores the remembered host",
           restored.findChild<QLineEdit*>(QStringLiteral("greenHeronHost"))->text()
               == QLatin1String("127.0.0.1"),
           restored.findChild<QLineEdit*>(QStringLiteral("greenHeronHost"))
               ->text().toStdString());
    report("a cold process reads the peripherals object out of the store",
           PeripheralSettings::deviceString(QStringLiteral("GreenHeron"),
                                            QStringLiteral("Switch"))
               == QLatin1String("AS-84F-3"));
    return g_failed ? 1 : 0;
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

    // …and the same claim made honestly, from a process that never saw this
    // one's cache. The child inherits HOME / XDG_CONFIG_HOME /
    // AETHER_SETTINGS_DIR from TestSettingsProfile through the environment, so
    // it opens THIS test's sandboxed store and no other.
    //
    // Two processes on one SQLite store is safe here, and not by luck. The
    // applet's constructor can write — refreshSwitchChoices() populating the
    // combo re-persists the remembered switch — but this process is parked in
    // waitForFinished() for the child's whole life and issues nothing, so
    // there is never more than one writer, which is exactly what WAL allows.
    // The child also cannot dial anything: PeripheralSettings::autoReconnect()
    // defaults to False, so it restores the address without opening a socket
    // to this test's long-dead ephemeral port.
    QProcess child;
    child.setProcessChannelMode(QProcess::MergedChannels);
    child.start(QCoreApplication::applicationFilePath(),
                {QStringLiteral("--restart-child")});
    const bool finished = child.waitForFinished(60000);
    const QString childOutput = QString::fromUtf8(child.readAll()).trimmed();
    report("the restart child ran to completion",
           finished && child.exitStatus() == QProcess::NormalExit,
           childOutput.toStdString());
    report("the choice survives a real restart, not just the settings cache",
           finished && child.exitCode() == 0,
           childOutput.toStdString());
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

// ── Rotator ─────────────────────────────────────────────────────────────────

// Connect a tile to `device` and wait until the antenna roster is on screen.
void bringUp(GreenHeronApplet& applet, FakeDevice& device)
{
    applet.findChild<QLineEdit*>(QStringLiteral("greenHeronHost"))
        ->setText(QStringLiteral("127.0.0.1"));
    applet.findChild<QSpinBox*>(QStringLiteral("greenHeronPort"))
        ->setValue(device.port());
    applet.findChild<QPushButton*>(QStringLiteral("greenHeronConnect"))->click();
    waitFor([&]() { return applet.model()->isReady(); });
}

void testRotorAppearsOnlyWhileItIsReporting()
{
    FakeDevice device;
    GreenHeronApplet applet;
    applet.show();
    bringUp(applet, device);

    auto* section = applet.findChild<QWidget*>(QStringLiteral("greenHeronRotorSection"));
    report("the tile has a rotator section", section != nullptr);
    // A server whose rotator controller is off announces nothing at all, and
    // most installations have no rotator — an empty rotator row would be a
    // permanent fixture claiming something the wire never said.
    report("no rotator row while the device reports no heading",
           section != nullptr && !section->isVisible());
    report("and nothing names a rotator", applet.selectedRotor().isEmpty(),
           applet.selectedRotor().toStdString());

    device.send(kRotorPoint);
    const bool appeared = waitFor([&]() { return !applet.selectedRotor().isEmpty(); });
    report("a POINT brings the rotator row up", appeared);
    report("the row is visible", section != nullptr && section->isVisible());

    auto* readout = applet.findChild<QLabel*>(QStringLiteral("greenHeronRotorReadout"));
    report("the readout shows the REPORTED heading",
           readout != nullptr && readout->text() == QStringLiteral("50.4°"),
           readout != nullptr ? readout->text().toStdString() : "missing");
}

void testChoosingAHeadingDoesNotSendIt()
{
    // The invariant that is easiest to "improve" away. There is no stop or
    // park verb in this protocol, so a rotation that starts cannot be recalled
    // in software — typing a heading may only PROPOSE one.
    FakeDevice device;
    GreenHeronApplet applet;
    applet.show();
    bringUp(applet, device);
    device.send(kRotorPoint);
    waitFor([&]() { return !applet.selectedRotor().isEmpty(); });

    auto* heading = applet.findChild<QLineEdit*>(QStringLiteral("greenHeronHeading"));
    report("the tile has a heading field", heading != nullptr);
    heading->setText(QStringLiteral("89.0"));
    waitFor([&]() { return false; }, 200);
    report("typing a heading transmits nothing",
           !device.received().contains(QByteArrayLiteral("TURN")),
           device.received().toHex(' ').toStdString());

    // ...and Turn is what transmits.
    applet.findChild<QPushButton*>(QStringLiteral("greenHeronTurn"))->click();
    const bool sent = waitFor([&]() {
        return device.received().contains(
            QByteArrayLiteral("TURN\x1f" "Rotor\x1f" "89.0\r"));
    });
    report("Turn sends the captured command bytes", sent,
           device.received().toHex(' ').toStdString());
}

void testTheReadoutNeverShowsWhatWasAskedFor()
{
    // Measured on real hardware: a commanded 64.3 settled at a mean 62.9 and
    // dithered over several degrees. Showing the commanded value as the
    // position would put a number on screen that no antenna is pointing at.
    FakeDevice device;
    GreenHeronApplet applet;
    applet.show();
    bringUp(applet, device);
    device.send(QByteArrayLiteral("POINT\x1f" "Rotor\x1f" "62.9\x1f" "0\x1f" "0\r\n"));
    waitFor([&]() { return !applet.selectedRotor().isEmpty(); });

    applet.findChild<QLineEdit*>(QStringLiteral("greenHeronHeading"))
        ->setText(QStringLiteral("64.3"));
    applet.findChild<QPushButton*>(QStringLiteral("greenHeronTurn"))->click();
    waitFor([&]() {
        return device.received().contains(QByteArrayLiteral("TURN"));
    });

    auto* readout = applet.findChild<QLabel*>(QStringLiteral("greenHeronRotorReadout"));
    const QString text = readout != nullptr ? readout->text() : QString{};
    report("the reported heading still leads the readout",
           text.startsWith(QStringLiteral("62.9°")), text.toStdString());
    report("the commanded heading is labelled as asked, not as position",
           text.contains(QStringLiteral("asked 64.3°")), text.toStdString());
    // 62.9 against 64.3 is 1.4 — and it is shown as a difference and nothing
    // more. No arrival verdict is possible at this hardware's ±3.8° rest
    // dither; any threshold would flicker.
    report("the gap is shown as a delta", text.contains(QLatin1String("1.4")),
           text.toStdString());
    report("and no arrival verdict is claimed",
           !text.contains(QLatin1String("target"), Qt::CaseInsensitive)
               && !text.contains(QLatin1String("arrived"), Qt::CaseInsensitive),
           text.toStdString());
}

void testAHeadingTypedWithACommaStillReachesTheWireAsADot()
{
    // Driven through real keystrokes, not setText(): setText bypasses the
    // validator, which is the half a human actually meets. QDoubleValidator
    // answers Intermediate for a foreign decimal mark, so "64,3" does land in
    // the field — and would go out as a group-separated 643 if sendTurn() did
    // not normalise it.
    FakeDevice device;
    GreenHeronApplet applet;
    applet.show();
    bringUp(applet, device);
    device.send(kRotorPoint);
    waitFor([&]() { return !applet.selectedRotor().isEmpty(); });

    auto* heading = applet.findChild<QLineEdit*>(QStringLiteral("greenHeronHeading"));
    heading->setFocus();
    QTest::keyClicks(heading, QStringLiteral("64,3"));
    report("a comma reaches the field rather than being swallowed",
           heading->text() == QLatin1String("64,3"), heading->text().toStdString());

    applet.findChild<QPushButton*>(QStringLiteral("greenHeronTurn"))->click();
    const bool sent = waitFor([&]() {
        return device.received().contains(
            QByteArrayLiteral("TURN\x1f" "Rotor\x1f" "64.3\r"));
    });
    report("and goes out as a dot, not as 643", sent,
           device.received().toHex(' ').toStdString());
}

// Everything that depends on GreenHeron::kRotorSilentAfterMs, in ONE wait.
//
// Three independent scenarios, each with its own tile and its own stand-in
// server, all set up before the clock starts and all asserted after it. Run as
// three separate tests they would pay the deadline three times, and that
// deadline is real time by design — the failure being tested cannot be
// provoked by sending anything, because the signal is the absence of a record,
// and a timeout a test may shorten is a timeout the test is no longer
// measuring. So the wait is shared instead of shortened.
void testTheCompassIsFloatingOnly()
{
    // The rose was refused in the docked rail — 248px of width spent restating
    // one number — and admitted only where the operator sized the window
    // themselves. So "is it visible" is a question about the dock mode, not
    // just about the rotator.
    FakeDevice device;
    GreenHeronApplet applet;
    applet.show();
    bringUp(applet, device);

    auto* compass =
        applet.findChild<QWidget*>(QStringLiteral("greenHeronRotorCompass"));
    report("the tile has a compass", compass != nullptr);
    report("no compass before any rotator reports",
           compass != nullptr && !compass->isVisible());

    device.send(kRotorPoint);
    waitFor([&]() { return !applet.selectedRotor().isEmpty(); });
    auto* section =
        applet.findChild<QWidget*>(QStringLiteral("greenHeronRotorSection"));
    report("a POINT brings the rotator row up", section != nullptr
                                                    && section->isVisible());
    // The discriminating one: the row is up, the rotator is live, and the
    // compass is still deliberately absent because the tile is docked.
    report("and the DOCKED tile still shows no compass", !compass->isVisible());

    applet.setFloating(true);
    report("floating brings the compass up", compass->isVisible());
    auto* readout =
        applet.findChild<QLabel*>(QStringLiteral("greenHeronRotorReadout"));
    report("the text readout is still there beside it — the dial adds, it does "
           "not replace",
           readout != nullptr && readout->isVisible()
               && readout->text() == QStringLiteral("50.4°"),
           readout != nullptr ? readout->text().toStdString() : "missing");

    applet.setFloating(false);
    report("docking it again puts the compass away", !compass->isVisible());
}

void testTheCompassTracksDueNorth()
{
    // Due north is heading 0.0, and 0.0 is exactly the value a change guard
    // written with qFuzzyCompare cannot distinguish from "unset" — Qt compares
    // relative to magnitude and documents it as unusable against zero. A
    // rotator pointing north is an ordinary thing for this widget to be handed.
    FakeDevice device;
    GreenHeronApplet applet;
    applet.show();
    bringUp(applet, device);
    applet.setFloating(true);

    device.send(point("Rotor", "180.0"));
    waitFor([&]() { return !applet.selectedRotor().isEmpty(); });
    const auto* compass = applet.compass();
    report("the dial follows a southward heading",
           compass != nullptr && compass->reportedHeading() > 179.9
               && compass->reportedHeading() < 180.1,
           compass != nullptr ? std::to_string(compass->reportedHeading()) : "missing");

    device.send(point("Rotor", "0"));
    const bool north = waitFor([&]() {
        return compass != nullptr && compass->reportedHeading() < 0.05;
    });
    report("and swings to due north rather than sticking at the last heading",
           north,
           compass != nullptr ? std::to_string(compass->reportedHeading()) : "missing");
    report("with nothing commanded, so no ghost tick is drawn",
           compass != nullptr && !compass->hasAskedHeading());
}

void testTheCompassCannotAimTheRotator()
{
    // The reason this test exists: a compass rose's natural affordance IS
    // click-to-point, and this protocol has no stop or park verb, so a
    // rotation that starts cannot be recalled. Clicking the dial must neither
    // transmit nor load the field that Turn reads — the latter would be one
    // Enter away from the former.
    FakeDevice device;
    GreenHeronApplet applet;
    applet.show();
    bringUp(applet, device);
    device.send(kRotorPoint);
    waitFor([&]() { return !applet.selectedRotor().isEmpty(); });
    applet.setFloating(true);

    auto* compass =
        applet.findChild<QWidget*>(QStringLiteral("greenHeronRotorCompass"));
    auto* heading =
        applet.findChild<QLineEdit*>(QStringLiteral("greenHeronHeading"));
    // A click needs real geometry, and the layout pass has not run yet.
    waitFor([&]() { return compass != nullptr && compass->width() > 20; }, 2000);
    report("the compass is up and laid out",
           compass != nullptr && compass->isVisible() && compass->width() > 20);

    // Due east of the hub — where a click-to-point dial would read 90° and
    // send TURN␟Rotor␟90.0␍.
    QTest::mouseClick(compass, Qt::LeftButton, Qt::NoModifier,
                      QPoint(compass->width() - 4, compass->height() / 2));
    waitFor([]() { return false; }, 200);

    report("clicking the dial transmits nothing",
           !device.received().contains(QByteArrayLiteral("TURN")),
           device.received().toHex(' ').toStdString());
    report("and does not even propose a heading",
           heading != nullptr && heading->text().isEmpty(),
           heading != nullptr ? heading->text().toStdString() : "missing");
}

void testWhatHappensWhenARotatorGoesQuiet()
{
    // ── (a) one rotator, none chosen: the row and its controls go away, and
    //        the antenna list — a different half of the same device — does not.
    FakeDevice deviceA;
    GreenHeronApplet appletA;
    appletA.show();
    bringUp(appletA, deviceA);
    deviceA.send(kRotorPoint);
    waitFor([&]() { return !appletA.selectedRotor().isEmpty(); });
    auto* sectionA =
        appletA.findChild<QWidget*>(QStringLiteral("greenHeronRotorSection"));
    report("(a) the rotator row is up to begin with", sectionA->isVisible());
    // Floated, so the compass is up too and this case covers it on the SAME
    // wait — the dial must not survive the rotator it is drawing. A needle
    // left pointing at a stale heading after the controller is switched off is
    // the exact failure the section's gate exists to prevent, and it is
    // invisible to every other test here.
    appletA.setFloating(true);
    auto* compassA =
        appletA.findChild<QWidget*>(QStringLiteral("greenHeronRotorCompass"));
    report("(a) and so is the compass, the tile being floated",
           compassA != nullptr && compassA->isVisible());

    // ── (b) two rotators, the operator picks the second: when only THAT one
    //        goes quiet the tile must not fall back to the other. The switch
    //        half learned this the hard way; it is worse here, because a
    //        rotation has no stop verb to recall it.
    FakeDevice deviceB;
    GreenHeronApplet appletB;
    appletB.show();
    bringUp(appletB, deviceB);
    deviceB.send(point("Rotor-A", "10.0") + point("Rotor-B", "200.0"));
    waitFor([&]() { return appletB.model()->rotorNames().size() == 2; });
    auto* comboB = appletB.findChild<QComboBox*>(QStringLiteral("greenHeronRotor"));
    report("(b) the chooser appears once there are two rotators",
           comboB != nullptr && comboB->isVisible());
    comboB->setCurrentText(QStringLiteral("Rotor-B"));
    report("(b) the operator's rotator is the one shown",
           appletB.selectedRotor() == QLatin1String("Rotor-B"),
           appletB.selectedRotor().toStdString());
    auto* sectionB =
        appletB.findChild<QWidget*>(QStringLiteral("greenHeronRotorSection"));

    // ── (c) an "asked" heading must not outlive the power cycle it belonged
    //        to, or it comes back beside a live reading as a command this
    //        session never sent.
    FakeDevice deviceC;
    GreenHeronApplet appletC;
    appletC.show();
    bringUp(appletC, deviceC);
    deviceC.send(kRotorPoint);
    waitFor([&]() { return !appletC.selectedRotor().isEmpty(); });
    appletC.findChild<QLineEdit*>(QStringLiteral("greenHeronHeading"))
        ->setText(QStringLiteral("89.0"));
    appletC.findChild<QPushButton*>(QStringLiteral("greenHeronTurn"))->click();
    auto* readoutC =
        appletC.findChild<QLabel*>(QStringLiteral("greenHeronRotorReadout"));
    report("(c) the asked heading shows while the rotator is live",
           readoutC->text().contains(QStringLiteral("asked 89.0°")),
           readoutC->text().toStdString());
    auto* sectionC =
        appletC.findChild<QWidget*>(QStringLiteral("greenHeronRotorSection"));

    // ── the one wait. Rotor-A keeps reporting throughout, which is what makes
    //    (b) discriminating: a fallback would have somewhere to fall.
    QDeadlineTimer deadline(GreenHeron::kRotorSilentAfterMs + 5000);
    while (!deadline.hasExpired()
           && (sectionA->isVisible() || sectionB->isVisible()
               || sectionC->isVisible())) {
        deviceB.send(point("Rotor-A", "10.0"));
        waitFor([]() { return false; }, 250);
    }

    report("(a) the row goes away when the headings stop", !sectionA->isVisible());
    report("(a) and the compass goes with it, not left on a stale heading",
           compassA != nullptr && !compassA->isVisible());
    report("(a) and nothing still names a rotator",
           appletA.selectedRotor().isEmpty(), appletA.selectedRotor().toStdString());
    report("(a) the antenna list is untouched — a different half of the device",
           portButton(appletA, QStringLiteral("Beam-20")) != nullptr
               && appletA.model()->isReady());

    report("(b) Rotor-A is still reporting",
           appletB.model()->isRotorLive(QStringLiteral("Rotor-A")));
    report("(b) the tile does NOT fall back to the other rotator",
           appletB.selectedRotor().isEmpty(), appletB.selectedRotor().toStdString());
    report("(b) and the controls are gone rather than aimed elsewhere",
           !sectionB->isVisible());

    // (c) the controller comes back reporting a heading nobody commanded.
    deviceC.send(point("Rotor", "12.0"));
    report("(c) the rotator comes back", waitFor([&]() { return sectionC->isVisible(); }));
    report("(c) with no stale asked heading beside it",
           readoutC->text() == QStringLiteral("12.0°"),
           readoutC->text().toStdString());
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
    // The child of testSwitchChoiceAndAddressPersist(). It must NOT build a
    // TestSettingsProfile — that would mint a fresh temp dir and it would find
    // an empty store. It adopts the parent's, inherited through the
    // environment, and mirrors the two QSettings redirections the profile makes
    // so the first-run legacy probe cannot reach the real user's settings.
    if (argc > 1 && QLatin1String(argv[1]) == QLatin1String("--restart-child")) {
        QStandardPaths::setTestModeEnabled(true);
        const QString legacyRoot =
            qEnvironmentVariable("HOME") + QStringLiteral("/legacy-settings");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, legacyRoot);
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, legacyRoot);
        QApplication childApp(argc, argv);
        return runRestartChild();
    }

    TestSettingsProfile profile("green_heron_applet_test");
    QApplication app(argc, argv);

    // main.cpp:617 does this at startup, and AppSettings::instance() does NOT
    // do it lazily: without it the store is never opened, save() has no
    // database to commit to, and every "persisted" assertion below is really
    // just reading back the in-memory cache. That is exactly the hole the
    // restart child exists to close — it found this.
    AppSettings::instance().load();

    testAppletBasics();
    testShowsOnlyTheChosenSwitch();
    testReconnectLeavesTheTileDeadUntilReplay();
    testSwitchChoiceAndAddressPersist();
    testPartialRosterKeepsTheRememberedSwitch();
    testConfigIsOneOwnedObject();
    testRotorAppearsOnlyWhileItIsReporting();
    testChoosingAHeadingDoesNotSendIt();
    testTheReadoutNeverShowsWhatWasAskedFor();
    testAHeadingTypedWithACommaStillReachesTheWireAsADot();
    testTheCompassIsFloatingOnly();
    testTheCompassCannotAimTheRotator();
    testTheCompassTracksDueNorth();
    testWhatHappensWhenARotatorGoesQuiet();

    std::printf(g_failed ? "\n%d check(s) FAILED\n" : "\nAll checks passed\n", g_failed);
    return g_failed ? 1 : 0;
}
