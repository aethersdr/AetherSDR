// The CTR2 window (FirmwareInstallDialog) shows only the relay while another
// kind of controller is selected, and its height follows: it shrinks when the
// CTR2-Max part hides, every time, not only on the first show. Socket-free:
// the device lists are injected, no HID device is opened, and both firmware
// catalogs point at a local file that does not exist.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/Ctr2HidPort.h"
#include "core/Ctr2MaxStockCatalog.h"
#include "core/EspFirmwareCatalog.h"
#include "gui/FirmwareInstallDialog.h"
#include "models/Ctr2ProxyModel.h"
#include "models/FirmwareInstallModel.h"

#include <QApplication>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QTabWidget>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <cstdio>
#include <memory>

using namespace AetherSDR;

namespace {

int g_failures = 0;

void check(bool ok, const char* name)
{
    std::printf("[%s] %s\n", ok ? "OK" : "FAIL", name);
    g_failures += !ok;
}

using Info = Ctr2HidPort::DeviceInfo;

const Info kKnob{QStringLiteral("knob"), 0x303A, 0x1001, {}, QStringLiteral("AETHER_KNOB"),
                 QStringLiteral("333333333333")};
const Info kMax{QStringLiteral("max"), 0x303A, 0x1001, {}, QStringLiteral("AetherCTR2"),
                QStringLiteral("444444444444")};

// Waits for the window to settle into `done`; true once it has.
template <typename Done>
bool settles(Done done)
{
    return QTest::qWaitFor(done, 5000);
}

struct Window {
    std::shared_ptr<QList<Info>> devices = std::make_shared<QList<Info>>();
    std::shared_ptr<QList<FirmwareInstallModel::Loader>> loaders =
        std::make_shared<QList<FirmwareInstallModel::Loader>>();
    Ctr2ProxyModel relay;
    std::unique_ptr<FirmwareInstallDialog> dialog;
    FirmwareInstallModel* fw{nullptr};
    QTabWidget* tabs{nullptr};

    // Opens the window with only an AetherKnob, selected.
    bool open()
    {
        *devices = {kKnob};
        relay.setUsbBackend([d = devices] { return *d; },
                            [](const Info&, QString*) { return static_cast<Ctr2HidPort*>(nullptr); });
        relay.setTransport(Ctr2ProxyModel::Transport::Usb);
        relay.setUsbDevicePath(kKnob.path);
        dialog = std::make_unique<FirmwareInstallDialog>(&relay);
        dialog->setAttribute(Qt::WA_DeleteOnClose, false);
        fw = dialog->findChild<FirmwareInstallModel*>();
        tabs = dialog->findChild<QTabWidget*>(QStringLiteral("firmwareInstallTabs"));
        if (!fw || !tabs) {
            return false;
        }
        fw->setDeviceSources([l = loaders] { return *l; }, [d = devices] { return *d; });
        dialog->show();
        return QTest::qWaitForWindowExposed(dialog.get());
    }
    bool partShown() const { return tabs->isVisibleTo(dialog.get()); }
    bool fits() const { return dialog->height() == dialog->sizeHint().height(); }
    void lookAgain()
    {
        fw->lookAgain();
        QCoreApplication::processEvents();
    }
};

// The transitions the review of PR #6975 reproduced: each time the part hides,
// the window ends at the height its contents ask for.
void heightFollowsTheMaxPart()
{
    Window w;
    check(w.open(), "the window opens, holding its firmware model and tabs");
    if (!w.fw || !w.tabs) {
        return;
    }
    check(!w.partShown(), "an AetherKnob alone hides the CTR2-Max part");
    check(settles([&] { return w.fits(); }), "and the window opens at the relay's height");
    const int shortHeight = w.dialog->height();

    w.relay.setTransport(Ctr2ProxyModel::Transport::Wifi);
    check(w.partShown(), "Wi-Fi mode shows it");
    check(settles([&] { return w.dialog->height() > shortHeight; }),
          "and the window grows for it");

    w.relay.setTransport(Ctr2ProxyModel::Transport::Usb);
    check(!w.partShown(), "back in USB mode it hides again");
    check(settles([&] { return w.fits(); }), "and the window shrinks back to the relay's height");
}

// A CTR2-Max seen once keeps the part for as long as the window is open: one
// rebooting after an install or a restart leaves the lists, and the relay
// selects the knob meanwhile, but the result stays in view.
void aCtr2MaxSeenKeepsThePart()
{
    {
        Window w;
        check(w.open(), "the window opens");
        if (!w.fw || !w.tabs) {
            return;
        }
        *w.devices = {kKnob, kMax};
        w.relay.refreshDevices();
        w.relay.setUsbDevicePath(kMax.path);
        w.lookAgain();
        check(w.partShown(), "a CTR2-Max plugged in beside the knob, and selected, shows it");
        check(settles([&] { return w.dialog->height() >= w.dialog->sizeHint().height(); }),
              "and the window is tall enough for it");
        const int tall = w.dialog->height();
        *w.devices = {kKnob};
        w.relay.refreshDevices();
        w.lookAgain();
        check(w.relay.usbDevicePath() == kKnob.path,
              "the CTR2-Max gone (rebooting), the relay selects the knob");
        check(w.partShown(), "but the part stays");
        check(w.dialog->height() == tall, "and so does the window's height");
    }
    {
        Window w;
        check(w.open(), "the window opens again");
        if (!w.fw || !w.tabs) {
            return;
        }
        check(!w.partShown(), "reopened with the knob alone, the part is hidden");
        *w.loaders = {{QStringLiteral("ttyACM0"), QStringLiteral("444444444444")}};
        w.lookAgain();
        check(w.partShown(), "a CTR2-Max in download mode shows it");
        *w.loaders = {};
        w.lookAgain();
        check(w.partShown(), "and it stays after download mode is left");
    }
}

// A relay port that accepts writes and goes nowhere.
class QuietPort : public Ctr2HidPort {
public:
    bool isOpen() const override { return true; }
    void send(const std::vector<ctr2hid::Report>&) override {}
    void discardQueued() override {}
    void shutdown(const std::vector<ctr2hid::Report>&) override { deleteLater(); }
    QString description() const override { return QStringLiteral("Quiet CTR2"); }
};

// A radio socket that never connects, so nothing reaches the network.
class InertRadioSocket final : public QTcpSocket {
public:
    using QTcpSocket::connectToHost;
    void connectToHost(const QString&, quint16, OpenMode = ReadWrite,
                       NetworkLayerProtocol = AnyIPProtocol) override
    {
    }
};

// Several controllers: a row each, with its own Start/Stop; selecting a row
// moves the focus and the details below, and starts or stops nothing.
void aRowPerController()
{
    Window w;
    check(w.open(), "the window opens");
    if (!w.fw || !w.tabs) {
        return;
    }
    w.relay.setUsbBackend([d = w.devices] { return *d; },
                          [](const Info&, QString*) { return static_cast<Ctr2HidPort*>(new QuietPort); });
    w.relay.setUsbRadioSocketFactory([] { return new InertRadioSocket; });
    w.relay.setAetherRadio(QHostAddress(QStringLiteral("192.0.2.10")), 4992, QStringLiteral("FLEX"), {});
    *w.devices = {kKnob, kMax};
    w.relay.refreshDevices();
    QCoreApplication::processEvents();
    auto* list = w.dialog->findChild<QListWidget*>(QStringLiteral("ctr2ProxyUsbDevices"));
    check(list && list->count() == 2, "a row for each controller");
    if (!list || list->count() != 2) {
        return;
    }
    const auto rowOf = [&](const Info& d) -> QListWidgetItem* {
        for (int i = 0; i < list->count(); ++i) {
            if (list->item(i)->data(Qt::UserRole).toString() == d.path)
                return list->item(i);
        }
        return nullptr;
    };
    const auto button = [&](const Info& d) {
        return list->itemWidget(rowOf(d))->findChild<QPushButton*>(QStringLiteral("ctr2ProxyUsbStart"));
    };
    check(button(kKnob) && button(kMax) && button(kKnob)->text() == QStringLiteral("Start"),
          "each row has its own Start");
    button(kKnob)->click();
    button(kMax)->click();
    check(w.relay.isRunning(kKnob.path) && w.relay.isRunning(kMax.path),
          "pressing both starts both");
    check(button(kKnob)->text() == QStringLiteral("Stop")
              && button(kKnob)->accessibleName() == QStringLiteral("Stop relaying AetherKnob"),
          "a relaying row offers Stop, named for its controller");
    check(rowOf(kKnob)->data(Qt::AccessibleTextRole).toString().startsWith(QStringLiteral("AetherKnob, ")),
          "a screen reader hears the row's controller and state");

    list->setCurrentItem(rowOf(kKnob));
    check(w.relay.usbDevicePath() == kKnob.path, "selecting a row makes it the focus");
    auto* state = w.dialog->findChild<QLabel*>(QStringLiteral("ctr2ProxyState"));
    check(state && state->text().startsWith(QStringLiteral("AetherKnob: ")),
          "the details below name the selected controller");
    check(w.relay.isRunning(kMax.path), "and selecting stops nothing");

    button(kKnob)->click();
    check(!w.relay.isRunning(kKnob.path) && w.relay.isRunning(kMax.path),
          "a row's Stop stops only its controller");
    check(button(kKnob)->text() == QStringLiteral("Start"), "and it offers Start again");
    w.relay.stop();
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile("firmware-install-dialog");
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    AppSettings::instance().load();

    // Nothing is fetched and nothing reaches the real firmware or backup folders.
    QTemporaryDir folders;
    EspFirmwareCatalog::setCatalogUrlForTesting(
        QUrl::fromLocalFile(folders.filePath(QStringLiteral("no-catalog.json"))));
    Ctr2MaxStockCatalog::setTrustedKeysForTesting({});
    FirmwareInstallModel::setFirmwareFolderForTesting(folders.filePath(QStringLiteral("firmware")));
    FirmwareInstallModel::setBackupFolderForTesting(folders.filePath(QStringLiteral("backups")));

    heightFollowsTheMaxPart();
    aCtr2MaxSeenKeepsThePart();
    aRowPerController();

    return g_failures ? 1 : 0;
}
