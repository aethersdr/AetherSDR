// The microHAM Trio on the Peripherals page: the row is offered only
// with a Trio model, its address lands under Peripherals.trio, the list line
// reads DIRECT / OFFLINE / Needs attention, the detail page shows the state and
// every label the Trio reports, the Web UI button lets go of the session before
// opening the Trio's page, Remove clears the address, every string the Trio
// supplies is shown as plain text, and nothing the page does sends anything
// that selects. A scripted Trio (tests/MicrohamTrioTestPeer.h):
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
#include <QRadioButton>
#include <QDesktopServices>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QTimer>
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
        const QByteArray text = clientText(bytes);
        if (text != QByteArrayLiteral("ping") && !text.startsWith(R"({"a":"set_labels")")) {
            return false;   // anything else would select something
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

    auto* radio2Name = dialog.findChild<QLineEdit*>(QStringLiteral("peripheralTrioName_radio_radio2"));
    auto* radio2This = dialog.findChild<QRadioButton*>(QStringLiteral("peripheralTrioThis_radio2"));
    auto* ant6Name = dialog.findChild<QLineEdit*>(QStringLiteral("peripheralTrioName_antenna_ant6"));
    auto* ant6Dummy = dialog.findChild<QCheckBox*>(QStringLiteral("peripheralTrioDummy_ant6"));
    report("Radio Ports and Antenna Ports list the Trio's names, editable",
           radio2Name && radio2Name->text() == QStringLiteral("HL2") && ant6Name
               && ant6Name->text() == QStringLiteral("Dummy") && ant6Dummy);
    report("the marker \"this AetherSDR\" is disabled with no radio connected",
           radio2This && !radio2This->isEnabled()
               && state->text().contains(QStringLiteral("This radio: no radio connected")));
    if (ant6Dummy) {
        ant6Dummy->click();
    }
    report("Dummy load marks ant6 with its label, per Trio",
           PeripheralSettings::deviceString(QStringLiteral("trio"), QStringLiteral("DummyLoadKey"))
                   == QStringLiteral("ant6")
               && PeripheralSettings::deviceString(QStringLiteral("trio"), QStringLiteral("DummyLoadLabel"))
                      == QStringLiteral("Dummy"));
    report("choosing markers sent nothing", sentOnlyReads(peer));

    // Write names: the set_labels command, and its reply.
    auto* writeNames = dialog.findChild<QPushButton*>(QStringLiteral("peripheralTrioWriteNames"));
    auto* namesResult = dialog.findChild<QLabel*>(QStringLiteral("peripheralTrioNamesResult"));
    auto* ant1Name = dialog.findChild<QLineEdit*>(QStringLiteral("peripheralTrioName_antenna_ant1"));
    if (writeNames && namesResult && ant1Name && ant6Name) {
        ant1Name->setText(QStringLiteral("EFHW 80"));
        ant1Name->setModified(true);
        writeNames->click();
        const QByteArray sent = peer.transport.sent.isEmpty() ? QByteArray()
                                                               : clientText(peer.transport.sent.last());
        report("Write Names sends one set_labels with the changed name only",
               sent.startsWith(R"({"a":"set_labels","c":[{"bid":"ant1","gid":"antenna","l":"EFHW 80"}],"id":")"),
               sent.toStdString());
        const QByteArray id = sent.mid(sent.indexOf("\"id\":\"") + 6, 36);
        peer.push(stateDoc("radio2", "ant6", id).replace("\"l\":\"EFHW\"", "\"l\":\"EFHW 80\""));
        report("its reply confirms it", namesResult->text().contains(QStringLiteral("written")),
               namesResult->text().toStdString());

        // Renaming the dummy-load port asks first.
        QStringList asked;
        RadioSetupDialog::setTrioConfirmationHookForTest(
            [&asked](const QString&, const QString& text) { asked << text; return false; });
        const int before = peer.transport.sent.size();
        ant6Name->setText(QStringLiteral("Load"));
        ant6Name->setModified(true);
        writeNames->click();
        report("renaming the dummy-load port asks, and a No sends nothing",
               asked.size() == 1 && asked.first().contains(QStringLiteral("dummy load"))
                   && peer.transport.sent.size() == before);
        RadioSetupDialog::setTrioConfirmationHookForTest({});
        ant6Name->setText(QStringLiteral("Dummy"));
        ant6Name->setModified(false);
    }

    // A rename outside AetherSDR: warning, until confirmed.
    auto* dummyWarning = dialog.findChild<QLabel*>(QStringLiteral("peripheralTrioDummyWarning"));
    auto* confirmDummy = dialog.findChild<QPushButton*>(QStringLiteral("peripheralTrioConfirmDummy"));
    peer.push(stateDoc("radio2", "ant6", TrioTestPeer::kZeroId, "HL2", "Spare"));
    report("the dummy-load port renamed at the Trio: a warning naming both labels",
           dummyWarning && dummyWarning->isVisibleTo(&dialog)
               && dummyWarning->text().contains(QStringLiteral("Spare"))
               && dummyWarning->text().contains(QStringLiteral("Dummy")),
           dummyWarning ? dummyWarning->text().toStdString() : std::string());
    if (confirmDummy) {
        confirmDummy->click();
    }
    report("Confirm Dummy Load accepts the new label",
           PeripheralSettings::deviceString(QStringLiteral("trio"), QStringLiteral("DummyLoadLabel"))
                   == QStringLiteral("Spare")
               && dummyWarning && !dummyWarning->isVisibleTo(&dialog));
    peer.push(stateDoc("radio2", "ant6"));
    if (confirmDummy) {
        confirmDummy->click();
    }

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
    report("and only one rename went out", [&peer]() {
        int labels = 0;
        for (const QByteArray& bytes : peer.transport.sent) {
            labels += clientText(bytes).contains("set_labels") ? 1 : 0;
        }
        return labels == 1;
    }());

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

// The Trio's names and labels are the device's text, and anyone answering at
// its address supplies them: every place one is shown takes it as plain text.
void testDeviceTextIsPlain()
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
    QAction* action = add && add->menu() ? addAction(add->menu(), QStringLiteral("trio")) : nullptr;
    if (!action) {
        report("markup: the Trio row can be added", false);
        return;
    }
    action->trigger();
    auto* address = dialog.findChild<QLineEdit*>(QStringLiteral("peripheralAddress_trio"));
    auto* connectButton = dialog.findChild<QPushButton*>(QStringLiteral("peripheralConnect_trio"));
    if (!address || !connectButton) {
        report("markup: the Trio page has address and Connect", false);
        return;
    }
    address->setText(QLatin1String(kHost));
    connectButton->click();
    QByteArray doc = stateDoc("radio2", "ant6", TrioTestPeer::kZeroId, "HL2", "<i>Dummy</i>");
    doc.replace(R"("n":"ANTENNA5")", R"("n":"<b>ANTENNA5</b>")");
    peer.advance(trio, 1);
    report("markup: held with a state carrying markup", peer.handshake(TrioTestPeer::textFrame(doc)));

    bool allPlain = true;
    bool literal = false;
    const QList<QLabel*> captions = dialog.findChildren<QLabel*>(QStringLiteral("trioPortCaption"));
    for (QLabel* caption : captions) {
        allPlain = allPlain && caption->textFormat() == Qt::PlainText;
        literal = literal || caption->text() == QStringLiteral("<b>ANTENNA5</b>");
    }
    report("markup: every port caption is plain text, the name shown literally",
           !captions.isEmpty() && allPlain && literal);

    auto* ant6Dummy = dialog.findChild<QCheckBox*>(QStringLiteral("peripheralTrioDummy_ant6"));
    auto* dummyWarning = dialog.findChild<QLabel*>(QStringLiteral("peripheralTrioDummyWarning"));
    auto* confirmDummy = dialog.findChild<QPushButton*>(QStringLiteral("peripheralTrioConfirmDummy"));
    auto* writeNames = dialog.findChild<QPushButton*>(QStringLiteral("peripheralTrioWriteNames"));
    auto* namesResult = dialog.findChild<QLabel*>(QStringLiteral("peripheralTrioNamesResult"));
    auto* ant6Name = dialog.findChild<QLineEdit*>(QStringLiteral("peripheralTrioName_antenna_ant6"));
    if (!ant6Dummy || !dummyWarning || !confirmDummy || !writeNames || !namesResult || !ant6Name) {
        report("markup: the port widgets exist", false);
        return;
    }
    ant6Dummy->click();
    peer.push(stateDoc("radio2", "ant6", TrioTestPeer::kZeroId, "HL2", "<h1>TX OK</h1>"));
    report("markup: the dummy-load warning is plain text, the label shown literally",
           dummyWarning->textFormat() == Qt::PlainText && dummyWarning->isVisibleTo(&dialog)
               && dummyWarning->text().contains(QStringLiteral("<h1>TX OK</h1>")),
           dummyWarning->text().toStdString());
    confirmDummy->click();

    // Renaming the dummy-load port: the real confirmation box, answered here.
    Qt::TextFormat boxFormat = Qt::AutoText;
    QString boxText;
    bool cancelIsDefault = false;
    bool boxSeen = false;
    QTimer::singleShot(0, [&]() {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!box) {
            return;
        }
        boxSeen = true;
        boxFormat = box->textFormat();
        boxText = box->text();
        cancelIsDefault = box->defaultButton() == box->button(QMessageBox::Cancel);
        box->button(QMessageBox::Cancel)->click();
    });
    QTimer::singleShot(3000, []() {   // never hang the suite on a missed box
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (auto* box = qobject_cast<QMessageBox*>(widget); box && box->isVisible()) {
                box->reject();
            }
        }
    });
    const int before = peer.transport.sent.size();
    ant6Name->setText(QStringLiteral("Load"));
    ant6Name->setModified(true);
    writeNames->click();
    report("markup: the rename question is a plain-text box, Cancel the default",
           boxSeen && boxFormat == Qt::PlainText && cancelIsDefault
               && boxText.contains(QStringLiteral("<h1>TX OK</h1>")),
           boxText.toStdString());
    report("markup: its Cancel sends nothing", peer.transport.sent.size() == before);
    report("markup: the names result is plain text, the label shown literally",
           namesResult->textFormat() == Qt::PlainText
               && namesResult->text().contains(QStringLiteral("<h1>TX OK</h1>")),
           namesResult->text().toStdString());
    MicrohamTrioSettings::remove();
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
    testDeviceTextIsPlain();
    testConfiguredRowIsListed();

    std::printf("\n%s (%d failed)\n", g_failed == 0 ? "ALL PASSED" : "FAILURES", g_failed);
    return g_failed == 0 ? 0 : 1;
}

#include "microham_trio_peripherals_test.moc"
