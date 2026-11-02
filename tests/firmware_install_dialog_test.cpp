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
#include <QTabWidget>
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

void settle()
{
    QTest::qWait(150);
}

// Each transition the reviewer of PR #6975 reproduced, checking that the
// window ends at the height its contents ask for.
void heightFollowsTheMaxPart()
{
    auto devices = std::make_shared<QList<Info>>(QList<Info>{kKnob});
    Ctr2ProxyModel relay;
    relay.setUsbBackend([devices] { return *devices; },
                        [](const Info&, QString*) { return static_cast<Ctr2HidPort*>(nullptr); });
    relay.setTransport(Ctr2ProxyModel::Transport::Usb);
    relay.setUsbDevicePath(kKnob.path);

    auto* dialog = new FirmwareInstallDialog(&relay);
    dialog->setAttribute(Qt::WA_DeleteOnClose, false);
    auto* fw = dialog->findChild<FirmwareInstallModel*>();
    auto* tabs = dialog->findChild<QTabWidget*>(QStringLiteral("firmwareInstallTabs"));
    check(fw && tabs, "the window holds its firmware model and tabs");
    if (!fw || !tabs) {
        delete dialog;
        return;
    }
    fw->setDeviceSources([] { return QList<FirmwareInstallModel::Loader>(); },
                         [devices] { return *devices; });
    dialog->show();
    settle();

    const auto fits = [dialog] { return dialog->height() == dialog->sizeHint().height(); };
    check(!tabs->isVisibleTo(dialog), "an AetherKnob alone hides the CTR2-Max part");
    check(fits(), "and the window opens at the relay's height");
    const int shortHeight = dialog->height();

    relay.setTransport(Ctr2ProxyModel::Transport::Wifi);
    settle();
    check(tabs->isVisibleTo(dialog), "Wi-Fi mode shows it");
    check(dialog->height() > shortHeight, "and the window grows for it");

    relay.setTransport(Ctr2ProxyModel::Transport::Usb);
    settle();
    check(!tabs->isVisibleTo(dialog), "back in USB mode it hides again");
    check(fits(), "and the window shrinks back to the relay's height");

    *devices = {kKnob, kMax};
    fw->lookAgain();
    settle();
    check(tabs->isVisibleTo(dialog), "a CTR2-Max plugged in beside the knob shows it");

    *devices = {kKnob};
    fw->lookAgain();
    settle();
    check(!tabs->isVisibleTo(dialog), "the CTR2-Max unplugged hides it");
    check(fits(), "and the window shrinks again");

    fw->setDeviceSources(
        [] {
            return QList<FirmwareInstallModel::Loader>{
                {QStringLiteral("ttyACM0"), QStringLiteral("444444444444")}};
        },
        [devices] { return *devices; });
    settle();
    check(tabs->isVisibleTo(dialog), "a CTR2-Max in download mode shows it");

    fw->setDeviceSources([] { return QList<FirmwareInstallModel::Loader>(); },
                         [devices] { return *devices; });
    settle();
    check(!tabs->isVisibleTo(dialog), "download mode left, it hides");
    check(fits(), "and the window shrinks again");

    delete dialog;
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

    return g_failures ? 1 : 0;
}
