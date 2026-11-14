// The CTR2 window (FirmwareInstallDialog) shows only the relay while another
// kind of controller is selected, and its height follows: it shrinks when the
// CTR2-Max part hides, every time, not only on the first show. Socket-free:
// the device lists are injected, no HID device is opened, and both firmware
// catalogs point at a local file that does not exist.

#include "Ctr2ProxyTestBackend.h"
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/Ctr2HidPort.h"
#include "core/Ctr2MaxStockCatalog.h"
#include "core/Ctr2RelaySettings.h"
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
    Ctr2ProxyModel relay{Ctr2ProxyTestBackend::inertUsb()};
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
        // The window builds its firmware model and polls it at once: with
        // this window's lists from the start, never the system's.
        FirmwareInstallModel::setDefaultDeviceSourcesForTesting(
            {[l = loaders] { return *l; }, [d = devices] { return *d; }});
        dialog = std::make_unique<FirmwareInstallDialog>(&relay);
        Ctr2ProxyTestBackend::installFirmwareSourceTripwire();
        dialog->setAttribute(Qt::WA_DeleteOnClose, false);
        fw = dialog->findChild<FirmwareInstallModel*>();
        tabs = dialog->findChild<QTabWidget*>(QStringLiteral("firmwareInstallTabs"));
        if (!fw || !tabs) {
            return false;
        }
        // This window's HID list too, in place of the relay's.
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

// A remembered controller that is not relaying offers Forget beside its Start;
// pressing it forgets the controller and the button goes. Never shown on a
// relaying row or one never remembered.
void forgetOnARememberedRow()
{
    Ctr2RelaySettings settings;
    settings.hasSetup = true;
    settings.usbDevices = {{kKnob.serial, kKnob.vendorId, kKnob.productId}};
    settings.save();
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
    if (!list || list->count() != 2) {
        check(false, "a row for each controller");
        return;
    }
    const auto child = [&](const Info& d, const char* name) -> QPushButton* {
        for (int i = 0; i < list->count(); ++i) {
            if (list->item(i)->data(Qt::UserRole).toString() == d.path)
                return list->itemWidget(list->item(i))->findChild<QPushButton*>(QLatin1String(name));
        }
        return nullptr;
    };
    QPushButton* forget = child(kKnob, "ctr2ProxyUsbForget");
    check(forget && forget->isVisibleTo(w.dialog.get()),
          "the remembered, idle knob offers Forget");
    check(forget && forget->accessibleName() == QStringLiteral("Forget AetherKnob"),
          "named for its controller");
    check(child(kMax, "ctr2ProxyUsbForget")
              && !child(kMax, "ctr2ProxyUsbForget")->isVisibleTo(w.dialog.get()),
          "a controller never remembered offers none");
    child(kKnob, "ctr2ProxyUsbStart")->click();
    check(w.relay.isRunning(kKnob.path) && !forget->isVisibleTo(w.dialog.get()),
          "while the knob relays, Forget is not offered");
    child(kKnob, "ctr2ProxyUsbStart")->click();
    check(!w.relay.isRunning(kKnob.path) && forget->isVisibleTo(w.dialog.get()),
          "stopped, it is offered again");
    forget->click();
    check(Ctr2RelaySettings::load().usbDevices.isEmpty(), "Forget forgets the knob, saved");
    check(!forget->isVisibleTo(w.dialog.get()), "and the button goes");
    w.relay.stop();
    Ctr2RelaySettings{}.save();
}

// A rescan that changes only part of the device list keeps the rows of the
// devices listed as before: the same widgets, so their focus and button
// state survive, and only the changed rows are added or removed, in the
// list's order.
void rowsSurviveAPartialRescan()
{
    const Info third{QStringLiteral("third"), 0x303A, 0x1001, {}, QStringLiteral("AETHER_KNOB"),
                     QStringLiteral("555555555555")};
    Window w;
    check(w.open(), "the window opens");
    if (!w.fw || !w.tabs) {
        return;
    }
    w.relay.setUsbBackend([d = w.devices] { return *d; },
                          [](const Info&, QString*) { return static_cast<Ctr2HidPort*>(new QuietPort); });
    w.relay.setUsbRadioSocketFactory([] { return new InertRadioSocket; });
    w.relay.setAetherRadio(QHostAddress(QStringLiteral("192.0.2.10")), 4992, QStringLiteral("FLEX"), {});
    *w.devices = {kKnob, third};
    w.relay.refreshDevices();
    QCoreApplication::processEvents();
    auto* list = w.dialog->findChild<QListWidget*>(QStringLiteral("ctr2ProxyUsbDevices"));
    if (!list || list->count() != 2) {
        check(false, "a row for each controller");
        return;
    }
    const auto itemOf = [&](const Info& d) -> QListWidgetItem* {
        for (int i = 0; i < list->count(); ++i) {
            if (list->item(i)->data(Qt::UserRole).toString() == d.path)
                return list->item(i);
        }
        return nullptr;
    };
    const auto rowOf = [&](const Info& d) -> QWidget* {
        QListWidgetItem* item = itemOf(d);
        return item ? list->itemWidget(item) : nullptr;
    };
    const auto startOf = [&](const Info& d) -> QPushButton* {
        QWidget* row = rowOf(d);
        return row ? row->findChild<QPushButton*>(QStringLiteral("ctr2ProxyUsbStart")) : nullptr;
    };
    QWidget* const knobRow = rowOf(kKnob);
    QPushButton* const knobStart = startOf(kKnob);
    knobStart->click();   // relaying: its button reads Stop
    check(w.relay.isRunning(kKnob.path) && knobStart->text() == QStringLiteral("Stop"),
          "the knob relays, its row offering Stop");
    w.dialog->activateWindow();
    knobStart->setFocus(Qt::TabFocusReason);
    QCoreApplication::processEvents();
    check(knobStart->hasFocus(), "the knob's Stop has the keyboard focus");

    // Another controller is plugged in, listed between the two.
    *w.devices = {kKnob, kMax, third};
    w.relay.refreshDevices();
    QCoreApplication::processEvents();
    check(list->count() == 3, "the new controller gets a row");
    check(rowOf(kKnob) == knobRow && startOf(kKnob) == knobStart,
          "the knob's row is the same widget, not made again");
    check(knobStart->text() == QStringLiteral("Stop") && knobStart->isEnabled(),
          "and its button still offers Stop");
    check(knobStart->hasFocus(), "and keeps the keyboard focus");
    check(list->row(itemOf(kKnob)) == 0 && list->row(itemOf(kMax)) == 1 && list->row(itemOf(third)) == 2,
          "the new row takes its place in the list's order");
    QWidget* const maxRow = rowOf(kMax);
    QWidget* const thirdRow = rowOf(third);

    // One unplugged: only its row goes.
    *w.devices = {kKnob, kMax};
    w.relay.refreshDevices();
    QCoreApplication::processEvents();
    check(list->count() == 2 && !itemOf(third), "the unplugged controller's row goes");
    check(rowOf(kKnob) == knobRow && rowOf(kMax) == maxRow, "and only that one");
    Q_UNUSED(thirdRow);

    // The same path listed as another device: that row is made again.
    Info switched = kMax;
    switched.product = QStringLiteral("ESP32S3_DEV");
    *w.devices = {kKnob, switched};
    w.relay.refreshDevices();
    QCoreApplication::processEvents();
    check(list->count() == 2 && rowOf(switched) && rowOf(switched) != maxRow,
          "a device listed differently at the same path gets a new row");
    check(rowOf(kKnob) == knobRow, "and the others keep theirs");
    w.relay.stop();
}

// A remembered controller that is not plugged in keeps a dimmed row: its
// name and identity, "Not connected", and only Forget. Plugged back in, that
// row becomes its live row, the same widget, at its place in the list, and
// the other row is kept. Forget on an unplugged row takes it off the saved
// list and the row goes. Socket-free: nothing is started.
void anUnpluggedRememberedRow()
{
    Ctr2RelaySettings settings;
    settings.hasSetup = true;
    settings.usbDevices = {{kKnob.serial, kKnob.vendorId, kKnob.productId, QStringLiteral("AetherKnob")}};
    settings.save();
    Window w;
    check(w.open(), "the window opens");
    if (!w.fw || !w.tabs) {
        return;
    }
    *w.devices = {kMax};
    w.relay.refreshDevices();
    QCoreApplication::processEvents();
    auto* list = w.dialog->findChild<QListWidget*>(QStringLiteral("ctr2ProxyUsbDevices"));
    if (!list || list->count() != 2) {
        check(false, "a row for the plugged-in controller and one for the unplugged knob");
        return;
    }
    const auto widgetAt = [&](int row) { return list->itemWidget(list->item(row)); };
    const auto labelsOf = [](QWidget* row) {
        QStringList texts;
        for (QLabel* l : row->findChildren<QLabel*>())
            texts << l->text();
        return texts;
    };
    QWidget* const absentRow = widgetAt(1);
    QWidget* const maxRow = widgetAt(0);
    QPushButton* const absentStart = absentRow->findChild<QPushButton*>(QStringLiteral("ctr2ProxyUsbStart"));
    QPushButton* const absentForget = absentRow->findChild<QPushButton*>(QStringLiteral("ctr2ProxyUsbForget"));
    check(list->item(0)->data(Qt::UserRole).toString() == kMax.path
              && list->item(1)->data(Qt::UserRole).toString().isEmpty(),
          "the unplugged knob's row comes after the device list, with no device path");
    check(labelsOf(absentRow).contains(QStringLiteral("AetherKnob"))
              && labelsOf(absentRow).contains(QStringLiteral("303a:1001 #333333333333"))
              && labelsOf(absentRow).contains(QStringLiteral("Not connected")),
          "it shows the knob's name, identity and Not connected");
    check(absentStart && !absentStart->isVisibleTo(w.dialog.get()), "it offers no Start");
    check(absentForget && absentForget->isVisibleTo(w.dialog.get())
              && absentForget->accessibleName() == QStringLiteral("Forget AetherKnob"),
          "only Forget, named for the knob");
    check(!(list->item(1)->flags() & Qt::ItemIsSelectable), "and it cannot become the focus");

    // Plugged back in, ahead of the CTR2-Max.
    *w.devices = {kKnob, kMax};
    w.relay.refreshDevices();
    QCoreApplication::processEvents();
    check(list->count() == 2, "plugged back in, the knob has one row");
    check(widgetAt(0) == absentRow && list->item(0)->data(Qt::UserRole).toString() == kKnob.path,
          "the unplugged row becomes its live row, the same widget, at its place");
    check(widgetAt(1) == maxRow, "and the other row is kept");
    check(absentStart->isVisibleTo(w.dialog.get()) && absentStart->text() == QStringLiteral("Start")
              && !labelsOf(absentRow).contains(QStringLiteral("Not connected")),
          "it offers Start, and is no longer Not connected");
    check(list->item(0)->flags() & Qt::ItemIsSelectable, "and can be selected again");

    // Unplugged again, then forgotten.
    *w.devices = {kMax};
    w.relay.refreshDevices();
    QCoreApplication::processEvents();
    QPushButton* forget = list->count() == 2
        ? widgetAt(1)->findChild<QPushButton*>(QStringLiteral("ctr2ProxyUsbForget"))
        : nullptr;
    check(forget && forget->isVisibleTo(w.dialog.get()), "unplugged again, it offers Forget");
    if (forget)
        forget->click();
    QCoreApplication::processEvents();
    check(Ctr2RelaySettings::load().usbDevices.isEmpty(), "Forget forgets the unplugged knob, saved");
    check(list->count() == 1 && widgetAt(0) == maxRow, "and its row goes, the other kept");
    w.relay.stop();
    Ctr2RelaySettings{}.save();
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile("firmware-install-dialog");
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    AppSettings::instance().load();
    // A model built without an injected USB backend would reach hidapi and
    // could open a real CTR2; it gets this counting tripwire instead.
    Ctr2ProxyTestBackend::installUsbTripwire();
    // Nor does a firmware model built without its own device lists enumerate
    // the real serial ports or HID devices: it gets a counting tripwire too.
    Ctr2ProxyTestBackend::installFirmwareSourceTripwire();

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
    forgetOnARememberedRow();
    rowsSurviveAPartialRescan();
    anUnpluggedRememberedRow();

    check(Ctr2ProxyTestBackend::usbTripwireCalls() == 0,
          "every relay model was built with an injected USB backend, never the default (hidapi)");
    check(Ctr2ProxyTestBackend::firmwareSourceTripwireCalls() == 0,
          "every firmware model polled only injected device lists, never the system's");
    return g_failures ? 1 : 0;
}
