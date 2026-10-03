#include "TestSettingsProfile.h"
#include "AutomationSensitiveLineEdit.h"
#include "core/TgxlConnection.h"
#include "core/PgxlConnection.h"
#include "core/VkampConnection.h"
#include "core/AcomConnection.h"
#include "core/SpeConnection.h"
#include "core/LpMeterConnection.h"
#include "core/AppSettings.h"
#include "core/PeripheralSettings.h"
#include "gui/RadioSetupDialog.h"
#include "gui/AntennaGeniusApplet.h"
#include "gui/ShackSwitchApplet.h"
#include "gui/PeripheralAuthStore.h"
#include "core/PeripheralRemovalGuard.h"
#include "gui/PeripheralAuthConnectFlow.h"
#include "models/AntennaGeniusModel.h"
#include "models/RadioModel.h"
#include "core/backends/TunerDelta.h"
#include "core/backends/AmpDelta.h"
#include "PeripheralAuthStoreFake.h"

#include <QApplication>
#include <QCheckBox>
#include <QPointer>
#include <QByteArray>
#include <QComboBox>
#include <QLabel>
#include <QScrollArea>
#include <QScrollBar>
#include <QLineEdit>
#include <QListWidget>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QMenu>
#include <QMetaObject>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTimer>
#include <QTreeWidget>
#include <cstdio>
#include <utility>
#include <memory>

namespace AetherSDR {
struct PeripheralConnectionTestAccess {
    template<typename Connection>
    static void injectConnect(Connection& connection, int& calls)
    {
        connection.m_connectTransport = [&calls](const QString&, quint16) { ++calls; };
    }
};
struct AntennaGeniusModelTestAccess {
    static void markConnected(AntennaGeniusModel& model)
    {
        model.m_connected = true;
    }
    static void setAuthWriter(AntennaGeniusModel& model)
    {
        model.m_authCommandWriter = [](const QByteArray&) {};
    }
};
}

using namespace AetherSDR;
using Attention = PeripheralDeviceStatus::Attention;

namespace {
// Presentation refreshes on the signals that change it. Tests poke it with a
// real one: the radio model's connection-state change.
void pokeRefresh(RadioModel& model)
{
    emit model.connectionStateChanged(model.isConnected());
}

// Remove stores no suppression of its own; the retired dismissal key never appears.
bool storesDismissal()
{
    return AppSettings::instance().value(QStringLiteral("Peripherals")).toString()
        .contains(QStringLiteral("DiscoveryDismissed"));
}

bool checkDiscoveredAuthRecovery(bool savedEmptyList, bool blockedBeforeOpening)
{
    AppSettings::instance().remove(QStringLiteral("Peripherals"));
    if (savedEmptyList) {
        PeripheralSettings::setVisibleDeviceIds({});
    }
    RadioModel model;
    TunerDelta tuner;
    tuner.ip = QStringLiteral("192.0.2.30");
    model.tunerModel().applyChanges(tuner);
    AmpDelta amplifier;
    amplifier.ip = QStringLiteral("192.0.2.31");
    amplifier.detectedModel = QStringLiteral("PowerGeniusXL");
    amplifier.handle = QStringLiteral("1");
    model.amplifier().applyChanges(amplifier);
    TgxlConnection tgxl;
    PgxlConnection pgxl;
    AntennaGeniusModel ag;
    QObject::connect(&tgxl, &TgxlConnection::authCodeRequired, &tgxl, [&tgxl](quint64 attempt) {
        tgxl.setAuthCodeForAttempt(attempt, {}, false);
    });
    QObject::connect(&pgxl, &PgxlConnection::authCodeRequired, &pgxl, [&pgxl](quint64 attempt) {
        pgxl.setAuthCodeForAttempt(attempt, {});
    });
    auto blockDevices = [&]() {
        // Inject the production handshake without opening a socket or vault.
        return QMetaObject::invokeMethod(&tgxl, "beginAttemptAt", Qt::DirectConnection,
                   Q_ARG(QString, QStringLiteral("192.0.2.30")), Q_ARG(quint16, 9010))
            && QMetaObject::invokeMethod(&tgxl, "processLine", Qt::DirectConnection,
                   Q_ARG(QString, QStringLiteral("V1.2.17 AUTH")))
            && QMetaObject::invokeMethod(&pgxl, "beginAttemptAt", Qt::DirectConnection,
                   Q_ARG(QString, QStringLiteral("192.0.2.31")), Q_ARG(quint16, 9008))
            && QMetaObject::invokeMethod(&pgxl, "processLine", Qt::DirectConnection,
                   Q_ARG(QString, QStringLiteral("V3.9.1 AUTH")))
            && tgxl.isAuthBlocked() && pgxl.isAuthBlocked();
    };
    if (blockedBeforeOpening && !blockDevices()) {
        return false;
    }
    RadioSetupDialog dialog(&model, nullptr, &tgxl, &pgxl, &ag);
    dialog.selectTab(QStringLiteral("Peripherals"));
    auto* list = dialog.findChild<QListWidget*>(QStringLiteral("peripheralDeviceList"));
    if (!list) {
        return false;
    }
    if (!blockedBeforeOpening && (list->count() != 0 || !blockDevices())) {
        return false;
    }
    pokeRefresh(model);
    if (list->count() != 2 || list->currentRow() < 0) {
        return false;
    }
    for (int row = 0; row < 2; ++row) {
        const QString id = row == 0 ? QStringLiteral("tgxl") : QStringLiteral("pgxl");
        list->setCurrentRow(row);
        auto* ip = dialog.findChild<QLineEdit*>(QStringLiteral("peripheralAddress_%1").arg(id));
        auto* port = dialog.findChild<QSpinBox*>(QStringLiteral("peripheralPort_%1").arg(id));
        auto* button = dialog.findChild<QPushButton*>(QStringLiteral("peripheralConnect_%1").arg(id));
        auto* clear = dialog.findChild<QPushButton*>(QStringLiteral("peripheralAuth_%1_6").arg(id));
        auto* details = dialog.findChild<QStackedWidget*>(QStringLiteral("peripheralDetailStack"));
        if (!ip || !port || !button || !clear || !details
            || ip->text() != (row == 0 ? QStringLiteral("192.0.2.30") : QStringLiteral("192.0.2.31"))
            || port->value() != (row == 0 ? 9010 : 9008)
            || !list->item(row)->text().contains(QStringLiteral("Needs attention"))
            || list->item(row)->data(Qt::AccessibleDescriptionRole).toString().isEmpty()
            || details->currentWidget()->objectName() != QStringLiteral("peripheralDetail_%1").arg(id)
            || !button->isEnabled() || button->text() != QStringLiteral("Connect")) {
            return false;
        }
        // Invalid input exercises the real button/persistence path without
        // opening a socket; retrying the discovered endpoint must stay automatic.
        auto* code = dialog.findChild<QLineEdit*>(QStringLiteral("peripheralAuth_%1_4").arg(id));
        if (!code) {
            return false;
        }
        code->setText(QStringLiteral("invalid code"));
        button->click();
        const QString ipKey = row == 0 ? QStringLiteral("TGXL_ManualIp") : QStringLiteral("PGXL_ManualIp");
        const QString portKey = row == 0 ? QStringLiteral("TGXL_ManualPort") : QStringLiteral("PGXL_ManualPort");
        if (AppSettings::instance().contains(ipKey) || AppSettings::instance().contains(portKey)) {
            return false;
        }
        bool attempted = false;
        connectPeripheralTarget(ip, ipKey, portKey, static_cast<quint16>(port->value()),
            [&](const QString& host, quint16 targetPort) {
                attempted = host == ip->text() && targetPort == port->value();
            });
        if (!attempted || AppSettings::instance().contains(ipKey)) {
            return false;
        }
        // Editing just the port is manual intent and must still persist.
        connectPeripheralTarget(ip, ipKey, portKey, static_cast<quint16>(port->value() + 1),
            [](const QString&, quint16) {});
        if (AppSettings::instance().value(ipKey).toString() != ip->text()
            || AppSettings::instance().value(portKey).toInt() != port->value() + 1) {
            return false;
        }
        AppSettings::instance().remove(ipKey);
        AppSettings::instance().remove(portKey);
        clear->click();
        QCoreApplication::processEvents();
    }
    const auto ids = PeripheralSettings::visibleDeviceIds();
    const bool recoveryOk = !tgxl.isAuthBlocked() && !pgxl.isAuthBlocked()
        && ids.has_value() == savedEmptyList && (!ids || ids->isEmpty())
        && !AppSettings::instance().contains(QStringLiteral("TGXL_ManualIp"))
        && !AppSettings::instance().contains(QStringLiteral("PGXL_ManualIp"));
    if (!recoveryOk) {
        return false;
    }
    auto* add = dialog.findChild<QPushButton*>(QStringLiteral("peripheralAddButton"));
    auto* remove = dialog.findChild<QPushButton*>(QStringLiteral("peripheralRemoveButton"));
    if (!add || !add->menu() || !remove) {
        return false;
    }
    // An unrelated Add/Remove must not persist either temporary recovery row.
    for (QAction* action : add->menu()->actions()) {
        if (action->data().toString() == QStringLiteral("ag")) {
            action->trigger();
        }
    }
    if (PeripheralSettings::visibleDeviceIds().value_or(QStringList{}) != QStringList{QStringLiteral("ag")}) {
        return false;
    }
    remove->click();
    QCoreApplication::processEvents();
    if (!PeripheralSettings::visibleDeviceIds().value_or(QStringList{}).isEmpty()) {
        return false;
    }
    // Explicit Add can promote a recovery row without duplicating it.
    for (QAction* action : add->menu()->actions()) {
        if (action->data().toString() == QStringLiteral("tgxl")) {
            if (!action->isEnabled()) {
                return false;
            }
            action->trigger();
        }
    }
    return list->count() == 2
        && PeripheralSettings::visibleDeviceIds().value_or(QStringList{}) == QStringList{QStringLiteral("tgxl")};
}

// Remove clears the row and its configuration and stores no suppression of its
// own; discovery that reports the device again brings the recovery row back.
bool checkRemovedDiscovery()
{
    for (const QString& id : {QStringLiteral("tgxl"), QStringLiteral("pgxl")}) {
        AppSettings::instance().remove(QStringLiteral("Peripherals"));
        PeripheralSettings::setVisibleDeviceIds({id});
        RadioModel model;
        TgxlConnection tgxl;
        PgxlConnection pgxl;
        RadioSetupDialog dialog(&model, nullptr, &tgxl, &pgxl);
        dialog.selectTab(QStringLiteral("Peripherals"));
        auto* remove = dialog.findChild<QPushButton*>(QStringLiteral("peripheralRemoveButton"));
        auto* list = dialog.findChild<QListWidget*>(QStringLiteral("peripheralDeviceList"));
        auto* add = dialog.findChild<QPushButton*>(QStringLiteral("peripheralAddButton"));
        if (!remove || !list || !add) {
            return false;
        }
        const QString host = QStringLiteral("192.0.2.55");
        const auto device = id == QStringLiteral("tgxl")
            ? PeripheralAuthStore::Device::Tgxl : PeripheralAuthStore::Device::Pgxl;
        const quint16 port = id == QStringLiteral("tgxl") ? 9010 : 9008;
        const QString endpoint = PeripheralAuthStore::configuredEndpoint(host, port);
        AppSettings::instance().setValue(
            id == QStringLiteral("tgxl") ? "TGXL_ManualIp" : "PGXL_ManualIp", host);
        PeripheralAuthStore::save(device, endpoint, QStringLiteral("saved-code"), &dialog);
        QCoreApplication::processEvents();
        FakePeripheralAuthStore::setNextClearResult(false);
        remove->click();
        QCoreApplication::processEvents();
        if (list->count() != 1) {
            return false;
        }
        remove->click();
        QCoreApplication::processEvents();
        if (list->count() != 0 || !PeripheralSettings::autoConnect(id)
            || AppSettings::instance().value(QStringLiteral("Peripherals")).toString()
                   .contains(QStringLiteral("DiscoveryDismissed"))) {
            return false;
        }
        // A blocked device the radio reports again brings its recovery row back.
        QObject* connection = id == QStringLiteral("tgxl")
            ? static_cast<QObject*>(&tgxl) : static_cast<QObject*>(&pgxl);
        if (!QMetaObject::invokeMethod(connection, "beginAttemptAt", Qt::DirectConnection,
                Q_ARG(QString, host), Q_ARG(quint16, port))
            || !QMetaObject::invokeMethod(connection, "processLine", Qt::DirectConnection,
                Q_ARG(QString, QStringLiteral("V1.2.17 AUTH")))
            || !QMetaObject::invokeMethod(connection, "onAuthTimeout", Qt::DirectConnection)
            || !QMetaObject::invokeMethod(connection, "onAuthTimeout", Qt::DirectConnection)
            || !QMetaObject::invokeMethod(connection, "onAuthTimeout", Qt::DirectConnection)) {
            return false;
        }
        pokeRefresh(model);
        RadioSetupDialog reopened(&model, nullptr, &tgxl, &pgxl);
        reopened.selectTab(QStringLiteral("Peripherals"));
        auto* reopenedList = reopened.findChild<QListWidget*>(QStringLiteral("peripheralDeviceList"));
        if (list->count() != 1 || !reopenedList || reopenedList->count() != 1
            || !PeripheralSettings::autoConnect(id)) {
            return false;
        }
    }
    AppSettings::instance().remove(QStringLiteral("Peripherals"));
    return true;
}

// Exercise the real Remove/close handlers with a held vault completion. No
// socket, firmware peer, or OS credential store is used.
bool checkPendingRemoval()
{
    for (const QString& id : {QStringLiteral("tgxl"), QStringLiteral("pgxl"), QStringLiteral("ag")}) {
        for (bool deletionSucceeds : {false, true}) {
            AppSettings& settings = AppSettings::instance();
            settings.remove("Peripherals");
            PeripheralSettings::setVisibleDeviceIds({id});
            const QString ipKey = id == "ag" ? "AG_ManualIp"
                : id == "tgxl" ? "TGXL_ManualIp" : "PGXL_ManualIp";
            const QString host = QStringLiteral("192.0.2.75");
            const quint16 port = id == "tgxl" ? 9010 : id == "pgxl" ? 9008 : 9007;
            settings.setValue(ipKey, host);
            RadioModel model;
            TgxlConnection tgxl;
            PgxlConnection pgxl;
            AntennaGeniusModel ag;
            int reconnects = 0;
            PeripheralConnectionTestAccess::injectConnect(tgxl, reconnects);
            PeripheralConnectionTestAccess::injectConnect(pgxl, reconnects);
            PeripheralConnectionTestAccess::injectConnect(ag, reconnects);
            const auto reconnect = [&]() {
                if (id == "tgxl") {
                    tgxl.connectToTgxl(host, port);
                } else if (id == "pgxl") {
                    pgxl.connectToPgxl(host, port);
                } else {
                    ag.connectToAddress(host, port);
                }
            };
            reconnect();
            if (reconnects != 1) {
                return false;
            }
            reconnects = 0;
            AntennaGeniusModelTestAccess::setAuthWriter(ag);
            QWidget owner;
            QPointer<RadioSetupDialog> dialog = new RadioSetupDialog(
                &model, nullptr, &tgxl, &pgxl, &ag, nullptr,
                nullptr, nullptr, nullptr, nullptr, &owner);
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->selectTab("Peripherals");
            dialog->show();
            QCoreApplication::processEvents();
            QObject* connection = id == "tgxl" ? static_cast<QObject*>(&tgxl)
                : id == "pgxl" ? static_cast<QObject*>(&pgxl) : static_cast<QObject*>(&ag);
            if (!QMetaObject::invokeMethod(connection, "beginAttemptAt", Qt::DirectConnection,
                    Q_ARG(QString, host), Q_ARG(quint16, port))) {
                return false;
            }
            if (id == "tgxl") {
                tgxl.setAuthCode("pending-code");
            } else if (id == "pgxl") {
                pgxl.setAuthCode("pending-code");
            } else {
                ag.setAuthCode("pending-code");
            }
            int accepted = 0;
            QObject::connect(&tgxl, &TgxlConnection::authCodeAccepted, &owner,
                             [&](const QString&) { ++accepted; });
            QObject::connect(&pgxl, &PgxlConnection::authCodeAccepted, &owner,
                             [&](const QString&) { ++accepted; });
            QObject::connect(&ag, &AntennaGeniusModel::authCodeAccepted, &owner,
                             [&](const QString&) { ++accepted; });
            const auto feed = [&](bool success) {
                return id == "ag"
                    ? QMetaObject::invokeMethod(connection, "processTcpBytes", Qt::DirectConnection,
                        Q_ARG(QByteArray, success ? QByteArray("R1|0|OK\r\n") : QByteArray("V4.0.22 AG AUTH\r\n")))
                    : QMetaObject::invokeMethod(connection, "processLine", Qt::DirectConnection,
                        Q_ARG(QString, success
                            ? (id == "pgxl" ? QStringLiteral("R1|0|Authorized") : QStringLiteral("R1|0|auth OK"))
                            : (id == "pgxl" ? QStringLiteral("V3.9.1 AUTH") : QStringLiteral("V1.2.17 AUTH"))));
            };
            if (!feed(false)) {
                return false;
            }
            auto* remove = dialog->findChild<QPushButton*>("peripheralRemoveButton");
            auto* notice = dialog->findChild<QLabel*>("peripheralRemovalNotice");
            auto* list = dialog->findChild<QListWidget*>("peripheralDeviceList");
            if (!remove || !notice || !list) {
                return false;
            }
            // A refused close must not run save-on-close edits either.
            auto* address = dialog->findChild<QLineEdit*>("peripheralAddress_" + id);
            if (!address) {
                return false;
            }
            address->setText(QString());
            address->setModified(true);
            FakePeripheralAuthStore::deferClear(true);
            FakePeripheralAuthStore::setNextClearResult(deletionSucceeds);
            remove->click();
            reconnect();
            if (id == "ag") {
                AgDeviceInfo discovered;
                discovered.ip = QHostAddress(host);
                discovered.port = port;
                ag.connectToDevice(discovered); // applet discovery path
                discovered.radioPorts = 1;
                discovered.name = QStringLiteral("ShackSwitch");
                ag.connectToDevice(discovered); // shared model's ShackSwitch path
                ag.connectToAddress(QHostAddress(host), port);
            }
            if (reconnects || storesDismissal()) {
                std::fprintf(stderr, "Pending removal allowed reconnect or persisted transient state\n");
                return false;
            }
            if (!feed(true) || accepted != 0 || remove->isEnabled()
                || notice->isHidden() || !notice->accessibleDescription().contains("Wait")) {
                std::fprintf(stderr, "Pending Remove accepted late AUTH or lacked its busy state (%s)\n", qPrintable(id));
                return false;
            }
            // Cover window-manager close and all QDialog done paths (including Escape).
            dialog->close();
            dialog->reject();
            dialog->accept();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            if (!dialog || !dialog->isVisible() || settings.value(ipKey, QString()).toString() != host) {
                std::fprintf(stderr, "Setup closed or saved edits before removal completed\n");
                return false;
            }
            FakePeripheralAuthStore::finishClear();
            QCoreApplication::processEvents();
            reconnect();
            if (reconnects != 1) {
                return false;
            }
            if (list->count() != (deletionSucceeds ? 0 : 1)
                || settings.value(ipKey, QString()).toString() != (deletionSucceeds ? QString() : host)
                || storesDismissal()
                || (!deletionSucceeds && !remove->isEnabled())
                || (deletionSucceeds && !remove->accessibleDescription().contains("Select"))) {
                std::fprintf(stderr, "Pending removal completion did not preserve success/failure semantics\n");
                return false;
            }
            if (!deletionSucceeds) {
                const QString failureNotice = notice->text();
                // The operation result must outlive later connection updates.
                QMetaObject::invokeMethod(connection, "connected", Qt::DirectConnection);
                QCoreApplication::processEvents();
                if (notice->isHidden() || notice->text() != failureNotice
                    || !failureNotice.contains("Retry Remove")
                    || !notice->accessibleDescription().contains("connection was stopped")) {
                    return false;
                }
            }
            dialog->close();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            if (dialog) {
                return false;
            }
            settings.remove(ipKey);
        }
    }
    AppSettings::instance().remove("Peripherals");
    return true;
}

bool checkRemovalTimeout()
{
    for (const QString& id : {QStringLiteral("tgxl"), QStringLiteral("pgxl"), QStringLiteral("ag")}) {
        for (bool success : {false, true}) {
            for (bool destroyDialog : {false, true}) {
                AppSettings& settings = AppSettings::instance();
                settings.remove("Peripherals");
                PeripheralSettings::setVisibleDeviceIds({id});
                const QString ipKey = id == "ag" ? "AG_ManualIp"
                    : id == "tgxl" ? "TGXL_ManualIp" : "PGXL_ManualIp";
                settings.setValue(ipKey, QStringLiteral("192.0.2.90"));
                RadioModel model;
                TgxlConnection tgxl;
                PgxlConnection pgxl;
                AntennaGeniusModel ag;
                AcomConnection acom;
                PeripheralSettings::setDeviceString("Acom", "ManualIp", "192.0.2.100");
                int connects = 0;
                PeripheralConnectionTestAccess::injectConnect(tgxl, connects);
                PeripheralConnectionTestAccess::injectConnect(pgxl, connects);
                PeripheralConnectionTestAccess::injectConnect(ag, connects);
                const auto reconnect = [&]() {
                    if (id == "tgxl") {
                        tgxl.connectToTgxl("192.0.2.91", 9010);
                    } else if (id == "pgxl") {
                        pgxl.connectToPgxl("192.0.2.91", 9008);
                    } else {
                        ag.connectToAddress("192.0.2.91", 9007);
                    }
                };
                auto dialog = std::make_unique<RadioSetupDialog>(
                    &model, nullptr, &tgxl, &pgxl, &ag, nullptr, &acom);
                dialog->selectTab("Peripherals");
                dialog->show();
                QCoreApplication::processEvents();
                auto* remove = dialog->findChild<QPushButton*>("peripheralRemoveButton");
                auto* address = dialog->findChild<QLineEdit*>("peripheralAddress_" + id);
                auto* notice = dialog->findChild<QLabel*>("peripheralRemovalNotice");
                if (!remove || !address || !notice) {
                    return false;
                }
                auto* acomField = dialog->findChild<QWidget*>("peripheralAddress_acom");
                QLineEdit* acomAddress = nullptr;
                for (QLineEdit* edit : acomField->findChildren<QLineEdit*>()) {
                    if (edit->text() == "192.0.2.100") {
                        acomAddress = edit;
                    }
                }
                if (!acomAddress) {
                    return false;
                }
                acomAddress->clear();
                acomAddress->setModified(true);
                address->clear();
                address->setModified(true);
                FakePeripheralAuthStore::deferClear(true);
                FakePeripheralAuthStore::setNextClearResult(success);
                remove->click();
                auto* deadline = dialog->findChild<QTimer*>("peripheralRemovalDeadline");
                if (!deadline || !deadline->isActive() || !deadline->isSingleShot()
                    || deadline->interval() != 15000) {
                    return false;
                }
                deadline->stop(); // Inject expiry without a wall-clock sleep.
                QMetaObject::invokeMethod(deadline, "timeout", Qt::DirectConnection);
                if (!notice->text().contains("unconfirmed")
                    || !notice->accessibleDescription().contains("unconfirmed")) {
                    return false;
                }
                reconnect();
                if (connects != 0 || !dialog->close() || dialog->isVisible()
                    || settings.value(ipKey).toString() != "192.0.2.90"
                    || !PeripheralSettings::deviceString("Acom", "ManualIp").isEmpty()) {
                    std::fprintf(stderr, "Removal timeout failed to bound close or retain state/lease\n");
                    return false;
                }
                if (destroyDialog) {
                    dialog.reset();
                }
                settings.setValue(ipKey, QStringLiteral("192.0.2.91"));
                FakePeripheralAuthStore::finishClear();
                QCoreApplication::processEvents();
                reconnect();
                if (connects != 1 || settings.value(ipKey).toString() != "192.0.2.91"
                    || storesDismissal()
                    || PeripheralSettings::visibleDeviceIds() != std::optional<QStringList>({id})) {
                    std::fprintf(stderr, "Late removal changed newer settings or retained lease\n");
                    return false;
                }
                settings.remove(ipKey);
            }
        }
    }
    return true;
}

bool checkSharedModelRemovalIsolation()
{
    for (bool removeAg : {false, true}) {
        AppSettings& settings = AppSettings::instance();
        settings.remove("Peripherals");
        const QString removed = removeAg ? "ag" : "shackswitch";
        PeripheralSettings::setVisibleDeviceIds({removed});
        RadioModel model;
        AntennaGeniusModel ag;
        int connects = 0;
        PeripheralConnectionTestAccess::injectConnect(ag, connects);
        AgDeviceInfo target;
        target.name = removeAg ? "ShackSwitch" : "Antenna Genius";
        target.host = QStringLiteral("192.0.2.92");
        target.port = 9007;
        ag.connectToDevice(target);
        AntennaGeniusModelTestAccess::markConnected(ag);
        RadioSetupDialog dialog(&model, nullptr, nullptr, nullptr, &ag);
        dialog.selectTab("Peripherals");
        auto* list = dialog.findChild<QListWidget*>("peripheralDeviceList");
        auto* remove = dialog.findChild<QPushButton*>("peripheralRemoveButton");
        if (!list || !remove) {
            return false;
        }
        for (int row = 0; row < list->count(); ++row) {
            if (list->item(row)->data(Qt::UserRole).toString() == removed) {
                list->setCurrentRow(row);
            }
        }
        remove->click();
        QCoreApplication::processEvents();
        if (!ag.isConnected() || ag.connectedDevice().name != target.name || connects != 1) {
            std::fprintf(stderr, "Remove disconnected the other shared-model device\n");
            return false;
        }
    }
    return true;
}

// A device that was never configured or connected has no discovery intent, so
// removing it must not dismiss radio discovery.
bool checkNeverConfiguredRemovalKeepsDiscovery()
{
    for (const QString& id : {QStringLiteral("tgxl"), QStringLiteral("pgxl")}) {
        AppSettings::instance().remove(QStringLiteral("Peripherals"));
        AppSettings::instance().remove(QStringLiteral("TGXL_ManualIp"));
        AppSettings::instance().remove(QStringLiteral("PGXL_ManualIp"));
        PeripheralSettings::setVisibleDeviceIds({});
        RadioModel model;
        TgxlConnection tgxl;
        PgxlConnection pgxl;
        RadioSetupDialog dialog(&model, nullptr, &tgxl, &pgxl);
        dialog.selectTab(QStringLiteral("Peripherals"));
        auto* remove = dialog.findChild<QPushButton*>(QStringLiteral("peripheralRemoveButton"));
        auto* list = dialog.findChild<QListWidget*>(QStringLiteral("peripheralDeviceList"));
        auto* add = dialog.findChild<QPushButton*>(QStringLiteral("peripheralAddButton"));
        if (!remove || !list || !add) {
            return false;
        }
        for (QAction* action : add->menu()->actions()) {
            if (action->data().toString() == id) {
                action->trigger();
            }
        }
        if (list->count() != 1) {
            return false;
        }
        list->setCurrentRow(0);
        remove->click();
        QCoreApplication::processEvents();
        if (list->count() != 0 || storesDismissal()) {
            std::fprintf(stderr, "Removing a never-configured %s dismissed discovery\n",
                         qPrintable(id));
            return false;
        }
    }
    return true;
}

// Remove announces the removed row so MainWindow can retire its applet button.
bool checkRemovalSignalAndPendingNotice()
{
    AppSettings& settings = AppSettings::instance();
    settings.remove("Peripherals");
    settings.remove("AG_ManualIp"); // A retained AG target would add a second row.
    PeripheralSettings::setVisibleDeviceIds({QStringLiteral("shackswitch")});
    settings.setValue("SS_ManualIp", "192.0.2.120");
    RadioModel radio;
    AntennaGeniusModel ag;
    RadioSetupDialog dialog(&radio, nullptr, nullptr, nullptr, &ag);
    dialog.selectTab("Peripherals");
    auto* remove = dialog.findChild<QPushButton*>("peripheralRemoveButton");
    auto* list = dialog.findChild<QListWidget*>("peripheralDeviceList");
    auto* notice = dialog.findChild<QLabel*>("peripheralRemovalNotice");
    if (!remove || !list || !notice || list->count() != 1) {
        return false;
    }
    list->setCurrentRow(0);
    QSignalSpy removed(&dialog, &RadioSetupDialog::peripheralRemoved);
    FakePeripheralAuthStore::deferClear(true);
    remove->click();
    auto* deadline = dialog.findChild<QTimer*>("peripheralRemovalDeadline");
    if (!deadline || !removed.isEmpty()) {
        return false;
    }
    deadline->stop();
    QMetaObject::invokeMethod(deadline, "timeout", Qt::DirectConnection);
    // A reopened Setup meets the lease the timed-out removal still holds.
    RadioSetupDialog second(&radio, nullptr, nullptr, nullptr, &ag);
    second.selectTab("Peripherals");
    auto* secondRemove = second.findChild<QPushButton*>("peripheralRemoveButton");
    auto* secondList = second.findChild<QListWidget*>("peripheralDeviceList");
    auto* secondNotice = second.findChild<QLabel*>("peripheralRemovalNotice");
    if (!secondRemove || !secondList || !secondNotice || secondList->count() != 1) {
        return false;
    }
    secondList->setCurrentRow(0);
    QSignalSpy secondRemoved(&second, &RadioSetupDialog::peripheralRemoved);
    secondRemove->click();
    const bool named = !secondNotice->isHidden() && secondNotice->text().contains("still pending")
        && secondNotice->text().contains("ShackSwitch");
    if (!named || settings.value("SS_ManualIp").toString() != "192.0.2.120") {
        std::fprintf(stderr, "Remove under a held lease returned silently\n");
        return false;
    }
    FakePeripheralAuthStore::finishClear();
    QCoreApplication::processEvents();
    // The timed-out removal's completion must not announce a removal it skipped.
    if (!removed.isEmpty() || !secondRemoved.isEmpty()) {
        return false;
    }
    secondRemove->click();
    QCoreApplication::processEvents();
    return secondRemoved.count() == 1
        && secondRemoved.first().first().toString() == QStringLiteral("shackswitch");
}

bool checkRemovalWithFailedVaultRead()
{
    for (bool unavailable : {true, false}) {
        AppSettings& settings = AppSettings::instance();
        settings.remove("Peripherals");
        PeripheralSettings::setVisibleDeviceIds({QStringLiteral("ag")});
        settings.setValue("AG_ManualIp", "192.0.2.112");
        const auto restoreSettings = qScopeGuard([&settings] { settings.remove("AG_ManualIp"); });
        RadioModel radio;
        AntennaGeniusModel ag;
        RadioSetupDialog dialog(&radio, nullptr, nullptr, nullptr, &ag);
        dialog.selectTab("Peripherals");
        auto* remove = dialog.findChild<QPushButton*>("peripheralRemoveButton");
        auto* list = dialog.findChild<QListWidget*>("peripheralDeviceList");
        auto* notice = dialog.findChild<QLabel*>("peripheralRemovalNotice");
        if (!remove || !list || !notice || list->count() != 1) {
            return false;
        }
        list->setCurrentRow(0);
        FakePeripheralAuthStore::setNextReadFailure(unavailable
            ? FakePeripheralAuthStore::ReadFailure::Unavailable
            : FakePeripheralAuthStore::ReadFailure::Denied);
        remove->click();
        QCoreApplication::processEvents();
        const bool removed = list->count() == 0 && !settings.contains("AG_ManualIp");
        if (unavailable
            ? !removed || notice->isHidden() || !notice->text().contains("deletion unconfirmed")
            : removed || list->count() != 1 || notice->isHidden()) {
            std::fprintf(stderr, "Shared removal with a failed vault read (unavailable=%d)\n", unavailable);
            return false;
        }
    }
    return true;
}

bool checkSharedCredentialRemoval()
{
    for (bool removeAg : {false, true}) {
        for (bool ownsCode : {false, true}) {
            AppSettings& settings = AppSettings::instance();
            settings.remove("Peripherals");
            const QString id = removeAg ? "ag" : "shackswitch";
            const QString ipKey = removeAg ? "AG_ManualIp" : "SS_ManualIp";
            PeripheralSettings::setVisibleDeviceIds({id});
            settings.setValue(ipKey, "192.0.2.110");
            const QString endpoint = PeripheralAuthStore::configuredEndpoint(
                ownsCode ? "192.0.2.110" : "192.0.2.111", 9007);
            PeripheralAuthStore::save(PeripheralAuthStore::Device::AntennaGenius,
                                      endpoint, "shared-code", qApp);
            RadioModel radio;
            AntennaGeniusModel ag;
            int connects = 0;
            PeripheralConnectionTestAccess::injectConnect(ag, connects);
            RadioSetupDialog dialog(&radio, nullptr, nullptr, nullptr, &ag);
            dialog.selectTab("Peripherals");
            auto* remove = dialog.findChild<QPushButton*>("peripheralRemoveButton");
            auto* list = dialog.findChild<QListWidget*>("peripheralDeviceList");
            for (int row = 0; row < list->count(); ++row) {
                if (list->item(row)->data(Qt::UserRole).toString() == id) {
                    list->setCurrentRow(row);
                }
            }
            FakePeripheralAuthStore::deferClear(true);
            remove->click();
            if (ownsCode) {
                ag.connectToAddress("192.0.2.110", 9007);
                if (connects || !PeripheralRemovalGuard::pending(PeripheralRemovalGuard::Device::AntennaGenius)) {
                    return false;
                }
            }
            FakePeripheralAuthStore::finishClear();
            QCoreApplication::processEvents();
            PeripheralAuthStore::LoadResult loaded;
            PeripheralAuthStore::load(PeripheralAuthStore::Device::AntennaGenius,
                endpoint, qApp, [&](const auto& result) { loaded = result; });
            QCoreApplication::processEvents();
            if ((!loaded.code.isEmpty()) == ownsCode || settings.contains(ipKey)
                || PeripheralSettings::visibleDeviceIds().value_or(QStringList()).contains(id)) {
                std::fprintf(stderr, "Shared credential ownership removal failed\n");
                return false;
            }
            PeripheralAuthStore::clear(PeripheralAuthStore::Device::AntennaGenius, qApp, [](auto) {});
            QCoreApplication::processEvents();
        }
    }
    return true;
}

bool checkRemovalOwnerTeardown()
{
    for (const QString& id : {QStringLiteral("tgxl"), QStringLiteral("pgxl")}) {
        for (bool success : {false, true}) {
            AppSettings& settings = AppSettings::instance();
            settings.remove("Peripherals");
            PeripheralSettings::setVisibleDeviceIds({id});
            const QString ipKey = id == "tgxl" ? "TGXL_ManualIp" : "PGXL_ManualIp";
            settings.setValue(ipKey, QStringLiteral("192.0.2.90"));
            settings.save();
            const auto device = id == "tgxl" ? PeripheralAuthStore::Device::Tgxl : PeripheralAuthStore::Device::Pgxl;
            const QString endpoint = PeripheralAuthStore::configuredEndpoint("192.0.2.90", id == "tgxl" ? 9010 : 9008);
            PeripheralAuthStore::save(device, endpoint, "saved-code", qApp);
            QCoreApplication::processEvents();
            RadioModel model;
            TgxlConnection tgxl;
            PgxlConnection pgxl;
            int reconnects = 0;
            PeripheralConnectionTestAccess::injectConnect(tgxl, reconnects);
            PeripheralConnectionTestAccess::injectConnect(pgxl, reconnects);
            const auto reconnect = [&]() {
                if (id == "tgxl") {
                    tgxl.connectToTgxl("192.0.2.90", 9010);
                } else {
                    pgxl.connectToPgxl("192.0.2.90", 9008);
                }
            };
            auto owner = std::make_unique<QWidget>();
            QPointer<RadioSetupDialog> dialog = new RadioSetupDialog(
                &model, nullptr, &tgxl, &pgxl, nullptr, nullptr,
                nullptr, nullptr, nullptr, nullptr, owner.get());
            dialog->selectTab("Peripherals");
            auto* remove = dialog->findChild<QPushButton*>("peripheralRemoveButton");
            if (!remove) {
                return false;
            }
            bool readDelivered = false;
            PeripheralAuthStore::load(device, endpoint, qApp, [&](const PeripheralAuthStore::LoadResult& result) {
                readDelivered = result.status == PeripheralAuthStore::LoadStatus::Missing && result.code.isEmpty();
            });
            FakePeripheralAuthStore::deferClear(true);
            FakePeripheralAuthStore::setNextClearResult(success);
            remove->click();
            owner.reset(); // Parent destruction bypasses closeEvent/done entirely.
            QCoreApplication::processEvents();
            settings.load(); // Read persisted data, not just the in-memory snapshot.
            if (dialog || !readDelivered || storesDismissal()
                || settings.value(ipKey).toString() != "192.0.2.90"
                || PeripheralSettings::visibleDeviceIds() != std::optional<QStringList>({id})) {
                return false;
            }
            reconnect();
            if (reconnects) {
                return false; // The vault, not the destroyed widget, owns the guard.
            }
            FakePeripheralAuthStore::finishClear();
            QCoreApplication::processEvents();
            reconnect();
            if (reconnects != 1 || storesDismissal()
                || settings.value(ipKey).toString() != "192.0.2.90") {
                return false;
            }
            settings.remove(ipKey);
        }
    }
    AppSettings::instance().remove("Peripherals");
    return true;
}

bool checkShackSwitchRetryDuringRemoval()
{
    for (bool deletionSucceeds : {false, true}) {
        for (bool cancelRetry : {false, true}) {
            AppSettings::instance().remove("Peripherals");
            PeripheralSettings::setVisibleDeviceIds({QStringLiteral("ag")});
            RadioModel radio;
            AntennaGeniusModel ag;
            ag.setAutoReconnect(true);
            int connections = 0;
            PeripheralConnectionTestAccess::injectConnect(ag, connections);
            AgDeviceInfo shackSwitch;
            shackSwitch.name = QStringLiteral("ShackSwitch");
            shackSwitch.host = QStringLiteral("shackswitch.example.invalid");
            shackSwitch.port = 9007;
            ag.connectToDevice(shackSwitch);
            AntennaGeniusModelTestAccess::markConnected(ag);
            if (!QMetaObject::invokeMethod(&ag, "onTcpDisconnected", Qt::DirectConnection)) {
                return false;
            }
            auto* retry = ag.findChild<QTimer*>("agReconnectTimer");
            if (!retry || !retry->isActive() || connections != 1) {
                return false;
            }
            RadioSetupDialog dialog(&radio, nullptr, nullptr, nullptr, &ag);
            dialog.selectTab("Peripherals");
            auto* remove = dialog.findChild<QPushButton*>("peripheralRemoveButton");
            if (!remove) {
                return false;
            }
            FakePeripheralAuthStore::deferClear(true);
            FakePeripheralAuthStore::setNextClearResult(deletionSucceeds);
            remove->click();
            // Deliver deterministic single-shot expirations, without waiting
            // five seconds or opening any socket. A real expiry is inactive
            // before it delivers timeout, so stop it before injecting the signal.
            for (int expiry = 0; expiry < 2; ++expiry) {
                retry->stop();
                QMetaObject::invokeMethod(retry, "timeout", Qt::DirectConnection);
                if (!retry->isActive() || retry->interval() != 5000 || connections != 1) {
                    std::fprintf(stderr, "AG removal consumed ShackSwitch retry\n");
                    return false;
                }
            }
            if (cancelRetry) {
                ag.disconnectFromDevice();
                if (retry->isActive()) {
                    return false;
                }
            }
            FakePeripheralAuthStore::finishClear();
            QCoreApplication::processEvents();
            if (retry->isActive() == cancelRetry || connections != 1) {
                return false;
            }
            if (!cancelRetry) {
                retry->stop();
                QMetaObject::invokeMethod(retry, "timeout", Qt::DirectConnection);
                if (connections != 2 || retry->isActive()) {
                    std::fprintf(stderr, "ShackSwitch retry did not resume after removal\n");
                    return false;
                }
            }
        }
    }
    AppSettings::instance().remove("Peripherals");
    return true;
}

bool checkOneShotShackSwitchDuringRemoval()
{
    for (bool deletionSucceeds : {false, true}) {
        for (bool discovered : {false, true}) {
            for (int outcome = 0; outcome < 3; ++outcome) {
                AppSettings::instance().remove("Peripherals");
                PeripheralSettings::setVisibleDeviceIds({QStringLiteral("ag")});
                RadioModel radio;
                AntennaGeniusModel ag;
                ag.setAutoReconnect(false); // A one-shot request is sufficient.
                int connections = 0;
                PeripheralConnectionTestAccess::injectConnect(ag, connections);
                AgDeviceInfo oldAg;
                oldAg.name = QStringLiteral("Antenna Genius");
                oldAg.host = QStringLiteral("old-ag.example.invalid");
                ag.connectToDevice(oldAg);
                AntennaGeniusModelTestAccess::markConnected(ag);
                RadioSetupDialog dialog(&radio, nullptr, nullptr, nullptr, &ag);
                dialog.selectTab("Peripherals");
                auto* remove = dialog.findChild<QPushButton*>("peripheralRemoveButton");
                auto* retry = ag.findChild<QTimer*>("agReconnectTimer");
                if (!remove || !retry) {
                    return false;
                }
                FakePeripheralAuthStore::deferClear(true);
                FakePeripheralAuthStore::setNextClearResult(deletionSucceeds);
                remove->click();
                AgDeviceInfo request;
                request.name = QStringLiteral("ShackSwitch");
                request.radioPorts = 1;
                request.port = 9007;
                if (discovered) {
                    request.ip = QHostAddress("192.0.2.97");
                } else {
                    request.host = QStringLiteral("switch.example.invalid");
                }
                ag.connectToDevice(request); // No existing retry or socket event.
                if (!retry->isActive() || connections != 1
                    || ag.connectedDevice().host != oldAg.host) {
                    std::fprintf(stderr, "One-shot ShackSwitch request was lost or changed live state\n");
                    return false;
                }
                for (int expiry = 0; expiry < 2; ++expiry) {
                    retry->stop();
                    QMetaObject::invokeMethod(retry, "timeout", Qt::DirectConnection);
                    if (!retry->isActive() || connections != 1) {
                        return false;
                    }
                }
                if (outcome == 1) {
                    ag.disconnectFromDevice();
                }
                FakePeripheralAuthStore::finishClear();
                QCoreApplication::processEvents();
                if (connections != 1 || retry->isActive() != (outcome != 1)) {
                    std::fprintf(stderr, "Removal completion lost or revived deferred intent\n");
                    return false;
                }
                if (outcome == 0) {
                    retry->stop();
                    QMetaObject::invokeMethod(retry, "timeout", Qt::DirectConnection);
                    if (connections != 2 || retry->isActive()
                        || ag.connectedDevice().host != request.host
                        || ag.connectedDevice().ip != request.ip) {
                        return false;
                    }
                } else if (outcome == 2) {
                    AgDeviceInfo newer = request;
                    newer.host = QStringLiteral("newer-switch.example.invalid");
                    ag.connectToDevice(newer);
                    if (connections != 2 || retry->isActive()
                        || ag.connectedDevice().host != newer.host) {
                        return false;
                    }
                }
            }
        }
    }
    AppSettings::instance().remove("Peripherals");
    return true;
}

bool checkRemovalDuringAuthRead()
{
    for (const QString& id : {QStringLiteral("tgxl"), QStringLiteral("pgxl"), QStringLiteral("ag")}) {
        AppSettings::instance().remove("Peripherals");
        PeripheralSettings::setVisibleDeviceIds({id});
        RadioModel model;
        TgxlConnection tgxl;
        PgxlConnection pgxl;
        AntennaGeniusModel ag;
        quint64 attempt = 0;
        int failures = 0;
        QObject::connect(&tgxl, &TgxlConnection::authCodeRequired, &tgxl,
                         [&](quint64 token) { attempt = token; });
        QObject::connect(&pgxl, &PgxlConnection::authCodeRequired, &pgxl,
                         [&](quint64 token) { attempt = token; });
        QObject::connect(&ag, &AntennaGeniusModel::authCodeRequired, &ag,
                         [&](quint64 token) { attempt = token; });
        QObject::connect(&tgxl, &TgxlConnection::connectionFailed, &tgxl,
                         [&](const QString&) { ++failures; });
        QObject::connect(&pgxl, &PgxlConnection::connectionFailed, &pgxl,
                         [&](const QString&) { ++failures; });
        QObject::connect(&ag, &AntennaGeniusModel::connectionError, &ag,
                         [&](const QString&) { ++failures; });
        RadioSetupDialog dialog(&model, nullptr, &tgxl, &pgxl, &ag);
        dialog.selectTab(QStringLiteral("Peripherals"));
        QObject* connection = id == "tgxl" ? static_cast<QObject*>(&tgxl)
            : id == "pgxl" ? static_cast<QObject*>(&pgxl) : static_cast<QObject*>(&ag);
        const QString host = QStringLiteral("192.0.2.70");
        const quint16 port = id == "tgxl" ? 9010 : id == "pgxl" ? 9008 : 9007;
        if (!QMetaObject::invokeMethod(connection, "beginAttemptAt", Qt::DirectConnection,
                Q_ARG(QString, host), Q_ARG(quint16, port))) {
            return false;
        }
        const bool fed = id == "ag"
            ? QMetaObject::invokeMethod(connection, "processTcpBytes", Qt::DirectConnection,
                Q_ARG(QByteArray, QByteArray("V4.0.22 AG AUTH\r\n")))
            : QMetaObject::invokeMethod(connection, "processLine", Qt::DirectConnection,
                Q_ARG(QString, QStringLiteral("V1.2.17 AUTH")));
        auto* remove = dialog.findChild<QPushButton*>(QStringLiteral("peripheralRemoveButton"));
        if (!fed || !attempt || failures || !remove) {
            return false;
        }
        // Leave the credential request unanswered while the asynchronous delete
        // completes. No OS vault or device socket is involved.
        remove->click();
        QCoreApplication::processEvents();
        if (id == "tgxl") {
            tgxl.setAuthCodeForAttempt(attempt, QStringLiteral("late-code"));
        } else if (id == "pgxl") {
            pgxl.setAuthCodeForAttempt(attempt, QStringLiteral("late-code"));
        } else {
            ag.setAuthCodeForAttempt(attempt, QStringLiteral("late-code"));
        }
        if (failures || tgxl.isAuthBlocked() || pgxl.isAuthBlocked()
            || ag.isAuthBlockedFor(host, port)) {
            return false;
        }
    }
    AppSettings::instance().remove("Peripherals");
    return true;
}

bool checkManualRecoveryProvenance()
{
    for (bool tuner : {true, false}) {
        for (bool matchingHost : {false, true}) {
            AppSettings::instance().remove("Peripherals");
            const QString id = tuner ? QStringLiteral("tgxl") : QStringLiteral("pgxl");
            const QString ipKey = tuner ? QStringLiteral("TGXL_ManualIp") : QStringLiteral("PGXL_ManualIp");
            const QString portKey = tuner ? QStringLiteral("TGXL_ManualPort") : QStringLiteral("PGXL_ManualPort");
            const QString host = QStringLiteral("192.0.2.80");
            const quint16 port = (tuner ? 9010 : 9008) + (matchingHost ? 10000 : 0);
            AppSettings::instance().setValue(ipKey, host);
            AppSettings::instance().setValue(portKey, port);
            RadioModel model;
            TunerDelta td;
            AmpDelta ad;
            ad.detectedModel = QStringLiteral("PowerGeniusXL");
            ad.handle = QStringLiteral("1");
            td.ip = ad.ip = matchingHost ? host : QStringLiteral("192.0.2.81");
            model.tunerModel().applyChanges(td);
            model.amplifier().applyChanges(ad);
            TgxlConnection tgxl;
            PgxlConnection pgxl;
            QObject* connection = tuner ? static_cast<QObject*>(&tgxl) : static_cast<QObject*>(&pgxl);
            QMetaObject::invokeMethod(connection, "beginAttemptAt", Qt::DirectConnection,
                Q_ARG(QString, host), Q_ARG(quint16, port));
            QMetaObject::invokeMethod(connection, "processLine", Qt::DirectConnection,
                Q_ARG(QString, QStringLiteral("V1.2.17 AUTH")));
            for (int i = 0; i < 3; ++i) {
                QMetaObject::invokeMethod(connection, "onAuthTimeout", Qt::DirectConnection);
            }
            {
                RadioSetupDialog dialog(&model, nullptr, &tgxl, &pgxl);
                dialog.selectTab(QStringLiteral("Peripherals"));
                auto* address = dialog.findChild<QLineEdit*>(QStringLiteral("peripheralAddress_%1").arg(id));
                auto* connect = dialog.findChild<QPushButton*>(QStringLiteral("peripheralConnect_%1").arg(id));
                if (!address || !connect) {
                    return false;
                }
                address->clear();
                connect->click(); // Clear saved target without contacting a device.
            }
            if (AppSettings::instance().contains(ipKey)) {
                return false;
            }
            RadioSetupDialog reopened(&model, nullptr, &tgxl, &pgxl);
            reopened.selectTab(QStringLiteral("Peripherals"));
            auto* address = reopened.findChild<QLineEdit*>(QStringLiteral("peripheralAddress_%1").arg(id));
            if (!address) {
                return false;
            }
            // The TGXL builder can prefill the radio address; select the old WAN
            // target explicitly when exercising recovery of that manual target.
            address->setText(host);
            connectPeripheralTarget(address, ipKey, portKey, port, [](const QString&, quint16) {});
            if (AppSettings::instance().value(ipKey).toString() != host
                || AppSettings::instance().value(portKey).toUInt() != port) {
                return false;
            }
            AppSettings::instance().remove(ipKey);
            AppSettings::instance().remove(portKey);
        }
    }
    AppSettings::instance().remove("Peripherals");
    return true;
}

bool checkExternalAgConfiguration()
{
    for (int scenario = 0; scenario < 5; ++scenario) {
        const bool beforeOpening = scenario == 0;
        AppSettings::instance().remove(QStringLiteral("Peripherals"));
        AppSettings::instance().remove(QStringLiteral("AG_ManualIp"));
        PeripheralSettings::setVisibleDeviceIds({});
        if (beforeOpening) {
            AppSettings::instance().setValue("AG_ManualIp", QStringLiteral("192.0.2.60"));
        }
        RadioModel model;
        AntennaGeniusModel ag;
        RadioSetupDialog dialog(&model, nullptr, nullptr, nullptr, &ag);
        dialog.selectTab(QStringLiteral("Peripherals"));
        if (!beforeOpening) {
            auto* prefill = dialog.findChild<QLineEdit*>(QStringLiteral("peripheralAddress_ag"));
            if (!prefill) {
                return false;
            }
            if (scenario >= 2) {
                prefill->setText(QStringLiteral("192.0.2.61")); // Untouched live-peer pre-fill.
            }
            if (scenario == 4) {
                auto* port = dialog.findChild<QSpinBox*>(QStringLiteral("peripheralPort_ag"));
                if (!port) {
                    return false;
                }
                port->setValue(19007);
            }
            if (scenario == 3) {
                prefill->selectAll();
                prefill->insert(QStringLiteral("192.0.2.62")); // Actual operator edit.
            }
            AppSettings::instance().setValue("AG_ManualIp", QStringLiteral("192.0.2.60"));
            pokeRefresh(model);
        }
        auto* list = dialog.findChild<QListWidget*>(QStringLiteral("peripheralDeviceList"));
        auto* address = dialog.findChild<QLineEdit*>(QStringLiteral("peripheralAddress_ag"));
        auto* remove = dialog.findChild<QPushButton*>(QStringLiteral("peripheralRemoveButton"));
        if (!list || list->count() != 1 || list->item(0)->data(Qt::UserRole) != QStringLiteral("ag")
            || !address || address->text() != (scenario == 3 ? QStringLiteral("192.0.2.62")
                : scenario == 4 ? QStringLiteral("192.0.2.61") : QStringLiteral("192.0.2.60")) || !remove) {
            return false;
        }
        remove->click();
        QCoreApplication::processEvents();
        if (list->count() != 0 || AppSettings::instance().contains("AG_ManualIp")) {
            return false;
        }
    }
    AppSettings::instance().remove(QStringLiteral("Peripherals"));
    return true;
}

bool checkAgCloseAfterExternalConfiguration()
{
    // Add, saved row, Remove/reconfigure, close before refresh, deliberate clear,
    // stale clear, and port-only edit with an externally configured address.
    for (int scenario = 0; scenario < 8; ++scenario) {
        AppSettings& settings = AppSettings::instance();
        settings.remove("Peripherals");
        settings.remove("AG_ManualIp");
        settings.remove("AG_ManualPort");
        PeripheralSettings::setVisibleDeviceIds(scenario == 0
            ? QStringList{} : QStringList{QStringLiteral("ag")});
        const QString oldHost = QStringLiteral("192.0.2.90");
        const QString newHost = QStringLiteral("192.0.2.91");
        if (scenario == 2 || scenario == 4 || scenario == 5 || scenario == 7) {
            settings.setValue("AG_ManualIp", oldHost);
            settings.setValue("AG_ManualPort", 19007);
        }
        RadioModel model;
        AntennaGeniusModel ag;
        RadioSetupDialog dialog(&model, nullptr, nullptr, nullptr, &ag);
        dialog.selectTab(QStringLiteral("Peripherals"));
        dialog.show();
        auto* address = dialog.findChild<QLineEdit*>(QStringLiteral("peripheralAddress_ag"));
        auto* port = dialog.findChild<QSpinBox*>(QStringLiteral("peripheralPort_ag"));
        auto* list = dialog.findChild<QListWidget*>(QStringLiteral("peripheralDeviceList"));
        if (!address || !port || !list) {
            return false;
        }
        if (scenario == 0) {
            auto* add = dialog.findChild<QPushButton*>(QStringLiteral("peripheralAddButton"));
            if (!add || !add->menu()) {
                return false;
            }
            for (QAction* action : add->menu()->actions()) {
                if (action->data().toString() == QStringLiteral("ag")) {
                    action->trigger();
                }
            }
        } else if (scenario == 2) {
            auto* remove = dialog.findChild<QPushButton*>(QStringLiteral("peripheralRemoveButton"));
            if (!remove) {
                return false;
            }
            remove->click();
            QCoreApplication::processEvents();
            if (!address->text().isEmpty() || port->value() != 9007) {
                return false;
            }
        } else if (scenario == 4 || scenario == 5) {
            address->selectAll();
            address->backspace(); // Deliberate user edit, unlike a programmatic reset.
            if (!address->isModified()) {
                return false;
            }
        } else if (scenario == 6) {
            port->setValue(29007); // Protect an in-progress port edit too.
        } else if (scenario == 7) {
            address->setText(QString()); // Programmatic clear carries no user intent.
        }
        if (scenario != 4 && scenario != 7) {
            settings.setValue("AG_ManualIp", newHost);
            settings.setValue("AG_ManualPort", 9007);
        }
        // Inject a connected LAN prologue; no socket or firmware peer is used.
        const QString connectedHost = (scenario == 4 || scenario == 7) ? oldHost : newHost;
        if (!QMetaObject::invokeMethod(&ag, "beginAttemptAt", Qt::DirectConnection,
                Q_ARG(QString, connectedHost), Q_ARG(quint16, quint16(9007)))
            || !QMetaObject::invokeMethod(&ag, "processTcpBytes", Qt::DirectConnection,
                Q_ARG(QByteArray, QByteArray("V4.0.22 AG\r\n"))) || !ag.isConnected()) {
            return false;
        }
        if (scenario != 3 && scenario != 7) {
            pokeRefresh(model);
        }
        if (scenario <= 2 && (address->text() != newHost || port->value() != 9007
                             || list->count() != 1)) {
            std::fprintf(stderr, "AG reconciliation failed for scenario %d\n", scenario);
            return false;
        }
        if (scenario == 1) {
            // Subsequent applet edits also refresh an already configured row.
            settings.setValue("AG_ManualIp", oldHost);
            settings.setValue("AG_ManualPort", 19007);
            pokeRefresh(model);
            if (address->text() != oldHost || port->value() != 19007) {
                return false;
            }
        }
        dialog.close(); // Exercise the production closeEvent/savers, not destruction.
        const bool cleared = !settings.contains("AG_ManualIp") && !settings.contains("AG_ManualPort");
        if (scenario == 4) {
            if (!cleared || ag.isConnected()) {
                std::fprintf(stderr, "Intentional AG clear was not saved on close\n");
                return false;
            }
        } else {
            const QString expected = (scenario == 1 || scenario == 7) ? oldHost : newHost;
            if (settings.value("AG_ManualIp").toString() != expected || !ag.isConnected()) {
                std::fprintf(stderr, "Closing Setup erased external AG configuration (scenario %d)\n", scenario);
                return false;
            }
        }
    }
    AppSettings::instance().remove("Peripherals");
    AppSettings::instance().remove("AG_ManualIp");
    AppSettings::instance().remove("AG_ManualPort");
    return true;
}

bool checkUnavailableKeychainRemoval()
{
    PeripheralSettings::setVisibleDeviceIds({QStringLiteral("tgxl")});
    AppSettings::instance().setValue(QStringLiteral("TGXL_ManualIp"), QStringLiteral("192.0.2.40"));
    RadioModel model;
    TgxlConnection tgxl;
    RadioSetupDialog dialog(&model, nullptr, &tgxl);
    dialog.selectTab(QStringLiteral("Peripherals"));
    auto* clear = dialog.findChild<QPushButton*>(QStringLiteral("peripheralAuth_tgxl_6"));
    auto* status = dialog.findChild<QLabel*>(QStringLiteral("peripheralStatus_tgxl"));
    auto* remove = dialog.findChild<QPushButton*>(QStringLiteral("peripheralRemoveButton"));
    auto* list = dialog.findChild<QListWidget*>(QStringLiteral("peripheralDeviceList"));
    auto* notice = dialog.findChild<QLabel*>(QStringLiteral("peripheralRemovalNotice"));
    if (!clear || !status || !remove || !list || !notice) {
        return false;
    }
    FakePeripheralAuthStore::setBackendAvailable(false);
    clear->click();
    QCoreApplication::processEvents();
    const bool clearExplained = status->text().contains(QStringLiteral("session code cleared"))
        && status->text().contains(QStringLiteral("deletion unconfirmed"));
    remove->click();
    QCoreApplication::processEvents();
    FakePeripheralAuthStore::setBackendAvailable(true);
    return clearExplained && list->count() == 0
        && !AppSettings::instance().contains(QStringLiteral("TGXL_ManualIp"))
        && PeripheralSettings::visibleDeviceIds().value_or(QStringList{}).isEmpty()
        && !notice->isHidden() && notice->text().contains(QStringLiteral("deletion unconfirmed"))
        && notice->accessibleDescription() == notice->text();
}

bool checkFieldDescriptions()
{
    PeripheralSettings::setVisibleDeviceIds({QStringLiteral("vkamp")});
    RadioModel model;
    VkampConnection vkamp;
    RadioSetupDialog dialog(&model, nullptr, nullptr, nullptr, nullptr, nullptr,
                            nullptr, nullptr, &vkamp);
    dialog.selectTab(QStringLiteral("Peripherals"));
    auto* address = dialog.findChild<QLineEdit*>(QStringLiteral("peripheralAddress_vkamp"));
    auto* port = dialog.findChild<QSpinBox*>(QStringLiteral("peripheralPort_vkamp"));
    if (!address || !port) {
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        pokeRefresh(model);
        if (address->accessibleDescription() != QStringLiteral("IP address or host name of the VK3AMP amplifier")
            || port->accessibleDescription() != QStringLiteral("TCP control port, 1 to 65535, default 5005")) {
            return false;
        }
    }
    return true;
}
} // namespace

// The per-device "Connect automatically" toggle: default on, stored under
// Peripherals.<id>.AutoConnect, reset by Remove, never changed by Connect.
bool checkConnectAutomaticallyToggle()
{
    for (const QString& id : {QStringLiteral("tgxl"), QStringLiteral("pgxl"),
                              QStringLiteral("ag"), QStringLiteral("shackswitch")}) {
        AppSettings::instance().remove(QStringLiteral("Peripherals"));
        AppSettings::instance().setValue("AG_ManualIp", QString());
        AppSettings::instance().setValue("SS_ManualIp", QString());
        PeripheralSettings::setVisibleDeviceIds({});
        RadioModel model;
        TgxlConnection tgxl;
        PgxlConnection pgxl;
        AntennaGeniusModel ag;
        RadioSetupDialog dialog(&model, nullptr, &tgxl, &pgxl, &ag);
        dialog.selectTab(QStringLiteral("Peripherals"));
        auto* list = dialog.findChild<QListWidget*>(QStringLiteral("peripheralDeviceList"));
        auto* add = dialog.findChild<QPushButton*>(QStringLiteral("peripheralAddButton"));
        auto* remove = dialog.findChild<QPushButton*>(QStringLiteral("peripheralRemoveButton"));
        if (!list || !add || !add->menu() || !remove) {
            return false;
        }
        const QString before = AppSettings::instance().value(QStringLiteral("Peripherals")).toString();
        for (QAction* action : add->menu()->actions()) {
            if (action->data().toString() == id) {
                action->trigger();
            }
        }
        // Add creates and selects the row and changes nothing else.
        auto* toggle = dialog.findChild<QCheckBox*>(QStringLiteral("peripheralAutoConnect_%1").arg(id));
        if (list->count() != 1 || list->currentRow() != 0 || !toggle || !toggle->isChecked()
            || toggle->text() != QStringLiteral("Connect automatically")
            || toggle->accessibleName().isEmpty() || toggle->accessibleDescription().isEmpty()
            || !PeripheralSettings::autoConnect(id) || storesDismissal()
            || AppSettings::instance().value(QStringLiteral("Peripherals")).toString().contains(
                   QStringLiteral("AutoConnect"))
            || AppSettings::instance().value(QStringLiteral("Peripherals")).toString().size()
                   != QString(before).replace(QStringLiteral("[]"),
                          QStringLiteral("[\"%1\"]").arg(id)).size()) {
            std::fprintf(stderr, "Add did more than create the row, or the toggle lacks its default (%s)\n",
                         qPrintable(id));
            return false;
        }
        toggle->setChecked(false);
        if (PeripheralSettings::autoConnect(id)) {
            return false;
        }
        for (const QString& other : {QStringLiteral("tgxl"), QStringLiteral("pgxl"),
                                     QStringLiteral("ag"), QStringLiteral("shackswitch")}) {
            if (other != id && !PeripheralSettings::autoConnect(other)) {
                return false; // The toggle is per device.
            }
        }
        // Remove asks first; Cancel changes nothing.
        QString asked;
        int confirmations = 0;
        RadioSetupDialog::setRemovalConfirmationHookForTest(
            [&](const QString&, const QString& text) { ++confirmations; asked = text; return false; });
        const auto restore = qScopeGuard([] {
            RadioSetupDialog::setRemovalConfirmationHookForTest(
                [](const QString&, const QString&) { return true; });
        });
        AppSettings::instance().setValue(id == "ag" ? "AG_ManualIp" : id == "shackswitch" ? "SS_ManualIp"
            : id == "tgxl" ? "TGXL_ManualIp" : "PGXL_ManualIp", QStringLiteral("192.0.2.77"));
        remove->click();
        QCoreApplication::processEvents();
        if (confirmations != 1 || list->count() != 1 || toggle->isChecked()
            || PeripheralSettings::autoConnect(id)
            || !asked.contains(QStringLiteral("Connect automatically"))
            || !asked.contains(QStringLiteral("connect again"))
            || remove->isEnabled() == false || !dialog.isEnabled()) {
            std::fprintf(stderr, "Cancel changed state or the confirmation lacks its text (%s)\n",
                         qPrintable(id));
            return false;
        }
        // OK removes the row and returns the toggle to its default, with no
        // suppression key left behind.
        RadioSetupDialog::setRemovalConfirmationHookForTest(
            [&](const QString&, const QString&) { ++confirmations; return true; });
        remove->click();
        QCoreApplication::processEvents();
        if (confirmations != 2 || list->count() != 0 || !PeripheralSettings::autoConnect(id)
            || !toggle->isChecked() || storesDismissal()
            || AppSettings::instance().value(QStringLiteral("Peripherals")).toString().contains(
                   QStringLiteral("AutoConnect"))) {
            std::fprintf(stderr, "Remove did not reset the toggle (%s)\n", qPrintable(id));
            return false;
        }
    }
    AppSettings::instance().remove(QStringLiteral("Peripherals"));
    return true;
}

// Off blocks automatic reconnect and discovery connects; an explicit Connect
// still connects and leaves the toggle alone.
bool checkConnectAutomaticallyGates()
{
    AppSettings::instance().remove(QStringLiteral("Peripherals"));
    const QString host = QStringLiteral("192.0.2.88");
    const QStringList allIds{QStringLiteral("tgxl"), QStringLiteral("pgxl"),
                             QStringLiteral("ag"), QStringLiteral("shackswitch")};
    // Every other device holds the opposite value, so a wrong key is caught.
    const auto setOnly = [&](const QString& id, bool on) {
        for (const QString& other : allIds) {
            PeripheralSettings::setAutoConnect(other, !on);
        }
        PeripheralSettings::setAutoConnect(id, on);
    };
    for (bool on : {false, true}) {
        for (const QString& id : {QStringLiteral("tgxl"), QStringLiteral("pgxl")}) {
            setOnly(id, on);
            TgxlConnection tgxl;
            PgxlConnection pgxl;
            int opened = 0;
            PeripheralConnectionTestAccess::injectConnect(tgxl, opened);
            PeripheralConnectionTestAccess::injectConnect(pgxl, opened);
            tgxl.setAutoReconnect(true);
            pgxl.setAutoReconnect(true);
            QObject* connection = id == "tgxl" ? static_cast<QObject*>(&tgxl) : static_cast<QObject*>(&pgxl);
            auto* retry = connection->findChild<QTimer*>(id + QStringLiteral("ReconnectTimer"));
            if (!retry) {
                return false;
            }
            if (id == "tgxl") {
                tgxl.autoConnectToTgxl(host, 9010);
            } else {
                pgxl.autoConnectToPgxl(host, 9008);
            }
            opened = 0;
            QMetaObject::invokeMethod(connection, "onError", Qt::DirectConnection,
                Q_ARG(QAbstractSocket::SocketError, QAbstractSocket::ConnectionRefusedError));
            retry->stop();
            QMetaObject::invokeMethod(retry, "timeout", Qt::DirectConnection);
            if (opened != (on ? 1 : 0)) {
                std::fprintf(stderr, "Reconnect ignored the toggle (%s, %d)\n", qPrintable(id), on);
                return false;
            }
            // Explicit Connect always connects and never writes the toggle.
            opened = 0;
            if (id == "tgxl") {
                tgxl.connectToTgxl(host, 9010);
            } else {
                pgxl.connectToPgxl(host, 9008);
            }
            retry->stop();
            if (opened != 1 || PeripheralSettings::autoConnect(id) != on) {
                return false;
            }
        }
        // The shared AG model follows the toggle of the device it is on.
        for (const QString& id : {QStringLiteral("ag"), QStringLiteral("shackswitch")}) {
            setOnly(id, on);
            AntennaGeniusModel ag;
            int opened = 0;
            PeripheralConnectionTestAccess::injectConnect(ag, opened);
            ag.setAutoReconnect(true);
            AgDeviceInfo info;
            info.ip = QHostAddress(host);
            info.port = 9007;
            info.name = id == "ag" ? QStringLiteral("Antenna Genius") : QStringLiteral("ShackSwitch");
            ag.connectToDevice(info);
            opened = 0;
            auto* retry = ag.findChild<QTimer*>(QStringLiteral("agReconnectTimer"));
            if (!retry) {
                return false;
            }
            retry->stop();
            QMetaObject::invokeMethod(retry, "timeout", Qt::DirectConnection);
            if (opened != (on ? 1 : 0)) {
                std::fprintf(stderr, "AG reconnect ignored the toggle (%s, %d)\n", qPrintable(id), on);
                return false;
            }
        }
        // Discovery connects honour the toggle: the AG applet for an AG, the
        // ShackSwitch applet for a ShackSwitch.
        for (const QString& id : {QStringLiteral("ag"), QStringLiteral("shackswitch")}) {
            setOnly(id, on);
            AntennaGeniusModel ag;
            int opened = 0;
            PeripheralConnectionTestAccess::injectConnect(ag, opened);
            AgDeviceInfo info;
            info.ip = QHostAddress(host);
            info.port = 9007;
            info.serial = QStringLiteral("serial-%1").arg(id);
            info.name = id == "ag" ? QStringLiteral("Antenna Genius") : QStringLiteral("ShackSwitch");
            AntennaGeniusApplet agApplet;
            ShackSwitchApplet ssApplet;
            agApplet.setModel(&ag);
            ssApplet.setModel(&ag);
            emit ag.deviceDiscovered(info);
            if (opened != (on ? 1 : 0)) {
                std::fprintf(stderr, "Discovery ignored the toggle (%s, %d)\n", qPrintable(id), on);
                return false;
            }
        }
    }
    AppSettings::instance().remove(QStringLiteral("Peripherals"));
    return true;
}

// What Setup shows follows the device's explicit status, never text, and the
// page is signal-driven (no presentation timer).
bool checkStatusPresentation()
{
    AppSettings::instance().remove(QStringLiteral("Peripherals"));
    PeripheralSettings::setVisibleDeviceIds({QStringLiteral("tgxl")});
    RadioModel model;
    TgxlConnection tgxl;
    PgxlConnection pgxl;
    RadioSetupDialog dialog(&model, nullptr, &tgxl, &pgxl);
    dialog.selectTab(QStringLiteral("Peripherals"));
    dialog.show();
    auto* list = dialog.findChild<QListWidget*>(QStringLiteral("peripheralDeviceList"));
    auto* status = dialog.findChild<QLabel*>(QStringLiteral("peripheralStatus_tgxl"));
    auto* add = dialog.findChild<QPushButton*>(QStringLiteral("peripheralAddButton"));
    if (!list || !status || !add || !add->menu() || list->count() != 1
        || dialog.findChild<QTimer*>(QStringLiteral("peripheralPresentationTimer"))) {
        return false;
    }
    // The detail status is always visible and carries the accessible state.
    if (!status->isVisible() || status->text() != QStringLiteral("Not connected")
        || status->accessibleName().isEmpty()
        || status->accessibleDescription() != status->text()
        || !list->item(0)->text().contains(QStringLiteral("OFFLINE"))
        || list->item(0)->text().contains(QStringLiteral("Needs attention"))) {
        std::fprintf(stderr, "Idle status not shown on the detail page\n");
        return false;
    }
    // A message that happens to start with "Error:" or hold a dash changes nothing
    // by itself; only the explicit attention does.
    dialog.editPeripheralStatusForTest(QStringLiteral("tgxl"), [](PeripheralDeviceStatus& state) {
        state.note = QStringLiteral("Error: looks like one — but is only a note");
    });
    pokeRefresh(model);
    if (list->item(0)->text().contains(QStringLiteral("Needs attention"))) {
        return false;
    }
    dialog.editPeripheralStatusForTest(QStringLiteral("tgxl"), [](PeripheralDeviceStatus& state) {
        state = PeripheralDeviceStatus{};
        state.attention = Attention::CredentialError;
        state.message = QStringLiteral("Stored code needs your attention");
    });
    const QString nameBefore = status->accessibleName();
    pokeRefresh(model);
    if (!list->item(0)->text().contains(QStringLiteral("Needs attention"))
        || status->text() != QStringLiteral("Stored code needs your attention")
        || status->accessibleName() == nameBefore
        || !status->accessibleName().contains(QStringLiteral("needs attention"))) {
        std::fprintf(stderr, "Attention was not carried to the list and detail status\n");
        return false;
    }
    // A disabled Add entry shows and speaks why.
    bool sawDisabled = false;
    for (QAction* action : add->menu()->actions()) {
        QMetaObject::invokeMethod(add->menu(), "aboutToShow");
        if (action->data().toString() == QStringLiteral("tgxl")) {
            sawDisabled = !action->isEnabled()
                && action->text().contains(QStringLiteral("already added"))
                && !action->statusTip().isEmpty();
        } else if (action->isEnabled() && action->text().contains(QStringLiteral("already added"))) {
            return false;
        }
    }
    AppSettings::instance().remove(QStringLiteral("Peripherals"));
    return sawDisabled;
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("peripheral-auth-dialog-test"));
    if (!profile.isValid()) {
        return 1;
    }
    QApplication app(argc, argv);
    AppSettings::instance().load();
    // Remove asks first; the lifecycle checks below confirm it. The confirmation
    // itself is covered by checkConnectAutomaticallyToggle.
    RadioSetupDialog::setRemovalConfirmationHookForTest(
        [](const QString&, const QString&) { return true; });
    if (!checkConnectAutomaticallyToggle() || !checkConnectAutomaticallyGates()
        || !checkStatusPresentation()) {
        std::fprintf(stderr, "Connect automatically regressed\n");
        return 1;
    }
    if (!checkSharedCredentialRemoval() || !checkRemovalWithFailedVaultRead() || !checkNeverConfiguredRemovalKeepsDiscovery() || !checkRemovalSignalAndPendingNotice() || !checkRemovalTimeout() || !checkSharedModelRemovalIsolation()
        || !checkPendingRemoval() || !checkRemovalOwnerTeardown()
        || !checkShackSwitchRetryDuringRemoval() || !checkOneShotShackSwitchDuringRemoval()) {
        std::fprintf(stderr, "Pending removal lifecycle regressed\n");
        return 1;
    }
    if (!checkRemovalDuringAuthRead()) {
        std::fprintf(stderr, "Removal during a pending auth read created a false failure\n");
        return 1;
    }
    if (!checkManualRecoveryProvenance()) {
        std::fprintf(stderr, "Manual recovery was mistaken for radio discovery\n");
        return 1;
    }
    if (!checkAgCloseAfterExternalConfiguration()) {
        std::fprintf(stderr, "AG external configuration/close regression\n");
        return 1;
    }
    if (!checkRemovedDiscovery() || !checkExternalAgConfiguration()) {
        std::fprintf(stderr, "Removed discovery or external AG configuration regressed\n");
        return 1;
    }
    if (!checkUnavailableKeychainRemoval() || !checkFieldDescriptions()) {
        std::fprintf(stderr, "Keychain-unavailable removal or field descriptions regressed\n");
        return 1;
    }
    for (bool savedEmptyList : {false, true}) {
        for (bool blockedBeforeOpening : {false, true}) {
            if (!checkDiscoveredAuthRecovery(savedEmptyList, blockedBeforeOpening)) {
                std::fprintf(stderr, "Blocked discovered devices lacked recovery controls (saved=%d, before=%d)\n",
                             savedEmptyList, blockedBeforeOpening);
                return 1;
            }
        }
    }
    AppSettings::instance().remove(QStringLiteral("Peripherals"));
    PeripheralSettings::setDeviceInt(QStringLiteral("Vkamp"), QStringLiteral("Variant"), 1);
    PeripheralSettings::setDeviceInt(QStringLiteral("Lp100a"), QStringLiteral("RangeHighW"), 3000);
    PeripheralSettings::setDeviceString(QStringLiteral("Vkamp"), QStringLiteral("ManualIp"),
                                        QStringLiteral("192.0.2.20"));
    PeripheralSettings::clearDeviceConnection(QStringLiteral("Vkamp"));
    PeripheralSettings::clearDeviceConnection(QStringLiteral("Lp100a"));
    if (PeripheralSettings::deviceInt(QStringLiteral("Vkamp"), QStringLiteral("Variant")) != 1
        || PeripheralSettings::deviceInt(QStringLiteral("Lp100a"),
                                         QStringLiteral("RangeHighW")) != 3000
        || !PeripheralSettings::deviceString(QStringLiteral("Vkamp"),
                                              QStringLiteral("ManualIp")).isEmpty()) {
        std::fprintf(stderr, "Removing connection settings changed device preferences\n");
        return 1;
    }
    RadioModel model;
    TgxlConnection tgxl;
    PgxlConnection pgxl;
    AntennaGeniusModel ag;
    AcomConnection acom;
    SpeConnection spe;
    VkampConnection vkamp;
    LpMeterConnection meter;
    RadioSetupDialog dialog(&model, nullptr, &tgxl, &pgxl, &ag, nullptr,
                            &acom, &spe, &vkamp, &meter);
    dialog.resize(960, 680);
    dialog.show();
    dialog.selectTab(QStringLiteral("Peripherals"));
    QCoreApplication::processEvents();

    auto* deviceList = dialog.findChild<QListWidget*>(QStringLiteral("peripheralDeviceList"));
    auto* addButton = dialog.findChild<QPushButton*>(QStringLiteral("peripheralAddButton"));
    auto* removeButton = dialog.findChild<QPushButton*>(QStringLiteral("peripheralRemoveButton"));
    if (!deviceList || !addButton || !addButton->menu() || !removeButton
        || deviceList->count() != 0 || removeButton->isEnabled()) {
        std::fprintf(stderr, "Peripheral list did not start empty with Add and Remove controls\n");
        return 1;
    }
    QAction* addTgxl = nullptr;
    for (QAction* action : addButton->menu()->actions()) {
        if (action->data().toString() == QStringLiteral("tgxl")) {
            addTgxl = action;
        }
    }
    if (!addTgxl) {
        std::fprintf(stderr, "TGXL was missing from Add device menu\n");
        return 1;
    }
    addTgxl->trigger();
    if (deviceList->count() != 1 || !removeButton->isEnabled()
        || !PeripheralSettings::visibleDeviceIds().value_or(QStringList{}).contains(QStringLiteral("tgxl"))) {
        std::fprintf(stderr, "Add did not select and persist TGXL\n");
        return 1;
    }

    // What Setup shows for a device, read from its explicit status.
    auto snap = [&dialog](const char* id) {
        return dialog.peripheralStatusForTest(QString::fromLatin1(id));
    };
    auto edit = [&dialog](const char* id, const std::function<void(PeripheralDeviceStatus&)>& change) {
        dialog.editPeripheralStatusForTest(QString::fromLatin1(id), change);
    };
    auto setPending = [&edit](const char* id, bool pending) {
        edit(id, [pending](PeripheralDeviceStatus& state) { state.pendingAuthCode = pending; });
    };
    QLineEdit* code = dialog.findChild<QLineEdit*>(QStringLiteral("peripheralAuth_tgxl_4"));
    QLabel* status = dialog.findChild<QLabel*>(QStringLiteral("peripheralStatus_tgxl"));
    QPushButton* show = dialog.findChild<QPushButton*>(QStringLiteral("peripheralAuth_tgxl_5"));
    QPushButton* connectButton = dialog.findChild<QPushButton*>(QStringLiteral("peripheralConnect_tgxl"));
    QPushButton* clearButton = dialog.findChild<QPushButton*>(QStringLiteral("peripheralAuth_tgxl_6"));
    QLineEdit* ip = dialog.findChild<QLineEdit*>(QStringLiteral("peripheralAddress_tgxl"));
    if (!code || !status || !show || !connectButton || !clearButton || !ip
        || !code->property("aetherSensitiveValue").toBool()) {
        std::fprintf(stderr, "TGXL credential controls missing or not sensitive\n");
        return 1;
    }
    auto fitsViewport = [](QWidget* widget) {
        for (QWidget* ancestor = widget->parentWidget(); ancestor; ancestor = ancestor->parentWidget()) {
            if (QScrollArea* scroll = qobject_cast<QScrollArea*>(ancestor)) {
                const QRect bounds(widget->mapTo(scroll->viewport(), QPoint()), widget->size());
                return scroll->horizontalScrollBar()->maximum() == 0
                    && bounds.left() >= 0 && bounds.right() < scroll->viewport()->width();
            }
        }
        return false;
    };
    QCoreApplication::processEvents();
    if (dialog.size() != QSize(960, 680) || !fitsViewport(clearButton) || !fitsViewport(status)) {
        std::fprintf(stderr, "Clear code or Status is clipped at the default dialog size\n");
        // The page's width depends on the host: each serial-port combo is as
        // wide as its longest "port — description" label. Name the widest one.
        for (QWidget* ancestor = clearButton->parentWidget(); ancestor; ancestor = ancestor->parentWidget()) {
            if (QScrollArea* scroll = qobject_cast<QScrollArea*>(ancestor)) {
                std::fprintf(stderr, "  dialog %dx%d, viewport %d, page minimum %d, horizontal scroll max %d\n",
                             dialog.width(), dialog.height(), scroll->viewport()->width(),
                             scroll->widget()->minimumSizeHint().width(),
                             scroll->horizontalScrollBar()->maximum());
                break;
            }
        }
        for (QComboBox* combo : dialog.findChildren<QComboBox*>()) {
            if (combo->findData(QStringLiteral("__custom__")) < 0) {
                continue;
            }
            QString widest;
            for (int i = 0; i < combo->count(); ++i) {
                if (combo->itemText(i).size() > widest.size()) {
                    widest = combo->itemText(i);
                }
            }
            std::fprintf(stderr, "  serial combo %s: minimum %d, widest item \"%s\"\n",
                         combo->isVisibleTo(&dialog) ? "(shown)" : "(hidden)",
                         combo->minimumSizeHint().width(), widest.toUtf8().constData());
        }
        return 1;
    }
    code->setText(QStringLiteral("sample"));
    show->click();
    if (code->echoMode() != QLineEdit::Normal
        || automationLineEditValue(code) != QStringLiteral("<hidden>")) {
        std::fprintf(stderr, "Show exposed the code to automation\n");
        return 1;
    }

    // The invalid-code path rejects the click before opening a socket.
    // The credential store linked here is an in-memory test adapter.
    ip->setText(QStringLiteral("192.0.2.10"));
    code->setText(QStringLiteral("bad code"));
    connectButton->click();
    if (snap("tgxl").attention != PeripheralDeviceStatus::Attention::InvalidCode
        || !status->text().contains(QStringLiteral("authorization code"), Qt::CaseInsensitive)
        || tgxl.isConnecting()) {
        std::fprintf(stderr, "Connect button did not reject invalid code\n");
        return 1;
    }
    const QString endpoint = PeripheralAuthStore::endpoint(QStringLiteral("192.0.2.10"), QStringLiteral("192.0.2.10"), 9010);
    // The dialog names a row's saved code from what is typed, before any
    // connection. That must be the identity the connection saves under, for a
    // literal address and for a name, or Clear and the status would miss it.
    if (PeripheralAuthStore::configuredEndpoint(QStringLiteral("192.0.2.10"), 9010) != endpoint
        || PeripheralAuthStore::configuredEndpoint(QStringLiteral(" Home.Example.Net "), 9010)
               != PeripheralAuthStore::endpoint(QStringLiteral("home.example.net"),
                                                QStringLiteral("198.51.100.7"), 9010)
        || !PeripheralAuthStore::configuredEndpoint(QString(), 9010).isEmpty()
        || !PeripheralAuthStore::configuredEndpoint(QStringLiteral("192.0.2.10"), 0).isEmpty()) {
        std::fprintf(stderr, "configuredEndpoint disagrees with endpoint\n");
        return 1;
    }
    PeripheralAuthStore::save(PeripheralAuthStore::Device::Tgxl, endpoint,
                              QStringLiteral("session-code"), &app);
    clearButton->click();
    QCoreApplication::processEvents();
    PeripheralAuthStore::LoadResult afterClear;
    PeripheralAuthStore::load(PeripheralAuthStore::Device::Tgxl, endpoint, &app,
        [&afterClear](const PeripheralAuthStore::LoadResult& result) { afterClear = result; });
    QCoreApplication::processEvents();
    if (afterClear.status != PeripheralAuthStore::LoadStatus::Missing || !code->text().isEmpty()) {
        std::fprintf(stderr, "Clear code button did not clear saved credential\n");
        return 1;
    }

    // A denied delete must leave a visible, actionable error in the dialog.
    PeripheralAuthStore::save(PeripheralAuthStore::Device::Tgxl, endpoint,
                              QStringLiteral("session-code"), &app);
    FakePeripheralAuthStore::setNextClearResult(false);
    clearButton->click();
    QCoreApplication::processEvents();
    if (snap("tgxl").attention != PeripheralDeviceStatus::Attention::CredentialError
        || !status->text().contains(QStringLiteral("saved code remains in keychain"), Qt::CaseInsensitive)) {
        std::fprintf(stderr, "Clear code failure was not shown\n");
        return 1;
    }

    pokeRefresh(model);
    QCoreApplication::processEvents();
    if (!status->isVisible() || !fitsViewport(clearButton)
        || !fitsViewport(status) || !status->wordWrap()) {
        std::fprintf(stderr, "Authorization error requires horizontal scrolling\n");
        return 1;
    }

    // Show retrieves the endpoint-bound saved value only on explicit intent.
    show->click();
    QCoreApplication::processEvents();
    if (code->text() != QStringLiteral("session-code")
        || code->echoMode() != QLineEdit::Normal || show->text() != QStringLiteral("Hide")
        || automationLineEditValue(code).contains(QStringLiteral("session-code"))) {
        std::fprintf(stderr, "Show did not reveal the saved code safely\n");
        return 1;
    }
    show->click();
    if (!code->text().isEmpty() || code->echoMode() != QLineEdit::Password) {
        std::fprintf(stderr, "Hide retained the revealed vault value\n");
        return 1;
    }
    show->click();
    ip->setText(QStringLiteral("192.0.2.11"));
    QCoreApplication::processEvents();
    if (!code->text().isEmpty() || code->echoMode() != QLineEdit::Password) {
        std::fprintf(stderr, "A delayed Show reply followed an address change\n");
        return 1;
    }
    ip->setText(QStringLiteral("192.0.2.10"));
    show->click();
    QCoreApplication::processEvents();
    QString replayedCode = QStringLiteral("not-called");
    bool connectCalled = false;
    PeripheralDeviceStatus shownCodeStatus;
    connectPeripheralWithCode(code, shownCodeStatus, QStringLiteral("192.0.2.10"), 9010,
        [&connectCalled](const QString&, quint16) { connectCalled = true; },
        [&replayedCode](const QString& value) { replayedCode = value; });
    if (!connectCalled || !replayedCode.isEmpty() || !code->text().isEmpty()
        || shownCodeStatus.pendingAuthCode) {
        std::fprintf(stderr, "Showing a saved code converted it to a new connection credential\n");
        return 1;
    }

    // Simulate the state reached after a newly entered code meets a LAN
    // greeting without AUTH. The status must say why the code was not saved.
    setPending("tgxl", true);
    tgxl.setAuthCode(QStringLiteral("sample"));
    if (!QMetaObject::invokeMethod(&tgxl, "processLine", Qt::DirectConnection,
                                   Q_ARG(QString, QStringLiteral("V1.2.17")))) {
        return 1;
    }
    if (!status->text().contains(QStringLiteral("code not saved"))
        || snap("tgxl").pendingAuthCode) {
        std::fprintf(stderr, "unchallenged connection did not explain unsaved code\n");
        return 1;
    }

    pokeRefresh(model);
    if (ip->isEnabled() || !ip->accessibleDescription().contains(QStringLiteral("Disconnect"))
        || !deviceList->item(0)->text().contains(QStringLiteral("DIRECT"))) {
        std::fprintf(stderr, "Connected device did not lock its address or update its list status\n");
        return 1;
    }

    // Recovery retires the prior error; a subsequent disconnect must show
    // the actual offline state instead of leaving "Connected" on screen.
    tgxl.connectionFailed(QStringLiteral("temporary connection failure"));
    if (snap("tgxl").attention != Attention::ConnectError
        || !deviceList->item(0)->text().contains(QStringLiteral("Needs attention"))) {
        std::fprintf(stderr, "connection failure was not marked\n");
        return 1;
    }
    tgxl.connected();
    if (snap("tgxl").needsAttention() || status->text() != QStringLiteral("Connected")) {
        std::fprintf(stderr, "recovery retained a stale error\n");
        return 1;
    }
    tgxl.disconnect();
    if (status->text() != QStringLiteral("Not connected")) {
        std::fprintf(stderr, "disconnect left a stale connected status\n");
        return 1;
    }

    pokeRefresh(model);
    if (!ip->isEnabled() || !deviceList->item(0)->text().contains(QStringLiteral("OFFLINE"))) {
        std::fprintf(stderr, "Disconnected device did not unlock its address or update its list status\n");
        return 1;
    }
    ip->setText(QStringLiteral("192.0.2.10"));
    pokeRefresh(model);
    if (code->placeholderText() != QStringLiteral("****") || !code->text().isEmpty()) {
        std::fprintf(stderr, "Cached credential availability was not shown\n");
        return 1;
    }
    ip->setText(QStringLiteral("192.0.2.11"));
    pokeRefresh(model);
    if (code->placeholderText() != QStringLiteral("Code blank")) {
        std::fprintf(stderr, "Credential availability followed the wrong endpoint\n");
        return 1;
    }
    QTreeWidget* navigation = dialog.findChild<QTreeWidget*>(QStringLiteral("radioSetupNavigation"));
    QPushButton* help = dialog.findChild<QPushButton*>(QStringLiteral("peripheralConnectionHelp"));
    if (!navigation || !help || help->mapTo(&dialog, help->rect().bottomLeft()).y()
        > navigation->mapTo(&dialog, navigation->rect().bottomLeft()).y()) {
        std::fprintf(stderr, "Peripheral content extends below the navigation tree\n");
        return 1;
    }

    // A rejected typed code cannot make a later authenticated connection
    // look like a connection that never requested authentication.
    setPending("tgxl", true);
    tgxl.connectionFailed(QStringLiteral("temporary socket failure"));
    if (!snap("tgxl").pendingAuthCode) {
        std::fprintf(stderr, "transient failure discarded pending code state\n");
        return 1;
    }
    if (!QMetaObject::invokeMethod(&tgxl, "beginAttempt", Qt::DirectConnection)) {
        return 1;
    }
    tgxl.setAuthCode(QStringLiteral("rejected-code"));
    if (!QMetaObject::invokeMethod(&tgxl, "processLine", Qt::DirectConnection,
                                   Q_ARG(QString, QStringLiteral("V1.2.17 AUTH")))
        || !QMetaObject::invokeMethod(&tgxl, "processLine", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("R1|0|Unauthorized")))) {
        return 1;
    }
    if (snap("tgxl").pendingAuthCode) {
        std::fprintf(stderr, "failed attempt retained pending code state\n");
        return 1;
    }
    if (!QMetaObject::invokeMethod(&tgxl, "processLine", Qt::DirectConnection,
                                   Q_ARG(QString, QStringLiteral("V1.2.17")))) {
        return 1;
    }
    if (status->text().contains(QStringLiteral("did not request authentication"))) {
        std::fprintf(stderr, "later connection inherited stale code note\n");
        return 1;
    }
    QLabel* pgxlStatus = dialog.findChild<QLabel*>(QStringLiteral("peripheralStatus_pgxl"));
    if (!pgxlStatus) {
        return 1;
    }
    setPending("pgxl", true);
    pgxl.setAuthCode(QStringLiteral("rejected-code"));
    if (!QMetaObject::invokeMethod(&pgxl, "processLine", Qt::DirectConnection,
                                   Q_ARG(QString, QStringLiteral("V3.9.1 AUTH")))
        || !QMetaObject::invokeMethod(&pgxl, "processLine", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("R1|FF|Denied")))) {
        return 1;
    }
    if (snap("pgxl").pendingAuthCode) {
        std::fprintf(stderr, "PGXL failure retained pending code state\n");
        return 1;
    }
    // Disconnecting before an AUTH reply discards an entered code. A later
    // greeting must not claim that this old code was merely unchallenged.
    for (const auto& item : {std::pair<QLabel*, QObject*>{status, &tgxl},
                             std::pair<QLabel*, QObject*>{pgxlStatus, &pgxl}}) {
        const char* const id = item.second == &tgxl ? "tgxl" : "pgxl";
        edit(id, [](PeripheralDeviceStatus& state) {
            state = PeripheralDeviceStatus{};
            state.pendingAuthCode = true;
        });
        if (item.second == &tgxl) {
            tgxl.setAuthCode(QStringLiteral("interrupted-code"));
            tgxl.disconnect();
        } else {
            pgxl.setAuthCode(QStringLiteral("interrupted-code"));
            pgxl.disconnect();
        }
        if (snap(id).pendingAuthCode || !snap(id).discardedAuthCode
            || !item.first->text().contains(QStringLiteral("discarded before verification"))) {
            std::fprintf(stderr, "discarded TGXL/PGXL code was not explained: %s pending=%d discarded=%d\n",
                         item.first->text().toUtf8().constData(),
                         snap(id).pendingAuthCode, snap(id).discardedAuthCode);
            return 1;
        }
    }

    // Set only the model's current-attempt metadata, without opening a socket.
    // The same model backs both rows and retains this metadata on TCP failure.
    AgDeviceInfo& attempt = const_cast<AgDeviceInfo&>(ag.connectedDevice());
    QLabel* agStatus = dialog.findChild<QLabel*>(QStringLiteral("peripheralStatus_ag"));
    QLabel* shackSwitchStatus = dialog.findChild<QLabel*>(QStringLiteral("peripheralStatus_shackswitch"));
    if (!agStatus || !shackSwitchStatus) {
        std::fprintf(stderr, "Antenna Genius or ShackSwitch status row missing\n");
        return 1;
    }
    attempt.name = QStringLiteral("ShackSwitch");
    ag.connectionError(QStringLiteral("ShackSwitch connection failed"));
    if (!shackSwitchStatus->text().contains(QStringLiteral("ShackSwitch connection failed"))
        || agStatus->text().contains(QStringLiteral("ShackSwitch connection failed"))) {
        std::fprintf(stderr, "ShackSwitch error appeared on the Antenna Genius row\n");
        return 1;
    }
    attempt.name = QStringLiteral("Antenna Genius");
    setPending("ag", true);
    if (!QMetaObject::invokeMethod(&ag, "beginAttemptAt", Qt::DirectConnection,
                                   Q_ARG(QString, QStringLiteral("192.0.2.10")), Q_ARG(quint16, 9007))) {
        return 1;
    }
    ag.setAuthCode(QStringLiteral("rejected-code"));
    if (!QMetaObject::invokeMethod(&ag, "processTcpBytes", Qt::DirectConnection,
                                   Q_ARG(QByteArray, QByteArray("V4.0.22 AG AUTH\r\n")))) {
        return 1;
    }
    if (!agStatus->text().contains(QStringLiteral("Connection closed before authorization command"))
        || shackSwitchStatus->text().contains(QStringLiteral("Connection closed before authorization command"))
        || snap("ag").pendingAuthCode) {
        std::fprintf(stderr, "Antenna Genius error routing or pending code state failed\n");
        return 1;
    }
    edit("ag", [](PeripheralDeviceStatus& state) {
        state = PeripheralDeviceStatus{};
        state.pendingAuthCode = true;
    });
    if (!QMetaObject::invokeMethod(&ag, "beginAttemptAt", Qt::DirectConnection,
                                   Q_ARG(QString, QStringLiteral("192.0.2.10")), Q_ARG(quint16, 9007))) {
        return 1;
    }
    ag.setAuthCode(QStringLiteral("interrupted-ag-code"));
    if (!QMetaObject::invokeMethod(&ag, "beginAttemptAt", Qt::DirectConnection,
                                   Q_ARG(QString, QStringLiteral("192.0.2.11")), Q_ARG(quint16, 9007))
        || snap("ag").pendingAuthCode || !snap("ag").discardedAuthCode
        || !agStatus->text().contains(QStringLiteral("discarded before verification"))) {
        std::fprintf(stderr, "discarded AG code was not explained\n");
        return 1;
    }

    // Clearing AG's saved credential must leave the shared model's current
    // ShackSwitch auth block intact.
    attempt.name = QStringLiteral("ShackSwitch");
    if (!QMetaObject::invokeMethod(&ag, "beginAttemptAt", Qt::DirectConnection,
                                   Q_ARG(QString, QStringLiteral("192.0.2.11")), Q_ARG(quint16, 9007))) {
        return 1;
    }
    ag.setAuthCode(QStringLiteral("rejected-switch-code"));
    if (!QMetaObject::invokeMethod(&ag, "processTcpBytes", Qt::DirectConnection,
                                   Q_ARG(QByteArray, QByteArray("V4.0.22 AG AUTH\r\n")))
        || !ag.isAuthBlockedFor(QStringLiteral("192.0.2.11"), 9007)) {
        std::fprintf(stderr, "ShackSwitch block setup failed\n");
        return 1;
    }
    QPushButton* clearAg = dialog.findChild<QPushButton*>(QStringLiteral("peripheralAuth_ag_6"));
    if (!clearAg) {
        return 1;
    }
    clearAg->click();
    QCoreApplication::processEvents();
    if (!ag.isAuthBlockedFor(QStringLiteral("192.0.2.11"), 9007)) {
        std::fprintf(stderr, "AG Clear code reset ShackSwitch auth block\n");
        return 1;
    }

    // Inject a synchronous target-switch discard at the same seam used by
    // each Connect row. This pins ordering without opening a TCP socket.
    QLineEdit replacement;
    PeripheralDeviceStatus replacementStatus;
    replacement.setText(QStringLiteral("replacement-code"));
    replacementStatus.pendingAuthCode = true;
    QString appliedCode;
    bool stalePendingObserved = false;
    connectPeripheralWithCode(&replacement, replacementStatus,
        QStringLiteral("192.0.2.11"), 9010,
        [&replacementStatus, &stalePendingObserved](const QString&, quint16) {
            stalePendingObserved = replacementStatus.pendingAuthCode;
            if (stalePendingObserved) {
                replacementStatus.discardedAuthCode = true;
            }
        },
        [&appliedCode](const QString& value) { appliedCode = value; });
    if (stalePendingObserved || !replacementStatus.pendingAuthCode
        || replacementStatus.discardedAuthCode
        || appliedCode != QStringLiteral("replacement-code")) {
        std::fprintf(stderr, "replacement code was discarded during target switch\n");
        return 1;
    }

    // Remove first refuses a failed Keychain deletion, then clears the saved
    // target and list entry. Re-adding it starts with an empty address.
    FakePeripheralAuthStore::setNextClearResult(false);
    removeButton->click();
    QCoreApplication::processEvents();
    if (deviceList->count() != 1 || snap("tgxl").attention != Attention::CredentialError
        || !status->text().contains(QStringLiteral("saved code remains in keychain"), Qt::CaseInsensitive)) {
        std::fprintf(stderr, "Remove hid a device after Keychain deletion failed\n");
        return 1;
    }
    removeButton->click();
    QCoreApplication::processEvents();
    if (deviceList->count() != 0
        || !AppSettings::instance().value(QStringLiteral("TGXL_ManualIp"), QString()).toString().isEmpty()
        || PeripheralSettings::visibleDeviceIds().value_or(QStringList{}).contains(QStringLiteral("tgxl"))) {
        std::fprintf(stderr, "Remove retained the TGXL connection configuration\n");
        return 1;
    }
    addTgxl->trigger();
    if (deviceList->count() != 1 || !ip->text().isEmpty()) {
        std::fprintf(stderr, "Re-adding a removed device restored a stale address: count=%d address=%s\n",
                     deviceList->count(), ip->text().toUtf8().constData());
        return 1;
    }
    for (const QString& id : {QStringLiteral("pgxl"), QStringLiteral("ag"),
                              QStringLiteral("shackswitch")}) {
        QAction* actionToAdd = nullptr;
        for (QAction* action : addButton->menu()->actions()) {
            if (action->data().toString() == id) {
                actionToAdd = action;
                break;
            }
        }
        if (!actionToAdd) {
            std::fprintf(stderr, "A device type was missing from Add\n");
            return 1;
        }
        actionToAdd->trigger();
    }
    QStackedWidget* details = dialog.findChild<QStackedWidget*>(
        QStringLiteral("peripheralDetailStack"));
    if (deviceList->count() != 4 || !details
        || details->currentWidget()->objectName() != QStringLiteral("peripheralDetail_shackswitch")) {
        std::fprintf(stderr, "Add did not select the matching device detail page\n");
        return 1;
    }
    removeButton->click();
    QCoreApplication::processEvents();
    if (deviceList->count() != 3
        || PeripheralSettings::visibleDeviceIds().value_or(QStringList{}).contains(
            QStringLiteral("shackswitch"))) {
        std::fprintf(stderr, "Remove retained the selected ShackSwitch entry\n");
        return 1;
    }
    RadioSetupDialog reopened(&model, nullptr, &tgxl, &pgxl, &ag);
    reopened.selectTab(QStringLiteral("Peripherals"));
    QListWidget* restoredList = reopened.findChild<QListWidget*>(
        QStringLiteral("peripheralDeviceList"));
    if (!restoredList || restoredList->count() != 3
        || restoredList->item(0)->data(Qt::UserRole).toString() != QStringLiteral("tgxl")
        || restoredList->item(1)->data(Qt::UserRole).toString() != QStringLiteral("pgxl")
        || restoredList->item(2)->data(Qt::UserRole).toString() != QStringLiteral("ag")) {
        std::fprintf(stderr, "Added and removed devices did not survive reopening Setup\n");
        return 1;
    }
    if (qEnvironmentVariableIsSet("AETHER_PERIPHERAL_SCREENSHOT")) {
        QCoreApplication::processEvents();
        dialog.grab().save(qEnvironmentVariable("AETHER_PERIPHERAL_SCREENSHOT"));
    }
    return 0;
}
