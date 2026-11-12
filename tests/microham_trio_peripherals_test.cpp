// The microHAM Trio on the Peripherals page, read only: the row is offered only
// with a Trio model, its address lands under Peripherals.trio, the list line
// reads DIRECT / OFFLINE / Needs attention, the detail page shows the state and
// every label the Trio reports, the Web UI button lets go of the session before
// opening the Trio's page, Remove clears the address, and nothing the page does
// sends anything that selects. A scripted Trio (tests/MicrohamTrioTestPeer.h):
// no socket. Offscreen; a private settings profile.
//
// Run:  QT_QPA_PLATFORM=offscreen ./build/microham_trio_peripherals_test

#include <QList>
#include <QObject>
#include <QUrl>

// Receives what the Web UI button would open in the browser.
class UrlCatcher : public QObject {
    Q_OBJECT

public:
    QList<QUrl> urls;

public slots:
    void open(const QUrl& url) { urls.append(url); }
};

#include "MicrohamTrioTestPeer.h"
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/MicrohamTrioSettings.h"
#include "core/PeripheralSettings.h"
#include "gui/RadioSetupDialog.h"
#include "models/MicrohamTrioModel.h"
#include "models/RadioModel.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QSpinBox>
#include <QUrl>

#include <cstdio>
#include <string>

using namespace AetherSDR;
using TrioTestPeer::Peer;
using TrioTestPeer::clientText;
using TrioTestPeer::stateDoc;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-74s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.c_str());
    if (!ok) {
        ++g_failed;
    }
}

const char* const kHost = "192.0.2.10";

QAction* addAction(QMenu* menu, const QString& id)
{
    for (QAction* action : menu->actions()) {
        if (action->data().toString() == id) {
            return action;
        }
    }
    return nullptr;
}

QString listLine(QListWidget* list, const QString& id)
{
    for (int i = 0; i < list->count(); ++i) {
        if (list->item(i)->data(Qt::UserRole).toString() == id) {
            return list->item(i)->text();
        }
    }
    return {};
}

// Nothing the model sent selects anything: the handshake, keep-alives and close only.
bool sentOnlyReads(const Peer& peer)
{
    for (const QByteArray& bytes : peer.transport.sent) {
        if (bytes.startsWith("GET /") || static_cast<quint8>(bytes.at(0)) == 0x88) {
            continue;
        }
        if (clientText(bytes) != QByteArrayLiteral("ping")) {
            return false;
        }
    }
    return true;
}

void testNoModelNoRow()
{
    AppSettings::instance().remove(QStringLiteral("Peripherals"));
    RadioModel radio;
    RadioSetupDialog dialog(&radio);
    dialog.selectTab(QStringLiteral("Peripherals"));
    auto* add = dialog.findChild<QPushButton*>(QStringLiteral("peripheralAddButton"));
    report("without a Trio model the Add menu offers no Trio",
           add && add->menu() && !addAction(add->menu(), QStringLiteral("trio")));
}

void testRowLifecycle()
{
    AppSettings::instance().remove(QStringLiteral("Peripherals"));
    RadioModel radio;
    Peer peer;
    MicrohamTrioModel trio(peer.environment());
    trio.loadSettings(true);
    RadioSetupDialog dialog(&radio, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                            &trio);
    dialog.selectTab(QStringLiteral("Peripherals"));
    auto* add = dialog.findChild<QPushButton*>(QStringLiteral("peripheralAddButton"));
    auto* list = dialog.findChild<QListWidget*>(QStringLiteral("peripheralDeviceList"));
    QAction* action = add && add->menu() ? addAction(add->menu(), QStringLiteral("trio")) : nullptr;
    report("the Add menu offers the microHAM Trio",
           action && action->text() == QStringLiteral("microHAM Trio"));
    if (!action || !list) {
        return;
    }
    action->trigger();
    report("Add lists it, not yet configured",
           listLine(list, QStringLiteral("trio")).contains(QStringLiteral("Address not set")));

    auto* address = dialog.findChild<QLineEdit*>(QStringLiteral("peripheralAddress_trio"));
    auto* port = dialog.findChild<QSpinBox*>(QStringLiteral("peripheralPort_trio"));
    auto* connectButton = dialog.findChild<QPushButton*>(QStringLiteral("peripheralConnect_trio"));
    auto* status = dialog.findChild<QLabel*>(QStringLiteral("peripheralStatus_trio"));
    auto* state = dialog.findChild<QLabel*>(QStringLiteral("peripheralDeviceInfo_trio"));
    auto* webUi = dialog.findChild<QPushButton*>(QStringLiteral("peripheralWebUi_trio"));
    auto* autoConnect = dialog.findChild<QCheckBox*>(QStringLiteral("peripheralAutoConnect_trio"));
    report("the page has address, port, Connect, Web UI, Connect automatically and a state field",
           address && port && connectButton && status && state && webUi && autoConnect);
    if (!address || !port || !connectButton || !status || !state || !webUi || !autoConnect) {
        return;
    }
    report("the port defaults to the web port 80", port->value() == 80);
    report("before any state the state field says so",
           state->text().contains(QStringLiteral("No state read yet")));

    address->setText(QLatin1String(kHost));
    connectButton->click();
    report("Connect stores Peripherals.trio.ManualIp and ManualPort",
           PeripheralSettings::deviceString(QStringLiteral("trio"), QStringLiteral("ManualIp"))
                   == QLatin1String(kHost)
               && PeripheralSettings::deviceInt(QStringLiteral("trio"), QStringLiteral("ManualPort"), 0)
                      == 80);
    report("and the session starts", trio.hasSession() && trio.isConnecting());
    report("held with the state", peer.connectHeld(trio, "radio2", "ant6"));
    report("the list line reads DIRECT",
           listLine(list, QStringLiteral("trio")).contains(QStringLiteral("DIRECT")),
           listLine(list, QStringLiteral("trio")).toStdString());
    report("the status reads Connected", status->text() == QStringLiteral("Connected"),
           status->text().toStdString());
    report("Connect became Disconnect and the address is locked",
           connectButton->text() == QStringLiteral("Disconnect") && !address->isEnabled());
    report("the state field shows focus, antenna and labels",
           state->text().contains(QStringLiteral("Radio with focus: R2 “HL2”"))
               && state->text().contains(QStringLiteral("Antenna: A6 “Dummy”"))
               && state->text().contains(QStringLiteral("A2 Fan Dipole")),
           state->text().toStdString());

    auto* slot = dialog.findChild<QComboBox*>(QStringLiteral("peripheralTrioSlot"));
    report("This Radio lists the Trio's radio ports, disabled with no radio connected",
           slot && !slot->isEnabled() && slot->findData(QStringLiteral("radio2")) >= 0
               && state->text().contains(QStringLiteral("This radio: no radio connected")));

    peer.hold(trio, 2000);
    peer.push(stateDoc("radio1", "ant1"));
    report("a front-panel change reaches the page",
           state->text().contains(QStringLiteral("Radio with focus: R1 “FT710”"))
               && state->text().contains(QStringLiteral("Antenna: A1 “EFHW”")));

    // Another client takes the session.
    peer.transport.peerCloses();
    report("an eviction: OFFLINE, Needs attention",
           listLine(list, QStringLiteral("trio")).contains(QStringLiteral("OFFLINE"))
               && listLine(list, QStringLiteral("trio")).contains(QStringLiteral("Needs attention")),
           listLine(list, QStringLiteral("trio")).toStdString());
    report("and the status says another client took it",
           status->text().contains(QStringLiteral("another client")), status->text().toStdString());
    report("the last state stays, marked as last read",
           state->text().contains(QStringLiteral("as last read")));
    const int opens = peer.transport.opens;
    peer.advance(trio, 600000);
    report("nothing reconnects by itself", peer.transport.opens == opens);

    connectButton->click();
    report("Connect takes it back, attention cleared",
           peer.connectHeld(trio) && status->text() == QStringLiteral("Connected"));

    // Web UI: the session is let go first, then the Trio's page opens.
    UrlCatcher catcher;
    QDesktopServices::setUrlHandler(QStringLiteral("http"), &catcher, "open");
    webUi->click();
    QDesktopServices::unsetUrlHandler(QStringLiteral("http"));
    report("Web UI opens the Trio's page", catcher.urls.size() == 1
               && catcher.urls.first() == QUrl(QStringLiteral("http://192.0.2.10/")),
           catcher.urls.isEmpty() ? std::string() : catcher.urls.first().toString().toStdString());
    report("after closing this app's session",
           !trio.hasSession() && trio.view().session == MicrohamTrioModel::SessionWord::Released);
    report("and the status says so",
           status->text().contains(QStringLiteral("released to the Trio's web page")),
           status->text().toStdString());

    report("nothing the page did sent a selection", sentOnlyReads(peer));

    // Test Connection: one session, read, released.
    auto* testButton = dialog.findChild<QPushButton*>(QStringLiteral("peripheralTest_trio"));
    auto* testResult = dialog.findChild<QLabel*>(QStringLiteral("peripheralTrioTestResult"));
    report("the page has Test Connection and its result field", testButton && testResult);
    if (testButton && testResult) {
        testButton->click();
        peer.connectHeld(trio, "radio2", "ant6");
        report("the test reports reachability and the labels read",
               testResult->text().contains(QStringLiteral("Reachable"))
                   && testResult->text().contains(QStringLiteral("A6 Dummy"))
                   && testResult->isVisibleTo(&dialog),
               testResult->text().toStdString());
        report("and leaves no session behind", !trio.hasSession());
    }
    report("still nothing sent that selects", sentOnlyReads(peer));

    // Remove: address gone, row gone.
    RadioSetupDialog::setRemovalConfirmationHookForTest(
        [](const QString&, const QString&) { return true; });
    autoConnect->setChecked(false);
    auto* remove = dialog.findChild<QPushButton*>(QStringLiteral("peripheralRemoveButton"));
    for (int i = 0; i < list->count(); ++i) {
        if (list->item(i)->data(Qt::UserRole).toString() == QStringLiteral("trio")) {
            list->setCurrentRow(i);
        }
    }
    if (remove) {
        remove->click();
    }
    report("Remove clears the address and Connect automatically, and drops the row",
           PeripheralSettings::deviceString(QStringLiteral("trio"), QStringLiteral("ManualIp")).isEmpty()
               && PeripheralSettings::autoConnect(QStringLiteral("trio"))
               && listLine(list, QStringLiteral("trio")).isEmpty()
               && trio.view().session == MicrohamTrioModel::SessionWord::NotConfigured);
    RadioSetupDialog::setRemovalConfirmationHookForTest({});
}

void testConfiguredRowIsListed()
{
    AppSettings::instance().remove(QStringLiteral("Peripherals"));
    PeripheralSettings::setVisibleDeviceIds({});
    MicrohamTrioSettings saved;
    saved.host = QLatin1String(kHost);
    saved.save();
    RadioModel radio;
    Peer peer;
    MicrohamTrioModel trio(peer.environment());
    trio.loadSettings(false);
    RadioSetupDialog dialog(&radio, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                            &trio);
    dialog.selectTab(QStringLiteral("Peripherals"));
    auto* list = dialog.findChild<QListWidget*>(QStringLiteral("peripheralDeviceList"));
    const QString line = list ? listLine(list, QStringLiteral("trio")) : QString();
    report("a stored Trio is listed even with an empty saved list, OFFLINE while off",
           line.contains(QLatin1String(kHost)) && line.contains(QStringLiteral("OFFLINE"))
               && !trio.hasSession(),
           line.toStdString());
    MicrohamTrioSettings::remove();
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("microham-trio-peripherals"));
    if (!profile.isValid()) {
        return 1;
    }
    QApplication app(argc, argv);
    AppSettings::instance().load();
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    testNoModelNoRow();
    testRowLifecycle();
    testConfiguredRowIsListed();

    std::printf("\n%s (%d failed)\n", g_failed == 0 ? "ALL PASSED" : "FAILURES", g_failed);
    return g_failed == 0 ? 0 : 1;
}

#include "microham_trio_peripherals_test.moc"
